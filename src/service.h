#ifndef SERVICE_H
#define SERVICE_H

#include <json/json.h>
#include <optional>
#include <string>

class Service {
public:
  Service();
  std::optional<Json::Value> query(const Json::Value &json);
  std::optional<Json::Value> trackById(int id);
  std::optional<Json::Value> albumById(int id);
  std::optional<Json::Value> artistById(int id);

private:
  std::optional<Json::Value> home(const Json::Value &json);
  std::optional<Json::Value> tag(const Json::Value &json);
  std::optional<Json::Value> album(const Json::Value &json);
  std::optional<Json::Value> artist(const Json::Value &json);
  std::optional<Json::Value> search(const Json::Value &json);
};

#endif
