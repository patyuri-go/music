// media_library::ByteStream 契约的单元测试。
//
// 只压本地文件流：远端流的「EOF vs 传输错误」区分需要真连一次上游，
// 那部分由 test/storage_test.cpp 的集成用例覆盖（这里不碰网络）。

#include "media_library/byte_stream.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace fs = std::filesystem;
using media_library::ByteStream;
using media_library::ErrorCode;
using media_library::make_file_stream;

// 每个用例一个独立临时文件，析构时删除；避免用例之间互相污染。
class TempFile {
public:
  explicit TempFile(const std::string &content) {
    static std::atomic<int> counter{0};
    path_ = fs::temp_directory_path() /
            ("musicd_byte_stream_test_" + std::to_string(::getpid()) + "_" +
             std::to_string(counter.fetch_add(1)));
    std::ofstream out(path_, std::ios::binary);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
  }
  ~TempFile() {
    std::error_code ec;
    fs::remove(path_, ec);
  }
  TempFile(const TempFile &) = delete;
  TempFile &operator=(const TempFile &) = delete;

  std::string string() const { return path_.string(); }

private:
  fs::path path_;
};

std::string to_string(const std::vector<std::byte> &bytes) {
  return std::string(reinterpret_cast<const char *>(bytes.data()), bytes.size());
}

TEST(ByteStreamTest, FileStreamReportsSizeAndReadsAllBytes) {
  const TempFile file("hello world");
  auto opened = make_file_stream(file.string(), "audio/mpeg");
  ASSERT_TRUE(opened.ok()) << opened.error().message;
  std::unique_ptr<ByteStream> stream = std::move(opened).value();

  EXPECT_EQ(stream->info().size_bytes, 11);
  EXPECT_EQ(stream->info().mime_type, "audio/mpeg");
  EXPECT_TRUE(stream->info().seekable);

  auto chunk = stream->read(5);
  ASSERT_TRUE(chunk.ok());
  EXPECT_EQ(to_string(chunk.value()), "hello");

  auto rest = stream->read(1024);
  ASSERT_TRUE(rest.ok());
  EXPECT_EQ(to_string(rest.value()), " world");
}

TEST(ByteStreamTest, FileStreamReturnsEmptyAtEof) {
  const TempFile file("abc");
  auto opened = make_file_stream(file.string(), "");
  ASSERT_TRUE(opened.ok());
  std::unique_ptr<ByteStream> stream = std::move(opened).value();

  ASSERT_TRUE(stream->read(16).ok());
  auto eof = stream->read(16);
  ASSERT_TRUE(eof.ok()) << "EOF 必须是成功 + 空数组，不是失败";
  EXPECT_TRUE(eof.value().empty());
}

TEST(ByteStreamTest, FileStreamSeekThenRead) {
  const TempFile file("0123456789");
  auto opened = make_file_stream(file.string(), "");
  ASSERT_TRUE(opened.ok());
  std::unique_ptr<ByteStream> stream = std::move(opened).value();

  ASSERT_TRUE(stream->seek(4).ok());
  auto chunk = stream->read(3);
  ASSERT_TRUE(chunk.ok());
  EXPECT_EQ(to_string(chunk.value()), "456");
}

TEST(ByteStreamTest, MissingFileReturnsNotFound) {
  auto opened = make_file_stream("/nonexistent/musicd/no-such-file", "");
  ASSERT_FALSE(opened.ok());
  EXPECT_EQ(opened.error().code, ErrorCode::NotFound);
}

TEST(ByteStreamTest, NegativeSeekIsRejected) {
  const TempFile file("abc");
  auto opened = make_file_stream(file.string(), "");
  ASSERT_TRUE(opened.ok());
  std::unique_ptr<ByteStream> stream = std::move(opened).value();

  const auto status = stream->seek(-1);
  ASSERT_FALSE(status.ok());
  EXPECT_EQ(status.error().code, ErrorCode::InvalidRequest);
}

// 默认实现是空操作，必须幂等、可从任意线程调用（worker 会在别的线程
// 调 cancel()，本地文件分支不该因此崩）。
TEST(ByteStreamTest, CancelIsIdempotentOnFileStream) {
  const TempFile file("abc");
  auto opened = make_file_stream(file.string(), "");
  ASSERT_TRUE(opened.ok());
  std::unique_ptr<ByteStream> stream = std::move(opened).value();

  std::thread first([&] { stream->cancel(); });
  std::thread second([&] { stream->cancel(); });
  first.join();
  second.join();
  stream->cancel();

  // 取消本地文件流不应让后续读取失败（没有可中止的上游）。
  auto chunk = stream->read(16);
  ASSERT_TRUE(chunk.ok());
  EXPECT_EQ(to_string(chunk.value()), "abc");
}

} // namespace
