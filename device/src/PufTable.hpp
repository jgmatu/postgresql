#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>

namespace pgdemo::device {

/**
 * @brief `devices.puf`: one 32-byte seed per `device_id`, mode 0600.
 *
 * A missing id is generated and appended. The same id on a later start returns
 * the stored seed. Writers take an exclusive lock, so several workers can share
 * the file. The seed never leaves this file; the private key is derived from it.
 */
class PufTable
{
public:
    /// @param path file created on first use, mode 0600
    explicit PufTable(std::filesystem::path path);

    /**
     * @brief Seed for `device_id`, created and stored the first time.
     * @throws std::runtime_error on I/O failure or a malformed file
     */
    [[nodiscard]] std::array<std::uint8_t, 32> seed_for(std::int64_t device_id) const;

private:
    std::filesystem::path path_;
};

} // namespace pgdemo::device
