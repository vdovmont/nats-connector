#include "NatsPublisher.h"
#include "NatsSubscriber.h"
#include <thread>
#include <chrono>

// For now as a stopgap, but this function should prevent process from exiting and maybe checking for NatsSubscriber health (connection) (?). In the future should be replaced.
void sleepingFunction() {
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

int main() {
    std::string serverUrl = "nats://localhost:4222"; //example server URL
    std::string clinetsChannel = "clients.*"; //example of common channel for clients
    NatsSubscriber subscriber;

    bool status = subscriber.initialize(serverUrl, clinetsChannel);

    if (!status) {
        std::cerr << "Failed to initialize subscriber.\n";
        return 1;
    } /*else { sleepingFunction(); // commented for the sake of testing (next block of code). otherwise here should be called a function for keeping the process alive.
    }*/

    // for testing purposes
    // testing should be done if we already have nats-service running up
    NatsPublisher publisher;
    std::string someClient = "clients.someClient";
    publisher.connect(serverUrl);
    publisher.publish(someClient, "Hello from NatsPublisher!");

    return 0;
}