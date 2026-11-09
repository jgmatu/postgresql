#pragma once

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>

#include "Paths.hpp"

namespace pgdemo::server::http {

/// One file of the control panel, held in memory.
struct StaticFile
{
    std::string body;      ///< file contents
    std::string_view mime; ///< `Content-Type`
    bool immutable;        ///< content-addressed (`/_next/static/`): cacheable forever
};

/**
 * @brief The control panel: the static export of `frontend/` (the webroot),
 * read into memory at startup.
 *
 * The export is a couple of megabytes and never changes while the server
 * runs, so it is loaded once; a request is a hash lookup and cannot reach
 * anything outside the directory, because only files found when loading are
 * keys. No Oat++ types here, so the lookup rules are unit-tested directly.
 */
class StaticSite
{
public:
    /// The largest webroot accepted; the export is about 2 MiB.
    static constexpr std::size_t kMaxBytes = 64u << 20;

    /**
     * @brief Read every regular file under `root`.
     * @throws std::runtime_error if `root` is not a directory, a file cannot
     *         be read, or the total exceeds `kMaxBytes`
     */
    static StaticSite load(const std::filesystem::path& root)
    {
        namespace fs = std::filesystem;
        if (!fs::is_directory(root)) throw std::runtime_error("webroot " + root.string() + " is not a directory");
        StaticSite site;
        for (const auto& entry : fs::recursive_directory_iterator(root))
        {
            if (!entry.is_regular_file()) continue;
            std::ifstream in(entry.path(), std::ios::binary);
            if (!in) throw std::runtime_error("cannot read " + entry.path().string());
            std::string body{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
            site.bytes_ += body.size();
            if (site.bytes_ > kMaxBytes) throw std::runtime_error("webroot " + root.string() + " exceeds 64 MiB");
            const std::string key = "/" + entry.path().lexically_relative(root).generic_string();
            site.files_.emplace(key, StaticFile{std::move(body), mime_for(key), key.starts_with("/_next/static/")});
        }
        return site;
    }

    /**
     * @brief File for a request path (without query string).
     *
     * `/` and `/x/` are `index.html` of that directory, as the static export
     * lays pages out (`trailingSlash`); `/x` falls back to `/x/index.html`.
     * Escapes are decoded; a `..` segment never matches.
     */
    [[nodiscard]] const StaticFile* lookup(std::string_view raw_path) const
    {
        std::string path = percent_decode(raw_path);
        if (path.empty() || path.front() != '/' || path.find('\0') != std::string::npos) return nullptr;
        if (path == "/.." || path.find("/../") != std::string::npos || path.ends_with("/..")) return nullptr;
        if (path.ends_with('/')) path += "index.html";
        if (const auto it = files_.find(path); it != files_.end()) return &it->second;
        if (const auto it = files_.find(path + "/index.html"); it != files_.end()) return &it->second;
        return nullptr;
    }

    /// `404.html` of the export, served with status 404 for unknown paths.
    [[nodiscard]] const StaticFile* not_found_page() const { return find("/404.html"); }

    /// Whether the export has its entry page; without it there is no panel.
    [[nodiscard]] bool has_index() const { return find("/index.html") != nullptr; }

    /// Number of files loaded.
    [[nodiscard]] std::size_t size() const noexcept { return files_.size(); }

    /// Total bytes loaded.
    [[nodiscard]] std::size_t bytes() const noexcept { return bytes_; }

    /// `VERSION` (commit and build date, written by the build) on one line, or empty.
    [[nodiscard]] std::string version() const
    {
        const auto* f = find("/VERSION");
        if (!f) return {};
        std::string v;
        for (const char c : f->body)
        {
            if (c == '\n') v += v.empty() ? "" : " ";
            else if (c != '\r') v += c;
        }
        while (!v.empty() && v.back() == ' ') v.pop_back();
        return v;
    }

    /// `Content-Type` for a path, by extension; text types in UTF-8.
    static std::string_view mime_for(std::string_view path)
    {
        const auto slash = path.rfind('/');
        const auto name = path.substr(slash == std::string_view::npos ? 0 : slash + 1);
        const auto dot = name.rfind('.');
        if (dot == std::string_view::npos) return "text/plain; charset=utf-8";
        const auto ext = name.substr(dot + 1);
        if (ext == "html") return "text/html; charset=utf-8";
        if (ext == "js") return "text/javascript; charset=utf-8";
        if (ext == "css") return "text/css; charset=utf-8";
        if (ext == "json") return "application/json";
        // React Server Component payloads of the export; Next expects text/plain.
        if (ext == "txt") return "text/plain; charset=utf-8";
        if (ext == "svg") return "image/svg+xml";
        if (ext == "ico") return "image/x-icon";
        if (ext == "png") return "image/png";
        if (ext == "woff2") return "font/woff2";
        return "application/octet-stream";
    }

private:
    [[nodiscard]] const StaticFile* find(const std::string& key) const
    {
        const auto it = files_.find(key);
        return it == files_.end() ? nullptr : &it->second;
    }

    std::unordered_map<std::string, StaticFile> files_;
    std::size_t bytes_ = 0;
};

} // namespace pgdemo::server::http
