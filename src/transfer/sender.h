#pragma once
#include <cstdint>
#include <string>

namespace svanipp {
    int run_sender(const std::string& ip, std::uint16_t port, const std::string& filePath, const std::string& relPath);
}