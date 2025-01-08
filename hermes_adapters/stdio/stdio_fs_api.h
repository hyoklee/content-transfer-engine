/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Distributed under BSD 3-Clause license.                                   *
 * Copyright by The HDF Group.                                               *
 * Copyright by the Illinois Institute of Technology.                        *
 * All rights reserved.                                                      *
 *                                                                           *
 * This file is part of Hermes. The full Hermes copyright notice, including  *
 * terms governing use, modification, and redistribution, is contained in    *
 * the COPYING file, which can be found at the top directory. If you do not  *
 * have access to the file, you may request a copy from help@hdfgroup.org.   *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#ifndef HERMES_ADAPTER_STDIO_NATIVE_H_
#define HERMES_ADAPTER_STDIO_NATIVE_H_

#include <memory>

#include "hermes_adapters/filesystem/filesystem.h"
#include "hermes_adapters/filesystem/filesystem_mdm.h"
#include "hermes_adapters/posix/posix_fs_api.h"
#include "stdio_api.h"

namespace hermes::adapter {

/** A class to represent POSIX IO file system */
class StdioFs : public hermes::adapter::Filesystem {
 public:
  HERMES_STDIO_API_T real_api_; /**< pointer to real APIs */

 public:
  StdioFs() : Filesystem(AdapterType::kStdio) { real_api_ = HERMES_STDIO_API; }

  /** Close an existing stream and then open with new path */
  FILE *Reopen(const std::string &user_path, const char *mode,
               AdapterStat &stat) {
    auto real_api_ = HERMES_STDIO_API;
    FILE *ret;
    ret = real_api_->freopen(user_path.c_str(), mode, stat.fh_);
    if (!ret) {
      return ret;
    }
    stat.fh_ = ret;
    HILOG(kDebug, "Reopen file for filename: {} in mode {}", user_path, mode);
    stat.UpdateTime();
    return (FILE *)&stat;
  }

  /** fdopen */
  FILE *FdOpen(const std::string &mode, std::shared_ptr<AdapterStat> &stat) {
    auto real_api_ = HERMES_STDIO_API;
    auto mdm = HERMES_FS_METADATA_MANAGER;
    stat->fh_ = real_api_->fdopen(stat->fd_, mode.c_str());
    stat->mode_str_ = mode;
    File f;
    f.hermes_fh_ = (FILE *)stat.get();
    mdm->Create(f, stat);
    return f.hermes_fh_;
  }

  /** Whether or not \a fd FILE DESCRIPTOR is tracked */
  static bool IsFdTracked(int fd, std::shared_ptr<AdapterStat> &stat) {
    return PosixFs::IsFdTracked(fd, stat);
  }

  /** Whether or not \a fd FILE DESCRIPTOR is tracked */
  static bool IsFdTracked(int fd) { return PosixFs::IsFdTracked(fd); }

  /** Whether or not \a fp FILE was generated by Hermes */
  static bool IsFpTracked(FILE *fp, std::shared_ptr<AdapterStat> &stat) {
    if (!fp || !HERMES->IsInitialized()) {
      return false;
    }
    hermes::adapter::File f;
    f.hermes_fh_ = fp;
    stat = HERMES_FS_METADATA_MANAGER->Find(f);
    return stat != nullptr;
  }

  /** Whether or not \a fp FILE was generated by Hermes */
  static bool IsFpTracked(FILE *fp) {
    std::shared_ptr<AdapterStat> stat;
    return IsFpTracked(fp, stat);
  }

  /** get the file name from \a fp file pointer */
  static std::string GetFilenameFromFP(FILE *fp) {
    char proclnk[kMaxPathLen];
    char filename[kMaxPathLen];
    int fno = fileno(fp);
    snprintf(proclnk, kMaxPathLen, "/proc/self/fd/%d", fno);
    size_t r = readlink(proclnk, filename, kMaxPathLen);
    filename[r] = '\0';
    return filename;
  }

 public:
  /** Allocate an fd for the file f */
  void RealOpen(File &f, AdapterStat &stat, const std::string &path) override {
    if (stat.mode_str_.find('w') != std::string::npos) {
      stat.hflags_.SetBits(HERMES_FS_TRUNC);
      stat.hflags_.SetBits(HERMES_FS_CREATE);
    }
    if (stat.mode_str_.find('a') != std::string::npos) {
      stat.hflags_.SetBits(HERMES_FS_APPEND);
      stat.hflags_.SetBits(HERMES_FS_CREATE);
    }

    if (stat.hflags_.Any(HERMES_FS_CREATE)) {
      if (stat.adapter_mode_ != AdapterMode::kScratch) {
        stat.fh_ = real_api_->fopen(path.c_str(), stat.mode_str_.c_str());
      }
    } else {
      stat.fh_ = real_api_->fopen(path.c_str(), stat.mode_str_.c_str());
    }

    if (stat.fh_ != nullptr) {
      stat.hflags_.SetBits(HERMES_FS_EXISTS);
    }
    if (stat.fh_ == nullptr && stat.adapter_mode_ != AdapterMode::kScratch) {
      f.status_ = false;
    }
  }

  /**
   * Called after real open. Allocates the Hermes representation of
   * identifying file information, such as a hermes file descriptor
   * and hermes file handler. These are not the same as STDIO file
   * descriptor and STDIO file handler.
   * */
  void HermesOpen(File &f, const AdapterStat &stat,
                  FilesystemIoClientState &fs_mdm) override {
    f.hermes_fh_ = (FILE *)fs_mdm.stat_;
  }

  /** Synchronize \a file FILE f */
  int RealSync(const File &f, const AdapterStat &stat) override {
    (void)f;
    if (stat.adapter_mode_ == AdapterMode::kScratch && stat.fh_ == nullptr) {
      return 0;
    }
    return real_api_->fflush(stat.fh_);
  }

  /** Close \a file FILE f */
  int RealClose(const File &f, AdapterStat &stat) override {
    if (stat.adapter_mode_ == AdapterMode::kScratch && stat.fh_ == nullptr) {
      return 0;
    }
    return real_api_->fclose(stat.fh_);
  }

  /**
   * Called before RealClose. Releases information provisioned during
   * the allocation phase.
   * */
  void HermesClose(File &f, const AdapterStat &stat,
                   FilesystemIoClientState &fs_mdm) override {
    (void)f;
    (void)stat;
    (void)fs_mdm;
  }

  /** Remove \a file FILE f */
  int RealRemove(const std::string &path) override {
    return remove(path.c_str());
  }

  /** Get initial statistics from the backend */
  size_t GetBackendSize(const chi::string &bkt_name) override {
    size_t true_size = 0;
    std::string filename = bkt_name.str();
    int fd = open(filename.c_str(), O_RDONLY);
    if (fd < 0) {
      return 0;
    }
    struct stat buf;
    fstat(fd, &buf);
    true_size = buf.st_size;
    close(fd);

    HILOG(kDebug, "The size of the file {} on disk is {}", filename, true_size);
    return true_size;
  }

  /** Write blob to backend */
  void WriteBlob(const std::string &bkt_name, const Blob &full_blob,
                 const FsIoOptions &opts, IoStatus &status) override {
    status.success_ = true;
    HILOG(kDebug,
          "Writing to file: {}"
          " offset: {}"
          " size: {}",
          bkt_name, opts.backend_off_, full_blob.size());
    FILE *fh = real_api_->fopen(bkt_name.c_str(), "r+");
    if (fh == nullptr) {
      status.size_ = 0;
      status.success_ = false;
      return;
    }
    real_api_->fseek(fh, opts.backend_off_, SEEK_SET);
    status.size_ =
        real_api_->fwrite(full_blob.data(), sizeof(char), full_blob.size(), fh);
    if (status.size_ != full_blob.size()) {
      status.success_ = false;
    }
    real_api_->fclose(fh);
  }

  /** Read blob from the backend */
  void ReadBlob(const std::string &bkt_name, Blob &full_blob,
                const FsIoOptions &opts, IoStatus &status) override {
    status.success_ = true;
    HILOG(kDebug,
          "Reading from file: {}"
          " offset: {}"
          " size: {}",
          bkt_name, opts.backend_off_, full_blob.size());
    FILE *fh = real_api_->fopen(bkt_name.c_str(), "r");
    if (fh == nullptr) {
      status.size_ = 0;
      status.success_ = false;
      return;
    }
    real_api_->fseek(fh, opts.backend_off_, SEEK_SET);
    status.size_ =
        real_api_->fread(full_blob.data(), sizeof(char), full_blob.size(), fh);
    if (status.size_ != full_blob.size()) {
      status.success_ = false;
    }
    real_api_->fclose(fh);
  }

  void UpdateIoStatus(const FsIoOptions &opts, IoStatus &status) override {
    (void)opts;
    (void)status;
  }
};

/** Simplify access to the stateless StdioFs Singleton */
#define HERMES_STDIO_FS \
  hshm::Singleton<::hermes::adapter::StdioFs>::GetInstance()
#define HERMES_STDIO_FS_T hermes::adapter::StdioFs *

}  // namespace hermes::adapter

#endif  // HERMES_ADAPTER_STDIO_NATIVE_H_
