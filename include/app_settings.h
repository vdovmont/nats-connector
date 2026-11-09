#pragma once

#include <chrono>
#include <string>

struct AppSettings {
    std::string nats_url = "nats://localhost:4222";
    std::chrono::seconds nats_reconnect_interval{5};
    std::chrono::seconds queue_entry_timeout{30};
    std::chrono::seconds mathcore_instance_ttl{5};
    std::chrono::seconds mathcore_assignment_grace{5};
    std::chrono::seconds mathcore_instance_release{10};
    std::chrono::seconds mathcore_health_check_interval{1};
};

AppSettings LoadAppSettings(const std::string& path = "settings.ini");
