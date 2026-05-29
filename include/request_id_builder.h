#pragma once

#include <string>

#include <nlohmann/json.hpp>

class RequestIdBuilder {
  public:
    std::string Build(const nlohmann::json& request) const;

  private:
    static std::string GenerateTimestamp();
    static std::string ExtractString(const nlohmann::json& request, const std::string& key);
    static std::string ExtractTaskNumber(const std::string& task_id);
};
