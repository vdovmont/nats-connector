#pragma once

#include <chrono>
#include <nlohmann/json.hpp>
#include <string>

class RequestIdBuilder {
  public:
    std::string Build(const nlohmann::json& request) const;
    std::string Build(const nlohmann::json& request, const std::chrono::system_clock::time_point& timestamp) const;

  private:
    static std::string GenerateTimestamp(const std::chrono::system_clock::time_point& timestamp);
    static std::string ExtractString(const nlohmann::json& request, const std::string& key);
    static std::string ExtractTaskNumber(const std::string& task_id);
};
