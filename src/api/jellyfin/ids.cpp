#include "ids.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>

namespace jellyfin {
namespace {

const std::string kServerId = "a1b2c3d4e5f60718293a4b5c6d7e8f90";
const std::string kUserId = "0123456789abcdef0123456789abcdef";
const std::string kUserName = "musicd";

std::string g_server_name = "musicd";

bool isHex(const std::string &s) {
  if (s.empty()) {
    return false;
  }
  for (char c : s) {
    if (!std::isxdigit(static_cast<unsigned char>(c))) {
      return false;
    }
  }
  return true;
}

} // namespace

std::string makeId(int kind, int id) {
  char buffer[33];
  std::snprintf(buffer, sizeof(buffer), "%08x%024x",
                static_cast<unsigned int>(kind),
                static_cast<unsigned int>(id));
  return std::string(buffer);
}

bool parseId(const std::string &guid, int &kind, int &id) {
  if (guid.size() != 32) {
    return false;
  }
  const std::string type_part = guid.substr(0, 8);
  const std::string id_part = guid.substr(8);
  if (!isHex(type_part) || !isHex(id_part)) {
    return false;
  }
  kind = static_cast<int>(std::strtol(type_part.c_str(), nullptr, 16));
  id = static_cast<int>(std::strtol(id_part.c_str(), nullptr, 16));
  return true;
}

const std::string &serverId() { return kServerId; }
const std::string &userId() { return kUserId; }
const std::string &userName() { return kUserName; }
const std::string &serverName() { return g_server_name; }

void setServerName(const std::string &name) {
  if (!name.empty()) {
    g_server_name = name;
  }
}

std::string musicViewId() { return makeId(kKindView, 1); }

std::string playlistsViewId() { return makeId(kKindView, 2); }

} // namespace jellyfin
