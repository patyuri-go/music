// 测试用存储服务：按实体 id 返回确定字节，验证 musicd 的代理行为。
//
//   ./fake_storage [port]        # 默认 15666
//
// 端点：
//   GET|HEAD /api/entities/{id}/content   单区间 Range；206/416 语义与上游一致
//   POST     /__control/reset             恢复到内置默认实体
//   POST     /__control/entity/{id}       覆盖状态/延迟/内容类型/字节
//
// 所有内容都在内存里；不依赖数据库或真实存储服务。

#include "fixture_ids.h"

#include <drogon/drogon.h>

#include <json/json.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

struct Entity {
  std::string bytes;
  std::string content_type{"application/octet-stream"};
  int status = 200;
  int delay_ms = 0;
};

std::mutex g_mutex;
std::map<std::string, Entity> g_entities;

std::string patternBytes(std::size_t size, unsigned seed) {
  std::string out;
  out.resize(size);
  for (std::size_t i = 0; i < size; ++i) {
    out[i] = static_cast<char>((i * 31 + seed * 17 + 7) % 251);
  }
  return out;
}

std::string jpegBytes(std::size_t size, unsigned seed) {
  std::string out = patternBytes(size, seed);
  if (out.size() >= 4) {
    out[0] = static_cast<char>(0xFF);
    out[1] = static_cast<char>(0xD8);
    out[2] = static_cast<char>(0xFF);
    out[3] = static_cast<char>(0xE0);
  }
  return out;
}

std::string wavBytes(std::size_t size, unsigned seed) {
  std::string out = patternBytes(size, seed);
  const char riff[] = {'R', 'I', 'F', 'F'};
  const char wave[] = {'W', 'A', 'V', 'E'};
  if (out.size() >= 12) {
    std::copy(std::begin(riff), std::end(riff), out.begin());
    std::copy(std::begin(wave), std::end(wave), out.begin() + 8);
  }
  return out;
}

void resetDefaults() {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_entities.clear();
  g_entities[fixture::kTrackRoleEntity] =
      Entity{wavBytes(fixture::kSizeBytes, 1), "audio/wav", 200, 0};
  g_entities[fixture::kTrackPlainEntity] =
      Entity{wavBytes(fixture::kSizeBytes, 2), "audio/wav", 200, 0};
  g_entities[fixture::kTrackAlbumNullEntity] =
      Entity{wavBytes(fixture::kSizeBytes, 4), "audio/wav", 200, 0};
  g_entities[fixture::kAlbumEntity] =
      Entity{jpegBytes(65536, 3), "image/jpeg", 200, 0};
}

struct ParsedRange {
  bool present = false;
  bool satisfiable = true;
  std::size_t start = 0;
  std::size_t end = 0; // inclusive
};

ParsedRange parseRange(const std::string &header, std::size_t size) {
  ParsedRange range;
  if (header.empty() || header.rfind("bytes=", 0) != 0) {
    return range;
  }
  const std::string spec = header.substr(6);
  if (spec.find(',') != std::string::npos) {
    // 多区间：上游按整文件处理。
    return range;
  }
  range.present = true;
  const std::size_t dash = spec.find('-');
  if (dash == std::string::npos) {
    range.present = false;
    return range;
  }
  const std::string first = spec.substr(0, dash);
  const std::string last = spec.substr(dash + 1);
  try {
    if (first.empty()) {
      // 后缀区间：取最后 N 字节。
      const long long suffix = std::stoll(last);
      if (suffix <= 0 || size == 0) {
        range.satisfiable = false;
        return range;
      }
      const std::size_t count =
          std::min<std::size_t>(static_cast<std::size_t>(suffix), size);
      range.start = size - count;
      range.end = size - 1;
      return range;
    }
    const long long start = std::stoll(first);
    if (start < 0 || static_cast<std::size_t>(start) >= size) {
      range.satisfiable = false;
      return range;
    }
    range.start = static_cast<std::size_t>(start);
    if (last.empty()) {
      range.end = size - 1;
    } else {
      const long long end = std::stoll(last);
      if (end < start) {
        range.satisfiable = false;
        return range;
      }
      range.end = std::min<std::size_t>(static_cast<std::size_t>(end),
                                       size - 1);
    }
  } catch (const std::exception &) {
    range.present = false;
  }
  return range;
}

drogon::HttpResponsePtr buildContent(const std::string &id,
                                     const std::string &range_header,
                                     const std::string &if_none_match,
                                     bool head_only) {
  (void)head_only;
  Entity entity;
  bool found = false;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto it = g_entities.find(id);
    if (it != g_entities.end()) {
      entity = it->second;
      found = true;
    }
  }
  if (!found) {
    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setStatusCode(drogon::k404NotFound);
    resp->setContentTypeString("application/json");
    resp->setBody("{\"erro\":true}");
    return resp;
  }
  if (entity.status != 200) {
    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setStatusCode(static_cast<drogon::HttpStatusCode>(entity.status));
    resp->setContentTypeString("application/json");
    resp->setBody("{\"erro\":true}");
    return resp;
  }

  const std::string &data = entity.bytes;
  const std::string etag = "\"" + id + "\"";
  if (!if_none_match.empty() && if_none_match == etag) {
    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setStatusCode(drogon::k304NotModified);
    resp->addHeader("ETag", etag);
    resp->addHeader("Accept-Ranges", "bytes");
    resp->setBody(std::string());
    return resp;
  }

  const ParsedRange range = parseRange(range_header, data.size());
  auto resp = drogon::HttpResponse::newHttpResponse();
  resp->setContentTypeString(entity.content_type);
  resp->addHeader("Accept-Ranges", "bytes");
  resp->addHeader("ETag", etag);

  if (range.present && !range.satisfiable) {
    resp->setStatusCode(drogon::k416RequestedRangeNotSatisfiable);
    resp->addHeader("Content-Range",
                    "bytes */" + std::to_string(data.size()));
    resp->setBody(std::string());
    return resp;
  }

  if (range.present) {
    const std::size_t length = range.end - range.start + 1;
    resp->setStatusCode(drogon::k206PartialContent);
    resp->addHeader("Content-Range",
                    "bytes " + std::to_string(range.start) + "-" +
                        std::to_string(range.end) + "/" +
                        std::to_string(data.size()));
    // Drogon 对 HEAD 自动保留头、丢弃体，因此统一设置 body 即可。
    resp->setBody(data.substr(range.start, length));
    return resp;
  }

  resp->setStatusCode(drogon::k200OK);
  resp->setBody(data);
  return resp;
}

drogon::HttpResponsePtr jsonOk() {
  auto resp = drogon::HttpResponse::newHttpResponse();
  resp->setStatusCode(drogon::k200OK);
  resp->setContentTypeString("application/json");
  resp->setBody("{\"ok\":true}");
  return resp;
}

drogon::HttpResponsePtr jsonError(int status, const std::string &message) {
  auto resp = drogon::HttpResponse::newHttpResponse();
  resp->setStatusCode(static_cast<drogon::HttpStatusCode>(status));
  resp->setContentTypeString("application/json");
  resp->setBody("{\"erro\":true,\"message\":\"" + message + "\"}");
  return resp;
}

} // namespace

int main(int argc, char **argv) {
  const int port = argc > 1 ? std::atoi(argv[1]) : 15666;
  resetDefaults();

  auto &app = drogon::app();
  app.addListener("127.0.0.1", static_cast<uint16_t>(port));
  app.setThreadNum(2);

  // 内容端点：延迟请求放到独立线程，避免阻塞事件循环。
  app.registerHandler(
      "/api/entities/{1}/content",
      [](const drogon::HttpRequestPtr &req,
         std::function<void(const drogon::HttpResponsePtr &)> &&callback,
         const std::string &id) {
        const bool head_only = req->isHead();
        int delay_ms = 0;
        {
          std::lock_guard<std::mutex> lock(g_mutex);
          const auto it = g_entities.find(id);
          if (it != g_entities.end()) {
            delay_ms = it->second.delay_ms;
          }
        }
        auto resp = buildContent(
            id, std::string(req->getHeader("range")),
            std::string(req->getHeader("if-none-match")), head_only);
        if (delay_ms > 0) {
          std::thread([callback = std::move(callback), resp, delay_ms]() mutable {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(delay_ms));
            callback(resp);
          }).detach();
          return;
        }
        callback(resp);
      },
      {drogon::Get, drogon::Head});

  app.registerHandler(
      "/__control/reset",
      [](const drogon::HttpRequestPtr &,
         std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
        resetDefaults();
        callback(jsonOk());
      },
      {drogon::Post});

  app.registerHandler(
      "/__control/entity/{1}",
      [](const drogon::HttpRequestPtr &req,
         std::function<void(const drogon::HttpResponsePtr &)> &&callback,
         const std::string &id) {
        Json::Value body;
        Json::CharReaderBuilder builder;
        std::string errors;
        const std::string raw(req->body());
        std::istringstream input(raw);
        if (!Json::parseFromStream(builder, input, &body, &errors) ||
            !body.isObject()) {
          callback(jsonError(400, "invalid json"));
          return;
        }
        std::lock_guard<std::mutex> lock(g_mutex);
        Entity &entity = g_entities[id];
        if (body.isMember("status")) {
          entity.status = body["status"].asInt();
        }
        if (body.isMember("delay_ms")) {
          entity.delay_ms = body["delay_ms"].asInt();
        }
        if (body.isMember("content_type")) {
          entity.content_type = body["content_type"].asString();
        }
        if (body.isMember("body")) {
          entity.bytes = body["body"].asString();
        }
        if (body.isMember("size")) {
          const auto size = static_cast<std::size_t>(body["size"].asUInt64());
          entity.bytes = patternBytes(size, 9);
        }
        callback(jsonOk());
      },
      {drogon::Post});

  std::cout << "[fake_storage] listening on 127.0.0.1:" << port << std::endl;
  app.run();
  return 0;
}