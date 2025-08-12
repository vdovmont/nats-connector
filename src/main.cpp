#include <chrono>
#include <thread>

#include "math_picker.h"

// For now as a stopgap, but this function should prevent process from exiting and maybe checking for NatsSubscriber health (connection) (?).
// In the future should be replaced.
void SleepingFunction() {
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

int main() {
    std::string server_url = "nats://localhost:4222";  //example server URL
    std::string clients_channel = "Clients.*";         //example of common channel for clients
    MathPicker math_picker;

    bool status = math_picker.Initialize(server_url, clients_channel);

    if (!status) {
        std::cerr << "Failed to initialize NATS service.\n";
        return 1;
    } else {
        SleepingFunction();
    }

    return 0;
}