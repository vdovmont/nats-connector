#pragma once

#include "nats_manager.h"

class MathPicker {
  public:
    MathPicker() = default;
    ~MathPicker() = default;

    bool Initialize(const std::string& serverUrl, const std::string& subject);
    void OnMessageTrigger(const std::string& subject, const std::string& message);

  private:
    NatsManager manager_;
};