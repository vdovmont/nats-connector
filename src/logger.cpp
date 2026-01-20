#include "logger.h"

#include <chrono>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace logger {

static std::string GetDateFileName() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t now_time = std::chrono::system_clock::to_time_t(now);
    std::tm local_tm{};
#if defined(_WIN32)
    localtime_s(&local_tm, &now_time);
#else
    localtime_r(&now_time, &local_tm);
#endif
    std::ostringstream filename;
    filename << std::put_time(&local_tm, "%Y-%m-%d") << ".log";
    return filename.str();
}

static std::string CreateLogFilePath() {
    const std::string date_file = GetDateFileName();
    const std::filesystem::path dir_path = std::filesystem::path("logs");
    std::filesystem::create_directories(dir_path);
    return (dir_path / date_file).string();
}

LogStream::LogStream(const std::string& filename, std::ostream& stream) :
    file_(filename, std::ios::out | std::ios::app), stream_(&stream) {}

LogStream::~LogStream() {
    if (file_.is_open()) {
        file_.flush();
    }
    stream_->flush();
}

void LogStream::WritePrefixIfNeeded() {
    if (!at_line_start_) {
        return;
    }
    const auto now = std::chrono::system_clock::now();
    const std::time_t now_time = std::chrono::system_clock::to_time_t(now);
    std::tm local_tm{};
#if defined(_WIN32)
    localtime_s(&local_tm, &now_time);
#else
    localtime_r(&now_time, &local_tm);
#endif
    std::ostringstream stamp;
    stamp << "[" << std::put_time(&local_tm, "%H:%M:%S") << "] ";
    (*stream_) << stamp.str();
    if (file_.is_open()) {
        file_ << stamp.str();
    }
    at_line_start_ = false;
}

LogStream& LogStream::operator<<(std::ostream& (*manip)(std::ostream&)) {
    WritePrefixIfNeeded();
    manip(*stream_);
    if (file_.is_open()) {
        manip(file_);
    }
    if (manip == static_cast<std::ostream& (*)(std::ostream&)>(std::endl<char, std::char_traits<char>>)) {
        at_line_start_ = true;
    }
    return *this;
}

LogStream log(const std::string& filename) { return LogStream(filename, std::cout); }

LogStream log() { return LogStream(CreateLogFilePath(), std::cout); }

LogStream log_error(const std::string& filename) { return LogStream(filename, std::cerr); }

LogStream log_error() { return LogStream(CreateLogFilePath(), std::cerr); }

}  // namespace logger