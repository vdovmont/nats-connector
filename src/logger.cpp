#include "logger.h"

#include <chrono>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace logger {

static bool g_latest_log_truncated = false;

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

static std::string CreateDateLogFilePath() {
    const std::string date_file = GetDateFileName();
    const std::filesystem::path dir_path = std::filesystem::path("logs");
    std::filesystem::create_directories(dir_path);
    return (dir_path / date_file).string();
}

static std::string CreateLatestLogFilePath() {
    const std::filesystem::path dir_path = std::filesystem::path("logs");
    std::filesystem::create_directories(dir_path);
    const std::filesystem::path latest_path = dir_path / "latest.log";
    return latest_path.string();
}

LogStream::LogStream(const std::string& filename, std::ostream& stream) :
    file_(filename, std::ios::out | std::ios::app), stream_(&stream) {}

LogStream::LogStream(const std::string& filename, const std::string& latest_filename, std::ostream& stream) :
    file_(filename, std::ios::out | std::ios::app),
    latest_file_(latest_filename, std::ios::out | (g_latest_log_truncated ? std::ios::app : std::ios::trunc)),
    stream_(&stream) {
    g_latest_log_truncated = true;
}

LogStream::~LogStream() {
    if (file_.is_open()) {
        file_.flush();
    }
    if (latest_file_.is_open()) {
        latest_file_.flush();
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
    if (latest_file_.is_open()) {
        latest_file_ << stamp.str();
    }
    at_line_start_ = false;
}

LogStream& LogStream::operator<<(std::ostream& (*manip)(std::ostream&)) {
    WritePrefixIfNeeded();
    manip(*stream_);
    if (file_.is_open()) {
        manip(file_);
    }
    if (latest_file_.is_open()) {
        manip(latest_file_);
    }
    if (manip == static_cast<std::ostream& (*)(std::ostream&)>(std::endl<char, std::char_traits<char>>)) {
        at_line_start_ = true;
    }
    return *this;
}

LogStream log(const std::string& filename) { return LogStream(filename, std::cout); }

LogStream log() { return LogStream(CreateDateLogFilePath(), CreateLatestLogFilePath(), std::cout); }

LogStream log_error(const std::string& filename) { return LogStream(filename, std::cerr); }

LogStream log_error() { return LogStream(CreateDateLogFilePath(), CreateLatestLogFilePath(), std::cerr); }

}  // namespace logger