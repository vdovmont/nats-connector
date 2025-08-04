#include "NatsSubscriber.h"

NatsSubscriber::NatsSubscriber() : conn_(nullptr), sub_(nullptr) {}

NatsSubscriber::~NatsSubscriber() {
    disconnect();
}

bool NatsSubscriber::connect(const std::string& serverUrl) {
    natsStatus s = natsConnection_ConnectTo(&conn_, serverUrl.c_str());
    if (s != NATS_OK) {
        std::cerr << "NATS connect failed: " << natsStatus_GetText(s) << "\n";
        return false;
    }
    return true;
}

bool NatsSubscriber::subscribe(const std::string& subject, std::function<void(const std::string&)> handler) {
    if (!conn_) return false;

    callback_ = handler;

    natsStatus s = natsConnection_Subscribe(&sub_, conn_, subject.c_str(), onMessage, this);
    if (s != NATS_OK) {
        std::cerr << "Subscribe failed: " << natsStatus_GetText(s) << "\n";
        return false;
    }

    return true;
}

void NatsSubscriber::onMessage(natsConnection* nc, natsSubscription* sub, natsMsg* msg, void* closure) {
    NatsSubscriber* self = static_cast<NatsSubscriber*>(closure);
    if (self && self->callback_) {
        std::string data(natsMsg_GetData(msg), natsMsg_GetDataLength(msg));
        self->callback_(data);
    }
    natsMsg_Destroy(msg);
}

void NatsSubscriber::disconnect() {
    if (sub_) {
        natsSubscription_Destroy(sub_);
        sub_ = nullptr;
    }
    if (conn_) {
        natsConnection_Destroy(conn_);
        conn_ = nullptr;
    }
}