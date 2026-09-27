#pragma once

#include <taglib/toolkit/tiostream.h>

#include <memory>
#include <string>

#include "webdav.h"

/// 基于 WebDAV 的 TagLib::IOStream。
///
/// 只读模式：按需 Range GET + 分块缓存（用于读取远端标签，不下载整文件）。
/// 读写模式：首次打开时下载整文件到内存，写入后 flush() 用 PUT 回写。
class WebdavStream : public TagLib::IOStream {
public:
  WebdavStream(const std::string &url, bool read_only);
  ~WebdavStream() override;

  TagLib::FileName name() const override;
  TagLib::ByteVector readBlock(std::size_t length) override;
  void writeBlock(const TagLib::ByteVector &data) override;
  void insert(const TagLib::ByteVector &data, TagLib::offset_t start = 0,
              std::size_t replace = 0) override;
  void removeBlock(TagLib::offset_t start = 0, std::size_t length = 0) override;
  bool readOnly() const override { return read_only_; }
  bool isOpen() const override { return open_; }
  void seek(TagLib::offset_t offset, Position p = Beginning) override;
  TagLib::offset_t tell() const override { return pos_; }
  TagLib::offset_t length() override;
  void truncate(TagLib::offset_t length) override;

  /// 读写模式下把内存缓冲 PUT 回远端。
  bool flush();

private:
  std::string fetch(TagLib::offset_t offset, std::size_t count);

  std::string url_;
  std::string path_;
  bool read_only_ = true;
  bool open_ = false;
  std::unique_ptr<webdav::Client> client_;

  std::string data_; // 读写模式下的整文件缓冲
  TagLib::offset_t pos_ = 0;
  TagLib::offset_t length_ = -1;

  std::string cache_;
  TagLib::offset_t cache_off_ = 0;
  bool cache_valid_ = false;
};
