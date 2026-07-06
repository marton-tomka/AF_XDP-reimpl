// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Marton Tomka

#pragma once

#include <cstdint>
#include <cstring>
#include <expected>
#include <format>
#include <print>
#include <source_location>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <utility>

namespace afxdp {
// gcc doesn't like std::hardware_destructive_interference_size
constexpr std::size_t CACHE_SIZE = 64;

struct AfxdpError {
    int code;
    std::string context;

    [[nodiscard]] std::string message() const {
        if (code != 0) return std::format("{}: {} (errno={})", context, std::strerror(code), code);
        return context;
    }
};

[[nodiscard]] inline AfxdpError make_errno_error(
    std::string_view context, std::source_location loc = std::source_location::current()) {
    return AfxdpError{
        .code = errno,
        .context = std::format(
            "{}:{} in {}: {}", loc.file_name(), loc.line(), loc.function_name(), context)};
}

[[nodiscard]] inline AfxdpError make_logic_error(
    std::string_view context, std::source_location loc = std::source_location::current()) {
    return AfxdpError{
        .code = 0,
        .context = std::format(
            "{}:{} in {}: {}", loc.file_name(), loc.line(), loc.function_name(), context)};
}

template<typename T>
using expect = std::expected<T, AfxdpError>;
using UNexpected = std::unexpected<AfxdpError>;

class FileDesc {
public:
    explicit FileDesc(int raw) noexcept
        : fd_(raw) {}

    FileDesc() noexcept = default;

    FileDesc(const FileDesc&) = delete;
    FileDesc& operator=(const FileDesc&) = delete;

    FileDesc(FileDesc&& o) noexcept
        : fd_(std::exchange(o.fd_, -1)) {}
    FileDesc& operator=(FileDesc&& o) noexcept {
        if (this != &o) {
            reset();
            fd_ = std::exchange(o.fd_, -1);
        }
        return *this;
    }

    ~FileDesc() { reset(); }

    void reset() noexcept {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

    [[nodiscard]] int get() const noexcept { return fd_; }
    [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }
    [[nodiscard]] int release() noexcept { return std::exchange(fd_, -1); }

private:
    int fd_ = -1;
};

template<std::size_t N>
concept PowerOfTwo = (N > 0) && ((N & (N - 1)) == 0);

enum class LogLevel { Debug, Info, Warn, Error };
inline void log(LogLevel level,
                std::string_view msg,
                std::source_location loc = std::source_location::current()) {
    using enum LogLevel;
    std::string_view prefix;
    switch (level) {
        case Debug:
            prefix = "[DBG] ";
            break;
        case Info:
            prefix = "[INF] ";
            break;
        case Warn:
            prefix = "[WRN] ";
            break;
        case Error:
            prefix = "[ERR] ";
            break;
    }
    std::println(stderr, "{}{}  ({}:{})", prefix, msg, loc.file_name(), loc.line());
}

} // namespace afxdp
