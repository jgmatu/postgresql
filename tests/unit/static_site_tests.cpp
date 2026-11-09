// StaticSite: how request paths map to the files of the control panel, and
// the path helpers that decide what is API (token required) and what is panel.

#include <filesystem>
#include <fstream>
#include <random>
#include <string>

#include <gtest/gtest.h>

#include "http/Paths.hpp"
#include "http/StaticSite.hpp"

namespace fs = std::filesystem;
using pgdemo::server::http::is_api_path;
using pgdemo::server::http::percent_decode;
using pgdemo::server::http::StaticSite;

namespace {

class StaticSiteTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        root_ = fs::temp_directory_path() / ("pgdemo-webroot-" + std::to_string(std::random_device{}()));
        write("index.html", "<html>login</html>");
        write("404.html", "<html>missing</html>");
        write("devices/index.html", "<html>devices</html>");
        write("admin/devices/index.html", "<html>admin</html>");
        write("devices/__next._tree.txt", "rsc");
        write("_next/static/chunks/app-1a2b.js", "console.log(1)");
        write("_next/static/css/app.css", "body{}");
        write("VERSION", "commit=abc123\nbuilt_at=2026-11-09T21:00:00Z\n");
        fs::create_directories(root_ / "empty");
    }

    void TearDown() override { fs::remove_all(root_); }

    void write(const std::string& rel, const std::string& body)
    {
        const auto p = root_ / rel;
        fs::create_directories(p.parent_path());
        std::ofstream(p, std::ios::binary) << body;
    }

    fs::path root_;
};

TEST_F(StaticSiteTest, LoadsEveryFile)
{
    const auto site = StaticSite::load(root_);
    EXPECT_EQ(site.size(), 8u);
    EXPECT_TRUE(site.has_index());
    EXPECT_GT(site.bytes(), 0u);
}

TEST_F(StaticSiteTest, DirectoriesResolveToIndexHtml)
{
    const auto site = StaticSite::load(root_);
    ASSERT_NE(site.lookup("/"), nullptr);
    EXPECT_EQ(site.lookup("/")->body, "<html>login</html>");
    ASSERT_NE(site.lookup("/devices/"), nullptr);
    EXPECT_EQ(site.lookup("/devices/")->body, "<html>devices</html>");
    ASSERT_NE(site.lookup("/devices"), nullptr);
    EXPECT_EQ(site.lookup("/devices")->body, "<html>devices</html>");
    ASSERT_NE(site.lookup("/admin/devices/"), nullptr);
    EXPECT_EQ(site.lookup("/admin/devices/")->mime, "text/html; charset=utf-8");
}

TEST_F(StaticSiteTest, HashedBundlesAreImmutable)
{
    const auto site = StaticSite::load(root_);
    const auto* js = site.lookup("/_next/static/chunks/app-1a2b.js");
    ASSERT_NE(js, nullptr);
    EXPECT_EQ(js->mime, "text/javascript; charset=utf-8");
    EXPECT_TRUE(js->immutable);
    EXPECT_FALSE(site.lookup("/devices/")->immutable);
    EXPECT_FALSE(site.lookup("/devices/__next._tree.txt")->immutable);
}

TEST_F(StaticSiteTest, UnknownAndEscapingPathsMiss)
{
    const auto site = StaticSite::load(root_);
    EXPECT_EQ(site.lookup("/nope/"), nullptr);
    EXPECT_EQ(site.lookup("/devices/../index.html"), nullptr);
    EXPECT_EQ(site.lookup("/%2e%2e/etc/passwd"), nullptr);
    EXPECT_EQ(site.lookup("/.."), nullptr);
    EXPECT_EQ(site.lookup("relative"), nullptr);
    EXPECT_EQ(site.lookup(""), nullptr);
    EXPECT_EQ(site.lookup("/index.html%00.js"), nullptr);
    EXPECT_EQ(site.lookup("/empty/"), nullptr);
}

TEST_F(StaticSiteTest, EscapesAreDecoded)
{
    const auto site = StaticSite::load(root_);
    ASSERT_NE(site.lookup("/%64evices/"), nullptr);
    EXPECT_EQ(site.lookup("/%64evices/")->body, "<html>devices</html>");
}

TEST_F(StaticSiteTest, NotFoundPageAndVersion)
{
    const auto site = StaticSite::load(root_);
    ASSERT_NE(site.not_found_page(), nullptr);
    EXPECT_EQ(site.not_found_page()->body, "<html>missing</html>");
    EXPECT_EQ(site.version(), "commit=abc123 built_at=2026-11-09T21:00:00Z");
    EXPECT_EQ(site.lookup("/VERSION")->mime, "text/plain; charset=utf-8");
}

TEST_F(StaticSiteTest, DirectoryWithoutIndexHasNoPanel)
{
    const auto site = StaticSite::load(root_ / "empty");
    EXPECT_EQ(site.size(), 0u);
    EXPECT_FALSE(site.has_index());
    EXPECT_EQ(site.not_found_page(), nullptr);
    EXPECT_EQ(site.version(), "");
}

TEST_F(StaticSiteTest, MissingDirectoryThrows)
{
    EXPECT_THROW(StaticSite::load(root_ / "absent"), std::runtime_error);
    EXPECT_THROW(StaticSite::load(root_ / "index.html"), std::runtime_error);
}

TEST(StaticSiteMime, ByExtension)
{
    EXPECT_EQ(StaticSite::mime_for("/a/index.html"), "text/html; charset=utf-8");
    EXPECT_EQ(StaticSite::mime_for("/x.css"), "text/css; charset=utf-8");
    EXPECT_EQ(StaticSite::mime_for("/x.json"), "application/json");
    EXPECT_EQ(StaticSite::mime_for("/__next._full.txt"), "text/plain; charset=utf-8");
    EXPECT_EQ(StaticSite::mime_for("/logo.svg"), "image/svg+xml");
    EXPECT_EQ(StaticSite::mime_for("/favicon.ico"), "image/x-icon");
    EXPECT_EQ(StaticSite::mime_for("/a.png"), "image/png");
    EXPECT_EQ(StaticSite::mime_for("/f.woff2"), "font/woff2");
    EXPECT_EQ(StaticSite::mime_for("/VERSION"), "text/plain; charset=utf-8");
    EXPECT_EQ(StaticSite::mime_for("/a.d/blob"), "text/plain; charset=utf-8");
    EXPECT_EQ(StaticSite::mime_for("/a.wasm"), "application/octet-stream");
}

TEST(PercentDecode, DecodesEscapesOnly)
{
    EXPECT_EQ(percent_decode("device%2Cconfig"), "device,config");
    EXPECT_EQ(percent_decode("%2c%2C"), ",,");
    EXPECT_EQ(percent_decode("2026-11-09T10:00:00+01:00"), "2026-11-09T10:00:00+01:00");
    EXPECT_EQ(percent_decode("2026-11-09T10:00:00%2B01:00"), "2026-11-09T10:00:00+01:00");
    EXPECT_EQ(percent_decode("100%"), "100%");
    EXPECT_EQ(percent_decode("%zz%4"), "%zz%4");
    EXPECT_EQ(percent_decode(""), "");
}

TEST(IsApiPath, FoldsCaseEscapesAndSlashes)
{
    EXPECT_TRUE(is_api_path("/api"));
    EXPECT_TRUE(is_api_path("/api/"));
    EXPECT_TRUE(is_api_path("/api/devices"));
    EXPECT_TRUE(is_api_path("/API/devices"));
    EXPECT_TRUE(is_api_path("/%61pi/devices"));
    EXPECT_TRUE(is_api_path("//api/devices"));
    EXPECT_FALSE(is_api_path("/"));
    EXPECT_FALSE(is_api_path("/apis"));
    EXPECT_FALSE(is_api_path("/devices/"));
    EXPECT_FALSE(is_api_path("/admin/api/"));
    EXPECT_FALSE(is_api_path("/_next/static/api.js"));
    EXPECT_FALSE(is_api_path(""));
}

} // namespace
