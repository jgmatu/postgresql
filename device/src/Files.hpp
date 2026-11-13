#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <utility>

namespace pgdemo::device {

/// Read a whole file. Throws if it cannot be opened.
inline std::string read_file(const std::filesystem::path& path)
{
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot read " + path.string());
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

/// Replace `path` by writing a temporary file beside it and renaming.
inline void write_file(const std::filesystem::path& path, const std::string& data)
{
    const std::filesystem::path tmp = path.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) throw std::runtime_error("cannot write " + tmp.string());
        out << data;
    }
    std::filesystem::rename(tmp, path);
}

/// Split `HOST:PORT`. The host may be a name; the port is the last colon.
inline std::pair<std::string, std::uint16_t> split_endpoint(const std::string& endpoint)
{
    const auto colon = endpoint.rfind(':');
    if (colon == std::string::npos || colon == 0) throw std::runtime_error("expected HOST:PORT, got " + endpoint);
    return {endpoint.substr(0, colon), static_cast<std::uint16_t>(std::stoi(endpoint.substr(colon + 1)))};
}

} // namespace pgdemo::device
