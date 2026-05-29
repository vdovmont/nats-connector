#include "request_id_builder.h"

#include <chrono>
#include <iomanip>
#include <sstream>

std::string RequestIdBuilder::Build(const nlohmann::json& request) const {
    std::string company = ExtractString(request, "company");
    if (company.empty()) {
        company = "0";
    }

    std::string task_number = ExtractTaskNumber(ExtractString(request, "task_id"));
    if (task_number.empty()) {
        task_number = "0";
    }

    return company + "-" + task_number + "-" + GenerateTimestamp();
}

std::string RequestIdBuilder::GenerateTimestamp() {
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);

    std::ostringstream oss;
    std::tm timeinfo{};
#if defined(_WIN32)
    localtime_s(&timeinfo, &time_t_now);
#else
    localtime_r(&time_t_now, &timeinfo);
#endif
    oss << std::put_time(&timeinfo, "%Y%m%d_%H%M%S");

    return oss.str();
}

std::string RequestIdBuilder::ExtractString(const nlohmann::json& request, const std::string& key) {
    if (!request.contains(key) || !request[key].is_string()) {
        return "";
    }
    return request[key].get<std::string>();
}

std::string RequestIdBuilder::ExtractTaskNumber(const std::string& task_id) {
    const size_t end = task_id.find_last_not_of('/');
    if (end == std::string::npos) {
        return "";
    }

    const size_t start = task_id.find_last_of('/', end);
    if (start == std::string::npos) {
        return task_id.substr(0, end + 1);
    }

    return task_id.substr(start + 1, end - start);
}
