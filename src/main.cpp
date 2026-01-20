#include "http_handler.h"
#include "logger.h"

int main(int argc, char** argv) {
    try {
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