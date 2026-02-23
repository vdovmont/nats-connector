#include "nats_manager.h"

#include <chrono>

#include "logger.h"

namespace {
bool IsConnectionError(natsStatus status) {
    return status == NATS_CONNECTION_CLOSED || status == NATS_CONNECTION_DISCONNECTED || status == NATS_NO_SERVER ||
           status == NATS_STALE_CONNECTION;
}
}  // namespace

NatsManager::NatsManager() : conn_(nullptr), connected_(false) {}

NatsManager::~NatsManager() {
    StopReconnectLoop();
    Disconnect();
}

bool NatsManager::Connect(const std::string& server_url) {
    std::lock_guard<std::mutex> lock(conn_mutex_);
    if (connected_.load(std::memory_order_acquire) && conn_) {
        return true;
    }
    if (conn_) {
        DisconnectLocked();
    }
    natsStatus status = natsConnection_ConnectTo(&conn_, server_url.c_str());
    if (status != NATS_OK) {
        logger::log_error() << "NATS connect failed: " << natsStatus_GetText(status) << "\n";
        return false;
    }
    connected_.store(true, std::memory_order_release);
    return true;
}

bool NatsManager::IsConnected() { return CheckConnection(); }

bool NatsManager::CheckConnection() {
    std::lock_guard<std::mutex> lock(conn_mutex_);
    if (!conn_) {
        connected_.store(false, std::memory_order_release);
        return false;
    }
    natsConnStatus status = natsConnection_Status(conn_);
    if (status != NATS_CONN_STATUS_CONNECTED) {
        connected_.store(false, std::memory_order_release);
        if (status == NATS_CONN_STATUS_CLOSED) {
            DisconnectLocked();
        }
        return false;
    }
    connected_.store(true, std::memory_order_release);
    return true;
}

void NatsManager::StartReconnectLoop(const std::string& server_url,
                                     std::function<void()> on_connected,
                                     std::function<void()> on_disconnected) {
    StopReconnectLoop();
    server_url_ = server_url;
    stop_reconnect_.store(false, std::memory_order_release);

    reconnect_thread_ = std::thread([this, on_connected, on_disconnected]() {
        bool was_connected = false;
        while (!stop_reconnect_.load(std::memory_order_acquire)) {
            bool is_connected = CheckConnection();
            if (!is_connected) {
                if (was_connected && on_disconnected) {
                    on_disconnected();
                }
                if (Connect(server_url_)) {
                    if (on_connected) {
                        on_connected();
                    }
                    was_connected = true;
                } else {
                    was_connected = false;
                }
            } else {
                if (!was_connected && on_connected) {
                    on_connected();
                }
                was_connected = true;
            }
            std::this_thread::sleep_for(std::chrono::seconds(5));
        }
    });
}

void NatsManager::StopReconnectLoop() {
    stop_reconnect_.store(true, std::memory_order_release);
    if (reconnect_thread_.joinable()) {
        reconnect_thread_.join();
    }
}

bool NatsManager::Publish(const std::string& subject, const nlohmann::json& message) {
    std::lock_guard<std::mutex> lock(conn_mutex_);
    if (!connected_.load(std::memory_order_acquire) || !conn_) {
        logger::log_error() << "Not connected to NATS server.\n";
        return false;
    }

    std::string msg_str = message.dump();
    natsStatus status = natsConnection_Publish(conn_, subject.c_str(), msg_str.c_str(), msg_str.size());
    if (status != NATS_OK) {
        logger::log_error() << "Publish failed: " << natsStatus_GetText(status) << "\n";
        if (IsConnectionError(status)) {
            connected_.store(false, std::memory_order_release);
            DisconnectLocked();
        }
        return false;
    }
    return true;
}

bool NatsManager::Subscribe(const std::string& subject,
                            std::function<void(const std::string&, const nlohmann::json&)> handler) {
    std::lock_guard<std::mutex> lock(conn_mutex_);
    if (!connected_.load(std::memory_order_acquire) || !conn_) {
        logger::log_error() << "Not connected to NATS server.\n";
        return false;
    }

    natsSubscription* sub = nullptr;
    natsStatus status = natsConnection_Subscribe(&sub, conn_, subject.c_str(), Callback, this);
    // We pass 'this' so that, inside the callback, we can get back the current object instance (NatsSubscriber*).
    // This gives us access to private member like callback_ or other methods.
    // for more information - google "bridging C callbacks with C++ member functions".
    // Basically in our case - it allows us pass down whatever function we want to deal with the messages.
    if (status != NATS_OK) {
        logger::log_error() << "Subscribe failed: " << natsStatus_GetText(status) << "\n";
        if (IsConnectionError(status)) {
            connected_.store(false, std::memory_order_release);
            DisconnectLocked();
        }
        return false;
    }

    subs_[subject] = sub;
    callbacks_[sub] = handler;
    return true;
}

bool NatsManager::Unsubscribe(const std::string& subject) {
    std::lock_guard<std::mutex> lock(conn_mutex_);
    auto it = subs_.find(subject);
    if (it == subs_.end()) {
        return false;
    }

    natsSubscription* sub = it->second;
    natsStatus status = natsSubscription_Unsubscribe(sub);
    if (status != NATS_OK) {
        logger::log_error() << "Unsubscribe failed: " << natsStatus_GetText(status) << "\n";
        return false;
    }

    callbacks_.erase(sub);
    subs_.erase(it);
    return true;
}

void NatsManager::Callback(natsConnection* nc, natsSubscription* sub, natsMsg* msg, void* closure) {
    MsgGuard guard{msg};  // will auto-destroy msg at scope exit
    NatsManager* self = static_cast<NatsManager*>(closure);

    if (!self) return;

    std::function<void(const std::string&, const nlohmann::json&)> handler;
    {
        std::lock_guard<std::mutex> lock(self->conn_mutex_);
        auto it = self->callbacks_.find(sub);
        if (it != self->callbacks_.end()) {
            handler = it->second;
        }
    }

    if (!handler) {
        logger::log_error() << "No callback found for subscription.\n";
        return;
    }

    std::string subject = natsMsg_GetSubject(msg);
    std::string data(natsMsg_GetData(msg), natsMsg_GetDataLength(msg));
    try {
        nlohmann::json json_data = nlohmann::json::parse(data);
        handler(subject, json_data);
    } catch (const std::exception& e) {
        logger::log_error() << "Failed to parse JSON message: " << e.what() << "\n";
    }
}

void NatsManager::Disconnect() {
    std::lock_guard<std::mutex> lock(conn_mutex_);
    DisconnectLocked();
}

void NatsManager::DisconnectLocked() {
    if (conn_) {
        natsConnection_Destroy(conn_);
        conn_ = nullptr;
    }
    connected_.store(false, std::memory_order_release);
    // Unsubscribe all subscriptions
    for (auto& pair : callbacks_) {
        natsSubscription* sub = pair.first;
        if (sub) {
            natsSubscription_Unsubscribe(sub);  // stop receiving messages
            natsSubscription_Destroy(sub);      // free the subscription object
        }
    }
    callbacks_.clear();
    subs_.clear();
}