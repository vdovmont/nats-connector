#include <gtest/gtest.h>

#include <chrono>
#include <thread>

#include "nats_manager.h"
#include "std_err_capture.h"

class NatsManagerIntegrationTest : public ::testing::Test {
  protected:
    StderrCapture stderr_capture_;
    NatsManager nats_;
    std::string server_url = "nats://localhost:4222";
    std::string test_channel = "test.subject";

    void SetUp() override {}
    void TearDown() override {}
};

TEST_F(NatsManagerIntegrationTest, Connect) {
    ASSERT_TRUE(nats_.Connect(server_url)) << "Failed to connect to NATS. Stderr:\n" << stderr_capture_.Output();
}

TEST_F(NatsManagerIntegrationTest, Publishing) {
    std::string sending_message = "Hello NATS!";

    ASSERT_TRUE(nats_.Connect(server_url)) << "Failed to connect to NATS. Stderr:\n" << stderr_capture_.Output();
    ASSERT_TRUE(nats_.Publish(test_channel, sending_message)) << "Failed to publish message. Stderr:\n"
                                                              << stderr_capture_.Output();
}

TEST_F(NatsManagerIntegrationTest, Subscribe) {
    std::string received_subject;
    std::string received_message;
    std::string sending_message = "Hello NATS!";

    ASSERT_TRUE(nats_.Connect(server_url)) << "Failed to connect to NATS. Stderr:\n" << stderr_capture_.Output();
    ASSERT_TRUE(nats_.Subscribe(test_channel,
                                [&](const std::string& subject, const std::string& message) {
                                    received_subject = subject;
                                    received_message = message;
                                }))
        << "Failed to subscribe. Stderr:\n"
        << stderr_capture_.Output();

    ASSERT_TRUE(nats_.Publish(test_channel, sending_message)) << "Failed to publish message. Stderr:\n"
                                                              << stderr_capture_.Output();

    // Since subscribe callbacks are async, wait a little to receive the message.
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    EXPECT_EQ(received_subject, test_channel);
    EXPECT_EQ(received_message, sending_message);
}

TEST_F(NatsManagerIntegrationTest, Disconnect) {
    ASSERT_TRUE(nats_.Connect(server_url)) << "Failed to connect to NATS. Stderr:\n" << stderr_capture_.Output();
    ASSERT_TRUE(nats_.Subscribe(test_channel, [&](const std::string& subject, const std::string& message) {}))
        << "Failed to subscribe. Stderr:\n"
        << stderr_capture_.Output();
    ASSERT_NE(nats_.get_connection(), nullptr);
    ASSERT_NE(nats_.get_subscription(), nullptr);
    nats_.Disconnect();
    EXPECT_EQ(nats_.get_connection(), nullptr);
    EXPECT_EQ(nats_.get_subscription(), nullptr);
}