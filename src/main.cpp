#include <Poco/Net/Net.h>

#include "http_handler.h"
#include "logger.h"

namespace {
struct NetGuard {
    NetGuard() { Poco::Net::initializeNetwork(); }
    ~NetGuard() { Poco::Net::uninitializeNetwork(); }
};
}  // namespace

int main(int argc, char** argv) {
    try {
        NetGuard net_guard;
        ServerApp app;
        return app.run(argc, argv);
    } catch (const std::exception& ex) {
        logger::log_error() << "Unhandled exception: " << ex.what() << std::endl;
        return 1;
    } catch (...) {
        logger::log_error() << "Unhandled unknown exception." << std::endl;
        return 1;
    }
}