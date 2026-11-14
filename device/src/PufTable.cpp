#include "PufTable.hpp"

#include <pki/Bytes.hpp>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <stdexcept>
#include <string>

namespace pgdemo::device {

namespace {

class LockedFile
{
public:
    explicit LockedFile(const std::filesystem::path& path)
    {
        fd_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        if (fd_ < 0)
            throw std::runtime_error("PufTable: cannot open " + path.string() + ": " + std::strerror(errno));
        if (::flock(fd_, LOCK_EX) != 0)
        {
            const int err = errno;
            ::close(fd_);
            throw std::runtime_error("PufTable: cannot lock " + path.string() + ": " + std::strerror(err));
        }
        if (::fchmod(fd_, 0600) != 0)
        {
            const int err = errno;
            ::flock(fd_, LOCK_UN);
            ::close(fd_);
            throw std::runtime_error("PufTable: cannot set mode of " + path.string() + ": " + std::strerror(err));
        }
    }

    ~LockedFile()
    {
        ::flock(fd_, LOCK_UN);
        ::close(fd_);
    }

    LockedFile(const LockedFile&) = delete;
    LockedFile& operator=(const LockedFile&) = delete;

    [[nodiscard]] std::string read_all() const
    {
        std::string data;
        char buf[4096];
        if (::lseek(fd_, 0, SEEK_SET) < 0) throw std::runtime_error("PufTable: cannot seek");
        while (true)
        {
            const ssize_t n = ::read(fd_, buf, sizeof buf);
            if (n < 0)
            {
                if (errno == EINTR) continue;
                throw std::runtime_error(std::string("PufTable: cannot read: ") + std::strerror(errno));
            }
            if (n == 0) return data;
            data.append(buf, static_cast<std::size_t>(n));
        }
    }

    void append(const std::string& line) const
    {
        if (::lseek(fd_, 0, SEEK_END) < 0) throw std::runtime_error("PufTable: cannot seek");
        std::size_t done = 0;
        while (done < line.size())
        {
            const ssize_t n = ::write(fd_, line.data() + done, line.size() - done);
            if (n < 0)
            {
                if (errno == EINTR) continue;
                throw std::runtime_error(std::string("PufTable: cannot write: ") + std::strerror(errno));
            }
            done += static_cast<std::size_t>(n);
        }
        if (::fsync(fd_) != 0) throw std::runtime_error(std::string("PufTable: cannot fsync: ") + std::strerror(errno));
    }

private:
    int fd_{-1};
};

std::array<std::uint8_t, 32> parse_seed(std::string_view hex)
{
    const auto bytes = pki::from_hex(hex);
    if (bytes.size() != 32) throw std::runtime_error("PufTable: seed is not 32 bytes");
    std::array<std::uint8_t, 32> out{};
    std::copy(bytes.begin(), bytes.end(), out.begin());
    return out;
}

} // namespace

PufTable::PufTable(std::filesystem::path path) : path_(std::move(path)) {}

std::array<std::uint8_t, 32> PufTable::seed_for(std::int64_t device_id) const
{
    const LockedFile file(path_);
    const std::string data = file.read_all();
    std::size_t at = 0;
    int line_no = 0;
    while (at < data.size())
    {
        const auto end = data.find('\n', at);
        const auto line = std::string_view(data).substr(at, end == std::string::npos ? std::string::npos : end - at);
        at = end == std::string::npos ? data.size() : end + 1;
        ++line_no;
        if (line.empty() || line.starts_with('#')) continue;
        const auto space = line.find(' ');
        if (space == std::string_view::npos)
            throw std::runtime_error("PufTable: malformed line " + std::to_string(line_no));
        std::int64_t id = 0;
        const auto id_text = line.substr(0, space);
        const auto [ptr, ec] = std::from_chars(id_text.data(), id_text.data() + id_text.size(), id);
        if (ec != std::errc{} || ptr != id_text.data() + id_text.size())
            throw std::runtime_error("PufTable: malformed device id on line " + std::to_string(line_no));
        if (id != device_id) continue;
        return parse_seed(line.substr(space + 1));
    }

    const auto fresh = pki::random_bytes(32);
    file.append(std::to_string(device_id) + " " + pki::to_hex(fresh) + "\n");
    std::array<std::uint8_t, 32> out{};
    std::copy(fresh.begin(), fresh.end(), out.begin());
    return out;
}

} // namespace pgdemo::device
