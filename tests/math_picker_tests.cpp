#include <gtest/gtest.h>

#include <chrono>
#include <thread>

#include "math_picker.h"
#include "nats_manager.h"
#include "std_err_capture.h"

class MathPickerIntegrationTest : public ::testing::Test {
  protected:
    StderrCapture stderr_capture_;
    MathPicker math_picker_;
    NatsManager nats_;
    std::string server_url = "nats://localhost:4222";
    std::string test_channel = "test.subject";

    void SetUp() override {}
    void TearDown() override {}
};

TEST_F(MathPickerIntegrationTest, Initialize) {
    ASSERT_TRUE(math_picker_.Initialize(server_url, test_channel)) << "Failed to initialize with error:\n"
                                                                   << stderr_capture_.Output();
}

TEST_F(MathPickerIntegrationTest, OnMessageTrigger) {
    std::string received_message;
    std::string response_message = "Your ticket is: someNumber, and message is:\n";
    std::string sending_message = "something";

    ASSERT_TRUE(nats_.Connect(server_url)) << "Failed to connect to NATS. Stderr:\n" << stderr_capture_.Output();
    ASSERT_TRUE(
        nats_.Subscribe("Answers." + test_channel,
                        [&](const std::string& subject, const std::string& message) { received_message = message; }))
        << "Failed to subscribe. Stderr:\n"
        << stderr_capture_.Output();

    ASSERT_TRUE(math_picker_.Initialize(server_url, test_channel)) << "Failed to initialize with error:\n"
                                                                   << stderr_capture_.Output();
    math_picker_.OnMessageTrigger(test_channel, sending_message);

    // Since subscribe callbacks are async, wait a little to receive the message.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    EXPECT_EQ(received_message, response_message + sending_message + "\n");
}