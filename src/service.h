#ifndef SERVICE_H
#define SERVICE_H

#include "crow/json.h"
#include <optional>
#include <string>

class Service {
public:
  Service();
  std::optional<crow::json::wvalue> query(const crow::json::rvalue &json);
  std::optional<crow::json::wvalue> trackById(int id);
  std::optional<crow::json::wvalue> albumById(int id);
  std::optional<crow::json::wvalue> artistById(int id);

private:
  std::optional<crow::json::wvalue> home(const crow::json::rvalue &json);
  std::optional<crow::json::wvalue> tag(const crow::json::rvalue &json);
  std::optional<crow::json::wvalue> album(const crow::json::rvalue &json);
  std::optional<crow::json::wvalue> artist(const crow::json::rvalue &json);
  std::optional<crow::json::wvalue> search(const crow::json::rvalue &json);
};

#endif
