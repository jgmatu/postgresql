// devices.puf: one seed per device id, stable across opens.

#include <gtest/gtest.h>

#include "PufTable.hpp"

#include <pki/DeviceKey.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

namespace {

std::string read_file(const fs::path& path)
{
    std::ifstream in(path);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

TEST(PufTable, KeepsTheSeedOfADevice)
{
    const auto dir = fs::temp_directory_path() / ("pgdemo-puf-" + std::to_string(::getpid()));
    fs::create_directories(dir);
    const auto path = dir / "devices.puf";
    pgdemo::device::PufTable table(path);
    const auto first = table.seed_for(7);
    EXPECT_EQ(table.seed_for(7), first);
    EXPECT_NE(table.seed_for(8), first);
    EXPECT_EQ(fs::status(path).permissions() & fs::perms::all, fs::perms::owner_read | fs::perms::owner_write);
    const auto again = (dir / "puf-again.key").string();
    const auto once = (dir / "puf-once.key").string();
    pgdemo::pki::DeviceKey::from_seed(first).save(once);
    pgdemo::pki::DeviceKey::from_seed(pgdemo::device::PufTable(path).seed_for(7)).save(again);
    EXPECT_EQ(read_file(once), read_file(again));
    fs::remove_all(dir);
}

} // namespace
