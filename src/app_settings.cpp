#include "app_settings.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "logger.h"

namespace {

constexpr std::array<std::pair<const char*, const char*>, 7> kDefaultSettings = {{
    {"nats_url", "nats://localhost:4222"},
    {"nats_reconnect_interval_sec", "5"},
    {"queue_entry_timeout_sec", "30"},
    {"mathcore_instance_ttl_sec", "5"},
    {"mathcore_assignment_grace_sec", "5"},
    {"mathcore_instance_release_sec", "10"},
    {"mathcore_health_check_interval_sec", "1"},
}};

void WriteDefaultSettings(std::ostream& output) {
    for (const auto& [key, value] : kDefaultSettings) {
        output << key << '=' << value << '\n';
    }
}

std::string Trim(std::string value) {
    auto not_space = [](unsigned char ch) {
        return !std::isspace(ch);
    };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
    return value;
}

std::unordered_map<std::string, std::string> ReadValues(const std::string& path) {
    std::unordered_map<std::string, std::string> values;
    std::ifstream input(path);
    std::string line;
    while (std::getline(input, line)) {
        line = Trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        const size_t pos = line.find('=');
        if (pos == std::string::npos) {
            logger::log_error() << "Ignoring malformed settings.ini line: " << line << std::endl;
            continue;
        }
        values[Trim(line.substr(0, pos))] = Trim(line.substr(pos + 1));
    }
    return values;
}

void AddMissingDefaults(const std::string& path, std::unordered_map<std::string, std::string>& values) {
    std::vector<std::pair<const char*, const char*>> missing;
    for (const auto& setting : kDefaultSettings) {
        if (values.find(setting.first) == values.end()) {
            missing.push_back(setting);
        }
    }
    if (missing.empty()) {
        return;
    }

    std::ofstream output(path, std::ios::app);
    if (!output.is_open()) {
        logger::log_error() << "Failed to add missing defaults to " << path << std::endl;
        return;
    }

    output << '\n';
    for (const auto& [key, value] : missing) {
        output << key << '=' << value << '\n';
        values[key] = value;
        logger::log() << "Added missing setting " << key << '=' << value << " to " << path << std::endl;
    }
}

std::optional<int> ParseInt(const std::unordered_map<std::string, std::string>& values,
                            const std::string& key,
                            int min_value) {
    auto it = values.find(key);
    if (it == values.end()) {
        logger::log_error() << "settings.ini missing " << key << "; using default" << std::endl;
        return std::nullopt;
    }

    try {
        size_t parsed = 0;
        int value = std::stoi(it->second, &parsed);
        if (parsed != it->second.size() || value < min_value) {
            throw std::invalid_argument("invalid range");
        }
        return value;
    } catch (const std::exception&) {
        logger::log_error() << "settings.ini invalid " << key << "=" << it->second << "; using default" << std::endl;
        return std::nullopt;
    }
}

}  // namespace

AppSettings LoadAppSettings(const std::string& path) {
    AppSettings settings;
    {
        std::ifstream probe(path);
        if (!probe.good()) {
            std::ofstream output(path, std::ios::trunc);
            WriteDefaultSettings(output);
            logger::log() << "Created default settings.ini" << std::endl;
            return settings;
        }
    }

    auto values = ReadValues(path);
    AddMissingDefaults(path, values);
    if (auto it = values.find("nats_url"); it != values.end() && !it->second.empty()) {
        settings.nats_url = it->second;
    } else {
        logger::log_error() << "settings.ini missing or empty nats_url; using default" << std::endl;
    }
    if (auto value = ParseInt(values, "nats_reconnect_interval_sec", 1)) {
        settings.nats_reconnect_interval = std::chrono::seconds(*value);
    }
    if (auto value = ParseInt(values, "queue_entry_timeout_sec", 1)) {
        settings.queue_entry_timeout = std::chrono::seconds(*value);
    }
    if (auto value = ParseInt(values, "mathcore_instance_ttl_sec", 1)) {
        settings.mathcore_instance_ttl = std::chrono::seconds(*value);
    }
    if (auto value = ParseInt(values, "mathcore_assignment_grace_sec", 0)) {
        settings.mathcore_assignment_grace = std::chrono::seconds(*value);
    }
    if (auto value = ParseInt(values, "mathcore_instance_release_sec", 1)) {
        settings.mathcore_instance_release = std::chrono::seconds(*value);
    }
    if (auto value = ParseInt(values, "mathcore_health_check_interval_sec", 1)) {
        settings.mathcore_health_check_interval = std::chrono::seconds(*value);
    }
    return settings;
}
