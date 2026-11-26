#include "json_util.h"

#include <charconv>
#include <memory>
#include <system_error>

namespace json_util {

namespace {

// 与 client.cpp 的 parse_int64 同口径：拒绝空串、尾部垃圾、前导空白与溢出。
std::int64_t parse_int64(const std::string &text, std::int64_t fallback) {
  if (text.empty()) {
    return fallback;
  }
  std::int64_t value = 0;
  const auto result =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (result.ec != std::errc() || result.ptr != text.data() + text.size()) {
    return fallback;
  }
  return value;
}

} // namespace

std::optional<Json::Value> parse(std::string_view text) {
  if (text.empty()) {
    return std::nullopt;
  }
  Json::CharReaderBuilder builder;
  builder["collectComments"] = false;
  std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
  Json::Value root;
  std::string errors;
  if (!reader->parse(text.data(), text.data() + text.size(), &root,
                     &errors)) {
    return std::nullopt;
  }
  return root;
}

Json::Value parseOrNull(std::string_view text) {
  auto value = parse(text);
  return value ? *value : Json::Value(Json::nullValue);
}

std::string dump(const Json::Value &value) {
  Json::StreamWriterBuilder builder;
  builder["indentation"] = "";
  builder["emitUTF8"] = true;
  return Json::writeString(builder, value);
}

std::int64_t int_or(const Json::Value &object, const char *key,
                    std::int64_t fallback) {
  if (!object.isObject() || !object.isMember(key)) {
    return fallback;
  }
  const Json::Value &value = object[key];
  if (value.isInt64()) {
    return value.asInt64();
  }
  if (value.isString()) {
    return parse_int64(value.asString(), fallback);
  }
  return fallback;
}

std::string string_or(const Json::Value &object, const char *key,
                      std::string fallback) {
  if (!object.isObject() || !object.isMember(key)) {
    return fallback;
  }
  const Json::Value &value = object[key];
  if (!value.isString()) {
    return fallback;
  }
  return value.asString();
}

} // namespace json_util