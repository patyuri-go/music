#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <json/json.h>

namespace json_util {

// 解析 JSON 文本；失败返回 nullopt。
std::optional<Json::Value> parse(std::string_view text);

// 解析失败时返回 nullValue，保留旧代码 if (!json) 的判断习惯。
Json::Value parseOrNull(std::string_view text);

// 从请求体取整数。键缺失、类型不符、字符串解析失败一律返回 fallback，
// 不抛异常：调用方是 HTTP 入口，不该因为一个字段的类型错误就断掉整个请求。
// 字符串形态的整数按数值解析（"42" -> 42）；尾部带垃圾（"12abc"）、
// 溢出、前导空白都视为解析失败。
std::int64_t int_or(const Json::Value &object, const char *key,
                    std::int64_t fallback = 0);

// 从请求体取字符串。只有 JSON string 原样返回；数字、布尔、数组、对象、
// null 一律返回 fallback（不做 asString() 的隐式转换）。
std::string string_or(const Json::Value &object, const char *key,
                      std::string fallback = {});

// 紧凑输出（无缩进、UTF-8 原样），用于 HTTP 响应体。
std::string dump(const Json::Value &value);

} // namespace json_util