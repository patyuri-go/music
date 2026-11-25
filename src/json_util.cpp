#include "json_util.h"

#include <memory>

namespace json_util {

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

} // namespace json_util