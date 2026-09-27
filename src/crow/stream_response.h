#pragma once

#include <functional>
#include <string>
#include <utility>

#include "crow/http_response.h"

namespace crow {

/// Build a response whose body is produced lazily by `producer`.
///
/// `producer(buf)` fills `buf` with the next chunk of the body and returns
/// false to signal end-of-stream. `length` is the total number of bytes the
/// producer will yield (used for the Content-Length header); pass a negative
/// value to omit it. `closer` (optional) runs once when the response finishes
/// or aborts.
inline response streamedResponse(int code, std::string content_type,
                                 long long length,
                                 std::function<bool(std::string &)> producer,
                                 std::function<void()> closer = nullptr) {
  response res;
  res.code = code;
  if (!content_type.empty()) {
    res.set_header("Content-Type", std::move(content_type));
  }
  if (length >= 0) {
    res.set_header("Content-Length", std::to_string(length));
    res.manual_length_header = true;
  }
  res.set_stream_source(std::move(producer));
  if (closer) {
    res.set_stream_closer(std::move(closer));
  }
  return res;
}

} // namespace crow
