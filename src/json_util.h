#pragma once

#include <optional>
#include <string>
#include <string_view>

#include <json/json.h>

namespace json_util {

// 解析 JSON 文本；失败返回 nullopt。
std::optional<Json::Value> parse(std::string_view text);

// 解析失败时返回 nullValue，保留旧代码 if (!json) 的判断习惯。
Json::Value parseOrNull(std::string_view text);

// 紧凑输出（无缩进、UTF-8 原样），用于 HTTP 响应体。
std::string dump(const Json::Value &value);

} // namespace json_util