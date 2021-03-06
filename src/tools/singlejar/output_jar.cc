// Copyright 2016 The Bazel Authors. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/*
 * The implementation of the OutputJar methods.
 */
#include "src/tools/singlejar/output_jar.h"

#include <err.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#if defined(__linux)
#include <sys/sendfile.h>
#endif
#include <sys/stat.h>
#include <sys/times.h>
#include <unistd.h>

#include "src/main/cpp/util/file.h"
#include "src/tools/singlejar/combiners.h"
#include "src/tools/singlejar/diag.h"
#include "src/tools/singlejar/input_jar.h"
#include "src/tools/singlejar/mapped_file.h"
#include "src/tools/singlejar/options.h"
#include "src/tools/singlejar/zip_headers.h"

#include <zlib.h>

#define TODO(cond, msg)                                              \
  if (!(cond)) {                                                     \
    diag_errx(2, "%s:%d: TODO(asmundak): " msg, __FILE__, __LINE__); \
  }

OutputJar::OutputJar()
    : options_(nullptr),
      fd_(-1),
      entries_(0),
      duplicate_entries_(0),
      cen_(nullptr),
      cen_size_(0),
      cen_capacity_(0),
      spring_handlers_("META-INF/spring.handlers"),
      spring_schemas_("META-INF/spring.schemas"),
      protobuf_meta_handler_("protobuf.meta"),
      manifest_("META-INF/MANIFEST.MF"),
      build_properties_("build-data.properties") {
  known_members_.emplace(spring_handlers_.filename(),
                         EntryInfo{&spring_handlers_});
  known_members_.emplace(spring_schemas_.filename(),
                         EntryInfo{&spring_schemas_});
  known_members_.emplace(manifest_.filename(), EntryInfo{&manifest_});
  known_members_.emplace(protobuf_meta_handler_.filename(),
                         EntryInfo{&protobuf_meta_handler_});
  known_members_.emplace(build_properties_.filename(),
                         EntryInfo{&build_properties_});
  manifest_.Append(
      "Manifest-Version: 1.0\r\n"
      "Created-By: singlejar\r\n");
}

int OutputJar::Doit(Options *options) {
  if (nullptr != options_) {
    diag_errx(1, "%s:%d: Doit() can be called only once.", __FILE__, __LINE__);
  }
  options_ = options;

  // TODO(asmundak): handle these options.
  TODO(!options_->force_compression, "Handle --compression");
  TODO(!options_->normalize_timestamps, "Handle --normalize");
  TODO(!options_->preserve_compression, "Handle --dont_change_compression");
  TODO(!options_->include_prefixes.size(), "Handle --include_prefixes");

  build_properties_.AddProperty("build.target", options_->output_jar.c_str());
  if (options_->verbose) {
    fprintf(stderr, "combined_file_name=%s\n", options_->output_jar.c_str());
    if (!options_->main_class.empty()) {
      fprintf(stderr, "main_class=%s\n", options_->main_class.c_str());
    }
    if (!options_->java_launcher.empty()) {
      fprintf(stderr, "java_launcher_file=%s\n",
              options_->java_launcher.c_str());
    }
    fprintf(stderr, "%ld source files\n", options_->input_jars.size());
    fprintf(stderr, "%ld manifest lines\n", options_->manifest_lines.size());
  }

  if (!Open()) {
    exit(1);
  }

  // Copy launcher if it is set.
  if (!options_->java_launcher.empty()) {
    const char *const launcher_path = options_->java_launcher.c_str();
    int in_fd = open(launcher_path, O_RDONLY);
    struct stat statbuf;
    if (fd_ < 0 || fstat(in_fd, &statbuf)) {
      diag_err(1, "%s", launcher_path);
    }
    ssize_t byte_count = AppendFile(in_fd, nullptr, statbuf.st_size);
    if (byte_count < 0) {
      diag_err(1, "%s:%d: Cannot copy %s to %s", __FILE__, __LINE__,
               launcher_path, options_->output_jar.c_str());
    } else if (byte_count != statbuf.st_size) {
      diag_err(1, "%s:%d: Copied only %ld bytes out of %" PRIu64 " from %s",
               __FILE__, __LINE__, byte_count, statbuf.st_size, launcher_path);
    }
    close(in_fd);
    if (options_->verbose) {
      fprintf(stderr, "Prepended %s (%" PRIu64 " bytes)\n", launcher_path,
              statbuf.st_size);
    }
  }

  if (!options_->main_class.empty()) {
    build_properties_.AddProperty("main.class", options_->main_class);
    manifest_.Append("Main-Class: ");
    manifest_.Append(options_->main_class);
    manifest_.Append("\r\n");
  }

  for (auto &manifest_line : options_->manifest_lines) {
    if (!manifest_line.empty()) {
      manifest_.Append(manifest_line);
      if (manifest_line[manifest_line.size() - 1] != '\n') {
        manifest_.Append("\r\n");
      }
    }
  }

  for (auto &build_info_line : options_->build_info_lines) {
    build_properties_.Append(build_info_line);
    build_properties_.Append("\n");
  }

  for (auto &build_info_file : options_->build_info_files) {
    MappedFile mapped_file;
    if (!mapped_file.Open(build_info_file)) {
      diag_err(1, "%s:%d: Bad build info file %s", __FILE__, __LINE__,
               build_info_file.c_str());
    }
    const char *data = reinterpret_cast<const char *>(mapped_file.start());
    const char *data_end = reinterpret_cast<const char *>(mapped_file.end());
    // TODO(asmundak): this isn't right, we should parse properties file.
    while (data < data_end) {
      const char *next_data = strchr(static_cast<const char *>(data), '\n');
      if (next_data) {
        ++next_data;
      } else {
        next_data = data_end;
      }
      build_properties_.Append(data, next_data - data);
      data = next_data;
    }
    mapped_file.Close();
  }

  for (auto &rpath : options_->classpath_resources) {
    // TODO(asmundak): On Windows, look for \, too.
    ClasspathResource(blaze_util::Basename(rpath), rpath);
  }

  for (auto &rdesc : options_->resources) {
    // A resource description is either NAME or NAME:PATH
    std::size_t colon = rdesc.find_first_of(':');
    if (0 == colon) {
      diag_errx(1, "%s:%d: Bad resource description %s", __FILE__, __LINE__,
                rdesc.c_str());
    }
    if (std::string::npos == colon) {
      ClasspathResource(rdesc, rdesc);
    } else {
      ClasspathResource(rdesc.substr(0, colon), rdesc.substr(colon + 1));
    }
  }

  // Ready to write zip entries.
  // First, write a directory entry for the META-INF, followed by the manifest
  // file, followed by the build properties file.
  AddDirectory("META-INF/");
  manifest_.Append("\r\n");
  WriteEntry(manifest_.OutputEntry());
  if (!options_->exclude_build_data) {
    WriteEntry(build_properties_.OutputEntry());
  }

  // Then classpath resources.
  for (auto &classpath_resource : classpath_resources_) {
    WriteEntry(classpath_resource->OutputEntry());
  }

  // Then copy source files' contents.
  for (int ix = 0; ix < options_->input_jars.size(); ++ix) {
    if (!AddJar(ix)) {
      exit(1);
    }
  }

  // All entries written, write Central Directory and close.
  Close();
  return 0;
}

OutputJar::~OutputJar() {
  if (fd_ >= 0) {
    diag_warnx("%s:%d: Close() should be called first", __FILE__, __LINE__);
  }
}

bool OutputJar::Open() {
  if (fd_ >= 0) {
    diag_errx(1, "%s:%d: Cannot open output archive twice", __FILE__, __LINE__);
  }
  // The output file has read/write/execute permissions for the owner,
  // default for the rest.
  mode_t old_umask = umask(0);
  fd_ = creat(path(), (S_IRWXU | S_IRWXG | S_IRWXO) & ~old_umask);
  umask(old_umask);
  if (fd_ < 0) {
    diag_warn("%s:%d: %s", __FILE__, __LINE__, path());
    return false;
  }
  if (options_->verbose) {
    fprintf(stderr, "Writing to %s\n", path());
  }
  return true;
}

bool OutputJar::AddJar(int jar_path_index) {
  const std::string& input_jar_path = options_->input_jars[jar_path_index];
  InputJar input_jar;
  if (!input_jar.Open(input_jar_path)) {
    return false;
  }
  const CDH *jar_entry;
  const LH *lh;
  while ((jar_entry = input_jar.NextEntry(&lh))) {
    const char *file_name = jar_entry->file_name();
    auto file_name_length = jar_entry->file_name_length();
    if (!file_name_length) {
      diag_errx(
          1, "%s:%d: Bad central directory record in %s at offset 0x%" PRIx64,
          __FILE__, __LINE__, input_jar_path.c_str(),
          input_jar.CentralDirectoryRecordOffset(jar_entry));
    }
    // Special files that cannot be handled by looking up known_members_ map:
    // * ignore *.SF, *.RSA, *.DSA
    //   (TODO(asmundak): should this be done only in META-INF?
    //
    if (ends_with(file_name, file_name_length, ".SF") ||
        ends_with(file_name, file_name_length, ".RSA") ||
        ends_with(file_name, file_name_length, ".DSA")) {
      continue;
    }

    bool is_dir = (file_name[file_name_length - 1] != '/');
    if (is_dir &&
        begins_with(file_name, file_name_length, "META-INF/services/")) {
      // The contents of the META-INF/services/<SERVICE> on the output is the
      // concatenation of the META-INF/services/<SERVICE> files from all inputs.
      std::string service_path(file_name, file_name_length);
      if (!known_members_.count(service_path)) {
        // Create a concatenator and add it to the known_members_ map.
        // The call to Merge() below will then take care of the rest.
        Concatenator *service_handler = new Concatenator(service_path);
        service_handlers_.emplace_back(service_handler);
        known_members_.emplace(service_path, EntryInfo{service_handler});
      }
    }

    // Install a new entry unless it is already present. All the plain (non-dir)
    // entries that require a combiner have been already installed, so the call
    // will add either a directory entry whose handler will ignore subsequent
    // duplicates, or an ordinary plain entry, for which we save the index of
    // the first input jar (in order to provide diagnostics on duplicate).
    auto got =
        known_members_.emplace(std::string(file_name, file_name_length),
                               EntryInfo{is_dir ? &null_combiner_ : nullptr,
                                         is_dir ? -1 : jar_path_index});
    if (!got.second) {
      auto &entry_info = got.first->second;
      // Handle special entries (the ones that have a combiner.
      if (entry_info.combiner_ != nullptr) {
        entry_info.combiner_->Merge(jar_entry, lh);
        continue;
      }

      // Plain file entry. If duplicates are not allowed, bail out. Otherwise
      // just ignore this entry.
      if (options_->no_duplicates ||
          (options_->no_duplicate_classes &&
           ends_with(file_name, file_name_length, ".class"))) {
        diag_errx(1, "%s:%d: %.*s is present both in %s and %s", __FILE__,
                  __LINE__, file_name_length, file_name,
                  options_->input_jars[entry_info.input_jar_index_].c_str(),
                  input_jar_path.c_str());
      } else {
        duplicate_entries_++;
        continue;
      }
    }

    // Now we have to copy:
    //  local header
    //  file data
    //  data descriptor, if present.
    off_t copy_from = jar_entry->local_header_offset();
    size_t num_bytes = lh->size();
    if (jar_entry->no_size_in_local_header()) {
      // The size of the data descriptor varies. The actual data in it is three
      // uint32's (crc32, compressed size, uncompressed size), but these can be
      // preceded by the "PK\x7\x8" signature word (alas, 'jar' has it).
      // Reading the descriptor just to figure out whether we need to copy four
      // or three words will cost us another page read, let us assume the data
      // description is always 4 words long at the cost of having an occasional
      // one word gap between the entries.
      num_bytes += jar_entry->compressed_file_size() + 4 * sizeof(uint32_t);
    } else {
      num_bytes += lh->compressed_file_size();
    }
    off_t output_position = Position();
    // Do the actual copy. Use sendfile, avoiding copying the data to user
    // space and back.
    ssize_t n_copied = AppendFile(input_jar.fd(), &copy_from, num_bytes);
    if (n_copied < 0) {
      diag_err(1, "%s:%d: Cannot copy %ld bytes of %.*s from %s", __FILE__,
               __LINE__, num_bytes, file_name_length, file_name,
               input_jar_path.c_str());
    } else if (static_cast<size_t>(n_copied) != num_bytes) {
      diag_err(1, "%s:%d: Copied only %ld bytes out of %ld from %s", __FILE__,
               __LINE__, n_copied, num_bytes, input_jar_path.c_str());
    }

    // Append central directory header for this file to the output central
    // directory we are building.
    TODO(output_position < 0xFFFFFFFF, "Handle Zip64");
    AppendToDirectoryBuffer(jar_entry)->local_header_offset32(output_position);
    ++entries_;
  }
  return input_jar.Close();
}

off_t OutputJar::Position() {
  off_t position = lseek(fd_, 0, SEEK_CUR);
  if (position == (off_t)-1) {
    diag_err(1, "%s:%d: lseek", __FILE__, __LINE__);
  }
  TODO(position < 0xFFFFFFFF, "Handle Zip64");
  return position;
}

// Writes an entry. The argument is the pointer to the contiguos block of
// memory containing Local Header for the entry, immediately followed by
// the data. The memory is freed after the data has been written.
void OutputJar::WriteEntry(void *buffer) {
  if (buffer == nullptr) {
    return;
  }
  LH *entry = reinterpret_cast<LH *>(buffer);
  if (options_->verbose) {
    fprintf(stderr, "%-.*s combiner has %lu bytes, %s to %lu\n",
            entry->file_name_length(), entry->file_name(),
            entry->uncompressed_file_size(),
            entry->compression_method() == Z_NO_COMPRESSION ? "copied"
                                                            : "compressed",
            entry->compressed_file_size());
  }
  uint8_t *data_end = entry->data() + entry->in_zip_size();
  uint8_t *data = reinterpret_cast<uint8_t *>(entry);
  off_t output_position = Position();
  while (data < data_end) {
    ssize_t written = write(fd_, data, data_end - data);
    if (written >= 0) {
      data += written;
    } else if (errno != EINTR) {
      diag_err(1, "%s:%d: write", __FILE__, __LINE__);
    }
  }
  // Data written, allocate CDH space and populate CDH.
  CDH *cdh = reinterpret_cast<CDH *>(
      ReserveCdh(sizeof(CDH) + entry->file_name_length()));
  cdh->signature();
  cdh->version(20);
  cdh->version_to_extract(entry->version());
  cdh->bit_flag(0x0);
  cdh->compression_method(entry->compression_method());
  cdh->last_mod_file_time(entry->last_mod_file_time());
  cdh->last_mod_file_date(entry->last_mod_file_date());
  cdh->crc32(entry->crc32());
  TODO(entry->compressed_file_size32() != 0xFFFFFFFF, "Handle Zip64");
  cdh->compressed_file_size32(entry->compressed_file_size32());
  TODO(entry->uncompressed_file_size32() != 0xFFFFFFFF, "Handle Zip64");
  cdh->uncompressed_file_size32(entry->uncompressed_file_size32());
  cdh->file_name(entry->file_name(), entry->file_name_length());
  cdh->extra_fields(nullptr, 0);
  cdh->comment_length(0);
  cdh->start_disk_nr(0);
  cdh->internal_attributes(0);
  cdh->external_attributes(0);
  cdh->local_header_offset32(output_position);
  ++entries_;
  free(reinterpret_cast<void *>(entry));
}

void OutputJar::AddDirectory(const char *path) {
  size_t n_path = strlen(path);
  size_t lh_size = sizeof(LH) + n_path;
  LH *lh = reinterpret_cast<LH *>(malloc(lh_size));
  lh->signature();
  lh->version(20);
  lh->bit_flag(0);  // TODO(asmundak): should I set UTF8 flag?
  lh->compression_method(Z_NO_COMPRESSION);
  lh->last_mod_file_time(0);
  lh->last_mod_file_date(33);
  lh->crc32(0);
  lh->compressed_file_size32(0);
  lh->uncompressed_file_size32(0);
  lh->file_name(path, n_path);
  lh->extra_fields(nullptr, 0);
  known_members_.emplace(path, EntryInfo{&null_combiner_});
  WriteEntry(lh);
}

// Appends a Central Directory Entry to the directory buffer.
CDH *OutputJar::AppendToDirectoryBuffer(const CDH *cdh) {
  size_t cdh_size = cdh->size();
  return reinterpret_cast<CDH *>(
      memcpy(reinterpret_cast<CDH *>(ReserveCdr(cdh_size)), cdh, cdh_size));
}

uint8_t *OutputJar::ReserveCdr(size_t chunk_size) {
  if (cen_size_ + chunk_size > cen_capacity_) {
    cen_capacity_ += 1000000;
    cen_ = reinterpret_cast<uint8_t *>(realloc(cen_, cen_capacity_));
    if (!cen_) {
      diag_errx(1, "%s:%d: Cannot allocate %ld bytes for the directory",
                __FILE__, __LINE__, cen_capacity_);
    }
  }
  uint8_t *entry = cen_ + cen_size_;
  cen_size_ += chunk_size;
  return entry;
}

uint8_t *OutputJar::ReserveCdh(size_t size) {
  return static_cast<uint8_t *>(memset(ReserveCdr(size), 0, size));
}

// Write out combined jar.
bool OutputJar::Close() {
  if (fd_ < 0) {
    return true;
  }

  for (auto &service_handler : service_handlers_) {
    WriteEntry(service_handler->OutputEntry());
  }
  for (auto &extra_combiner : extra_combiners_) {
    WriteEntry(extra_combiner->OutputEntry());
  }
  WriteEntry(spring_handlers_.OutputEntry());
  WriteEntry(spring_schemas_.OutputEntry());
  WriteEntry(protobuf_meta_handler_.OutputEntry());
  // TODO(asmundak): handle manifest;
  off_t output_position = lseek(fd_, 0, SEEK_CUR);
  if (output_position == (off_t)-1) {
    diag_err(1, "%s:%d: lseek", __FILE__, __LINE__);
  }
  TODO(output_position < 0xFFFFFFFF, "Handle Zip64");

  size_t cen_size =
      cen_size_;  // Save it before AppendToDirectoryBuffer updates it.
  ECD *ecd = reinterpret_cast<ECD *>(ReserveCdh(sizeof(ECD)));
  ecd->signature();
  ecd->this_disk_entries16((uint16_t)entries_);
  TODO(entries_ < 0xFFFF, "Handle >=64K entries");
  ecd->total_entries16((uint16_t)entries_);
  TODO(cen_size < 0xFFFFFFFF, "Handle Zip64");
  ecd->cen_size32(cen_size);
  TODO(output_position < 0xFFFFFFFF, "Handle Zip64");
  ecd->cen_offset32(output_position);

  // Write Central Directory.
  uint8_t *cen_end = cen_ + cen_size_;
  uint8_t *cen = cen_;
  while (cen < cen_end) {
    ssize_t n = write(fd_, cen, cen_end - cen);
    if (n < 0) {
      diag_err(1, "%s:%d: Cannot write central directory", __FILE__, __LINE__);
    }
    cen += n;
  }
  free(cen_);

  if (close(fd_)) {
    diag_err(1, "%s:%d: %s", __FILE__, __LINE__, path());
    fd_ = -1;
    return false;
  }

  fd_ = -1;
  if (options_->verbose) {
    fprintf(stderr, "Wrote %s with %d entries", path(), entries_);
    if (duplicate_entries_) {
      fprintf(stderr, ", skipped %d entries", duplicate_entries_);
    }
    fprintf(stderr, "\n");
  }
  return true;
}

void OutputJar::ClasspathResource(const std::string &resource_name,
                                  const std::string &resource_path) {
  if (known_members_.count(resource_name)) {
    if (options_->warn_duplicate_resources) {
      diag_warnx(
          "%s:%d: Duplicate resource name %s in the --classpath_resource or "
          "--resource option",
          __FILE__, __LINE__, resource_name.c_str());
      // TODO(asmundak): this mimics old behaviour. Confirm that unless
      // we run with --warn_duplicate_resources, the output zip file contains
      // the concatenated contents of the all the resources with the same name.
      return;
    }
  }
  MappedFile mapped_file;
  if (!mapped_file.Open(resource_path)) {
    diag_err(1, "%s:%d: %s", __FILE__, __LINE__, resource_path.c_str());
  }
  Concatenator *classpath_resource = new Concatenator(resource_name);
  classpath_resource->Append(
      reinterpret_cast<const char *>(mapped_file.start()), mapped_file.size());
  classpath_resources_.emplace_back(classpath_resource);
  known_members_.emplace(resource_name, EntryInfo{classpath_resource});
}

#if defined(__APPLE__)
ssize_t OutputJar::AppendFile(int in_fd, off_t *in_offset, size_t count) {
  if (!count) {
    return 0;
  }
  uint8_t buffer[8192];
  ssize_t total_written = 0;

  // If the input file position (the offset in the input file) has been  passed,
  // that's where we start, and the input file position has to be restored after
  // we are done copying.
  const off_t offset_error = static_cast<off_t>(-1);
  off_t old_input_offset = offset_error;
  if (in_offset) {
    if (offset_error == (old_input_offset = lseek(in_fd, 0, SEEK_CUR)) ||
        offset_error == lseek(in_fd, *in_offset, SEEK_SET)) {
      return -1;
    }
  }
  while (total_written < count) {
    ssize_t n_read =
        read(in_fd, buffer, std::min(sizeof(buffer), count - total_written));
    if (n_read > 0) {
      uint8_t *write_buffer = buffer;
      uint8_t *write_buffer_end = write_buffer + n_read;
      while (write_buffer < write_buffer_end) {
        ssize_t n_written =
            write(fd_, write_buffer, write_buffer_end - write_buffer);
        if (n_written > 0) {
          write_buffer += n_written;
        } else if (EAGAIN != errno) {
          return -1;
        }
      }
      total_written += n_read;
    } else if (n_read == 0) {
      break;
    } else if (EAGAIN != errno) {
      return -1;
    }
  }

  // If the input file position has been passed, update it and restore
  // the read position in the input file.
  if (in_offset) {
    if (offset_error == lseek(in_fd, old_input_offset, SEEK_SET)) {
      return -1;
    }
    *in_offset += total_written;
  }
  return total_written;
}

#elif defined(__linux)
ssize_t OutputJar::AppendFile(int in_fd, off_t *in_offset, size_t count) {
  // sendfile call is interruptable and has to be handled the same way as write
  // call.
  for (size_t to_write = count; to_write > 0;) {
    ssize_t written = sendfile(fd_, in_fd, in_offset, to_write);
    if (written < 0) {
      return written;
    } else if (written == 0) {
      return static_cast<ssize_t>(count - to_write);
    }
    to_write -= static_cast<size_t>(written);
  }
  return static_cast<ssize_t>(count);
}
#endif

void OutputJar::ExtraCombiner(const std::string &entry_name,
                              Combiner *combiner) {
  extra_combiners_.emplace_back(combiner);
  known_members_.emplace(entry_name, EntryInfo{combiner});
}
