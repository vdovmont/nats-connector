#pragma once

#include "NatsManager.h"

class MathPicker {
    public:
        MathPicker() = default;
        ~MathPicker() = default;

        bool initialize(const std::string& serverUrl, const std::string& subject);
        void onMessageTrigger(const std::string& subject, const std::string& message);

    private:
        NatsManager manager_;
};