// 测试用网易云 API：只实现 musicd 真正会打到的那几个端点。
//
//   build/bin/test/fake_netease          # 自动选空闲端口
//
// 端点：
//   GET /login/status        已登录、VIP 会员（让库健康、member_active 为真）
//   GET /song/detail?ids=    曲目元数据；版权不可用的曲目 fee=4
//   GET /song/url/v1?id=     播放地址；版权不可用返回空 data
//   GET|HEAD /audio/{id}     音频字节（按 id 给不同长度，避开本地 wav 的 1 MiB）
//
// 未实现的路径回 404，并把方法+路径打到 stderr：漏端点要能被测试发现。
// 端口由内核分配后打印 "listening <port>"，harness 据此拿到端口。

#include "fixture_ids.h"

#include <drogon/drogon.h>

#include <json/json.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>

namespace {

// 找一个空闲端口：bind 到 0 让内核选，拿到端口后立刻释放。
// Drogon 的 addListener 不接受 0，只能这样探。
std::uint16_t freePort() {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    std::cerr << "[fake_netease] socket() failed" << std::endl;
    std::exit(1);
  }
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  if (::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
    std::cerr << "[fake_netease] bind() failed" << std::endl;
    ::close(fd);
    std::exit(1);
  }
  socklen_t len = sizeof(addr);
  if (::getsockname(fd, reinterpret_cast<sockaddr *>(&addr), &len) != 0) {
    std::cerr << "[fake_netease] getsockname() failed" << std::endl;
    ::close(fd);
    std::exit(1);
  }
  ::close(fd);
  return ::ntohs(addr.sin_port);
}

drogon::HttpResponsePtr jsonResponse(const std::string &body) {
  auto resp = drogon::HttpResponse::newHttpResponse();
  resp->setStatusCode(drogon::k200OK);
  resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
  resp->setBody(body);
  return resp;
}

drogon::HttpResponsePtr notFound(const std::string &method,
                                 const std::string &path) {
  std::cerr << "[fake_netease] unhandled " << method << " " << path
            << std::endl;
  auto resp = drogon::HttpResponse::newHttpResponse();
  resp->setStatusCode(drogon::k404NotFound);
  resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
  resp->setBody(R"({"code":404,"msg":"unhandled endpoint"})");
  return resp;
}

bool isBlockedSong(const std::string &song_id) {
  return song_id == fixture::kNeteaseBlockedSongId;
}

// 不同 song id 给不同长度与内容，避免测试分不清拿到的是哪一份。
bool isEntitySong(const std::string &song_id) {
  return song_id == fixture::kNeteaseEntitySongId;
}

std::int64_t audioSizeOf(const std::string &song_id) {
  return isEntitySong(song_id) ? fixture::kNeteaseEntityAudioSize
                               : fixture::kNeteaseAudioSize;
}

std::string audioBytesOf(const std::string &song_id) {
  return isEntitySong(song_id) ? fixture::kNeteaseEntityAudioBytes()
                               : fixture::kNeteaseAudioBytes();
}

// /song/detail 的 ids 是逗号分隔；夹具只关心第一个。
std::string firstId(const std::string &ids) {
  const auto comma = ids.find(',');
  return comma == std::string::npos ? ids : ids.substr(0, comma);
}

} // namespace

int main() {
  const std::uint16_t port = freePort();
  const std::string base = "http://127.0.0.1:" + std::to_string(port);

  auto &app = drogon::app();
  app.addListener("127.0.0.1", port);
  app.setThreadNum(2);

  app.registerHandler(
      "/login/status",
      [](const drogon::HttpRequestPtr &,
         std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
        callback(jsonResponse(
            R"({"data":{"code":200,"account":{"id":17688534379,"vipType":12},"profile":{"nickname":"fixture"}}})"));
      },
      {drogon::Get});

  app.registerHandler(
      "/song/detail",
      [](const drogon::HttpRequestPtr &req,
         std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
        const std::string song_id = firstId(req->getParameter("ids"));
        if (song_id.empty()) {
          callback(jsonResponse(R"({"code":200,"songs":[]})"));
          return;
        }
        Json::Value song;
        song["id"] = static_cast<Json::Int64>(std::atoll(song_id.c_str()));
        song["name"] = "fixture song " + song_id;
        song["dt"] = 210000;
        song["fee"] = isBlockedSong(song_id) ? 4 : 0;
        song["no"] = 1;
        song["ar"] = Json::Value(Json::arrayValue);
        song["al"] = Json::Value(Json::objectValue);
        song["alia"] = Json::Value(Json::arrayValue);
        Json::Value root;
        root["code"] = 200;
        root["songs"] = Json::Value(Json::arrayValue);
        root["songs"].append(song);
        Json::StreamWriterBuilder writer;
        writer["indentation"] = "";
        callback(jsonResponse(Json::writeString(writer, root)));
      },
      {drogon::Get});

  app.registerHandler(
      "/song/url/v1",
      [&base](const drogon::HttpRequestPtr &req,
              std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
        const std::string song_id = req->getParameter("id");
        if (song_id.empty() || isBlockedSong(song_id)) {
          // 版权不可用：url 为空，客户端据此判 PermissionDenied。
          callback(jsonResponse(R"({"code":200,"data":[]})"));
          return;
        }
        Json::Value entry;
        entry["id"] = static_cast<Json::Int64>(std::atoll(song_id.c_str()));
        entry["url"] = base + "/audio/" + song_id;
        entry["type"] = "mp3";
        entry["br"] = 320000;
        entry["size"] = static_cast<Json::Int64>(audioSizeOf(song_id));
        entry["sr"] = 44100;
        entry["level"] = "exhigh";
        entry["expi"] = 3600;
        entry["fee"] = 0;
        entry["code"] = 200;
        entry["time"] = 210000;
        Json::Value root;
        root["code"] = 200;
        root["data"] = Json::Value(Json::arrayValue);
        root["data"].append(entry);
        Json::StreamWriterBuilder writer;
        writer["indentation"] = "";
        callback(jsonResponse(Json::writeString(writer, root)));
      },
      {drogon::Get});

  app.registerHandler(
      "/audio/{1}",
      [](const drogon::HttpRequestPtr &req,
         std::function<void(const drogon::HttpResponsePtr &)> &&callback,
         const std::string &id) {
        auto resp = drogon::HttpResponse::newHttpResponse();
        resp->setStatusCode(drogon::k200OK);
        resp->setContentTypeCode(drogon::CT_AUDIO_MPEG);
        // 忽略 Range：客户端接受 200，会从头发读。
        resp->setBody(audioBytesOf(id));
        if (req->method() == drogon::Head) {
          resp->setBody("");
        }
        callback(resp);
      },
      {drogon::Get, drogon::Head});

  // 兜底：没实现的路径要打到 stderr，漏端点才会被测试发现。
  // 用正则兜底而不是 "/{1}" 这类路径模板：drogon 不接受只有参数的根级模板。
  auto fallback =
      [](const drogon::HttpRequestPtr &req,
         std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
        callback(notFound(req->methodString(), req->path()));
      };
  app.registerHandlerViaRegex("/.*", fallback,
                             {drogon::Get, drogon::Post, drogon::Put,
                              drogon::Delete});

  std::cout << "listening " << port << std::endl;
  app.run();
  return 0;
}