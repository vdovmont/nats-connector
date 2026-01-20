#include "nats_manager.h"

#include "logger.h"

NatsManager::NatsManager() : conn_(nullptr) {}

NatsManager::~NatsManager() { Disconnect(); }

bool NatsManager::Connect(const std::string& server_url) {
    natsStatus status = natsConnection_ConnectTo(&conn_, server_url.c_str());
    if (status != NATS_OK) {
        logger::log_error() << "NATS connect failed: " << natsStatus_GetText(status) << "\n";
        return false;
    }
    return true;
}

bool NatsManager::Publish(const std::string& subject, const nlohmann::json& message) {
    if (!conn_) {
        logger::log_error() << "Not connected to NATS server.\n";
        return false;
    }

    std::string msg_str = message.dump();
    natsStatus status = natsConnection_Publish(conn_, subject.c_str(), msg_str.c_str(), msg_str.size());
    if (status != NATS_OK) {
        logger::log_error() << "Publish failed: " << natsStatus_GetText(status) << "\n";
        return false;
    }
    return true;
}

bool NatsManager::Subscribe(const std::string& subject,
                            std::function<void(const std::string&, const nlohmann::json&)> handler) {
    if (!conn_) {
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
        return false;
    }

    subs_[subject] = sub;
    callbacks_[sub] = handler;
    return true;
}

bool NatsManager::Unsubscribe(const std::string& subject) {
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

    auto it = self->callbacks_.find(sub);
    if (it != self->callbacks_.end()) {
        std::string subject = natsMsg_GetSubject(msg);
        std::string data(natsMsg_GetData(msg), natsMsg_GetDataLength(msg));
        try {
            nlohmann::json json_data = nlohmann::json::parse(data);
            it->second(subject, json_data);
        } catch (const std::exception& e) {
            logger::log_error() << "Failed to parse JSON message: " << e.what() << "\n";
        }
    } else {
        logger::log_error() << "No callback found for subscription.\n";
    }
}

void NatsManager::Disconnect() {
    if (conn_) {
        natsConnection_Destroy(conn_);
        conn_ = nullptr;
    }
    // Unsubscribe all subscriptions
    for (auto& pair : callbacks_) {
        natsSubscription* sub = pair.first;
        if (sub) {
            natsSubscription_Unsubscribe(sub);  // stop receiving messages
            natsSubscription_Destroy(sub);      // free the subscription object
        }
    }
}