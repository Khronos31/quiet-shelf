// SPDX-License-Identifier: GPL-2.0-only
#include "torned/config.h"
#include "torned/daemon.h"

#include <dtv/userland/discovery_json.h>

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

namespace wire = dtv::userland::wire;
using dtv::userland::Error;
using dtv::userland::Result;
using torne::daemon::Backend;
using torne::daemon::DaemonConfig;

int failures = 0;

#define CHECK(value) do { if (!(value)) { ++failures; \
    std::fprintf(stderr, "failed: %s:%d: %s\n", __FILE__, __LINE__, #value); } } while (false)

template <std::size_t N>
wire::WString<N> text(const char* value)
{
    wire::WString<N> output;
    output.length = static_cast<std::uint16_t>(std::strlen(value));
    std::memcpy(output.bytes.data(), value, output.length);
    return output;
}

std::string read_file(std::FILE* file)
{
    std::fflush(file);
    std::rewind(file);
    std::string contents;
    char buffer[256];
    while (true) {
        const std::size_t read = std::fread(buffer, 1U, sizeof(buffer), file);
        contents.append(buffer, read);
        if (read < sizeof(buffer)) break;
    }
    return contents;
}

class FakeBackend final : public Backend {
public:
    int discover_calls = 0;
    int open_calls = 0;
    bool fail_discovery = false;

    wire::DiscoveryScan scan{};
    wire::DiscoveryScanIssue issue{};

    FakeBackend()
    {
        scan.complete = false;
        scan.product = text<32>("torne");
        scan.version = text<64>(TORNE_USERLAND_PRODUCT_VERSION);
        issue.code = text<64>("fake-backend");
        issue.detail = text<256>("test-only backend");
        scan.issue_count = 1U;
        scan.issues = &issue;
    }

    Result<wire::DiscoveryScan> discover(const DaemonConfig&) noexcept override
    {
        ++discover_calls;
        if (fail_discovery) return Result<wire::DiscoveryScan>::failure(Error::USB_IO);
        return Result<wire::DiscoveryScan>::success(scan);
    }

    Result<FamilyPtr> open_family(const DaemonConfig&) noexcept override
    {
        ++open_calls;
        return Result<FamilyPtr>::failure(Error::UNSUPPORTED);
    }
};

void test_parse()
{
    {
        const char* argv[] = {"torned", "--help"};
        const auto parsed = torne::daemon::parse_daemon_arguments(2, argv);
        CHECK(parsed.valid && parsed.config.help);
    }
    {
        const char* argv[] = {"torned", "--version"};
        const auto parsed = torne::daemon::parse_daemon_arguments(2, argv);
        CHECK(parsed.valid && parsed.config.version);
    }
    {
        const char* argv[] = {"torned", "--list-json"};
        const auto parsed = torne::daemon::parse_daemon_arguments(2, argv);
        CHECK(parsed.valid && parsed.config.list_json);
    }
    {
        const char* argv[] = {"torned", "--device", "usb:1-2.3", "--instance", "torne"};
        const auto parsed = torne::daemon::parse_daemon_arguments(5, argv);
        CHECK(parsed.valid);
        CHECK(parsed.config.has_device);
        CHECK(parsed.config.device.kind ==
              dtv::userland::identity::SelectorKind::usb_topology);
        CHECK(parsed.config.instance == "torne");
    }
    {
        const char* argv[] = {"torned", "--device", "serial:abc"};
        const auto parsed = torne::daemon::parse_daemon_arguments(3, argv);
        CHECK(!parsed.valid);
    }
    {
        const char* argv[] = {"torned", "--device", "usb:01-2"};
        const auto parsed = torne::daemon::parse_daemon_arguments(3, argv);
        CHECK(!parsed.valid);
    }
    {
        const char* argv[] = {"torned", "--fd", "7", "--instance", "torne"};
        const auto parsed = torne::daemon::parse_daemon_arguments(5, argv);
        CHECK(parsed.valid);
        CHECK(parsed.config.has_fd);
        CHECK(!parsed.config.has_device);
        CHECK(parsed.config.fd == 7);
        CHECK(parsed.config.instance == "torne");
    }
    {
        const char* argv[] = {"torned", "--fd", "7"};
        const auto parsed = torne::daemon::parse_daemon_arguments(3, argv);
        CHECK(!parsed.valid);
    }
    {
        const char* argv[] = {"torned", "--fd", "7", "--device", "usb:1-2",
                              "--instance", "torne"};
        const auto parsed = torne::daemon::parse_daemon_arguments(7, argv);
        CHECK(!parsed.valid);
    }
    {
        const char* argv[] = {"torned", "--fd", "7", "--fd", "8",
                              "--instance", "torne"};
        const auto parsed = torne::daemon::parse_daemon_arguments(7, argv);
        CHECK(!parsed.valid);
    }
    {
        const char* argv[] = {"torned", "--fd", "-1", "--instance", "torne"};
        const auto parsed = torne::daemon::parse_daemon_arguments(5, argv);
        CHECK(!parsed.valid);
    }
    {
        const char* argv[] = {"torned", "--fd", "x", "--instance", "torne"};
        const auto parsed = torne::daemon::parse_daemon_arguments(5, argv);
        CHECK(!parsed.valid);
    }
    {
        const char* argv[] = {"torned", "--list-json", "--fd", "7",
                              "--instance", "torne"};
        const auto parsed = torne::daemon::parse_daemon_arguments(7, argv);
        CHECK(!parsed.valid);
    }
    {
        const char* argv[] = {"torned", "--list-json", "--instance", "torne"};
        const auto parsed = torne::daemon::parse_daemon_arguments(4, argv);
        CHECK(!parsed.valid);
    }
    {
        const char* argv[] = {"torned", "--bogus"};
        const auto parsed = torne::daemon::parse_daemon_arguments(2, argv);
        CHECK(!parsed.valid);
    }
    {
        const char* argv[] = {"torned"};
        const auto parsed = torne::daemon::parse_daemon_arguments(1, argv);
        CHECK(!parsed.valid);
    }
}

void test_missing_backend()
{
    DaemonConfig config;
    config.list_json = true;
    std::FILE* output = std::tmpfile();
    std::FILE* error = std::tmpfile();
    CHECK(output != nullptr && error != nullptr);
    const int status = torne::daemon::run_daemon(config, nullptr, output, error);
    CHECK(status == 3);
    CHECK(read_file(output).empty());
    CHECK(read_file(error).find("no hardware backend") != std::string::npos);
    std::fclose(output);
    std::fclose(error);

    DaemonConfig start;
    start.has_device = true;
    output = std::tmpfile();
    error = std::tmpfile();
    CHECK(output != nullptr && error != nullptr);
    CHECK(torne::daemon::run_daemon(start, nullptr, output, error) == 3);
    CHECK(read_file(output).empty());
    std::fclose(output);
    std::fclose(error);
}

void test_fake_backend()
{
    FakeBackend backend;
    DaemonConfig config;
    config.list_json = true;
    std::FILE* output = std::tmpfile();
    std::FILE* error = std::tmpfile();
    CHECK(output != nullptr && error != nullptr);
    const int status = torne::daemon::run_daemon(config, &backend, output, error);
    CHECK(status == 0);
    CHECK(backend.discover_calls == 1);
    const std::string json = read_file(output);
    CHECK(json.find("\"torne\"") != std::string::npos);
    CHECK(json.find("fake-backend") != std::string::npos);
    std::fclose(output);
    std::fclose(error);

    DaemonConfig start;
    start.has_device = true;
    output = std::tmpfile();
    error = std::tmpfile();
    CHECK(output != nullptr && error != nullptr);
    CHECK(torne::daemon::run_daemon(start, &backend, output, error) == 3);
    CHECK(backend.open_calls == 1);
    CHECK(read_file(error).find("hardware backend open failed") != std::string::npos);
    std::fclose(output);
    std::fclose(error);

    DaemonConfig borrowed;
    borrowed.has_fd = true;
    borrowed.fd = 7;
    borrowed.instance = "torne";
    const int discoveries_before_fd = backend.discover_calls;
    output = std::tmpfile();
    error = std::tmpfile();
    CHECK(output != nullptr && error != nullptr);
    CHECK(torne::daemon::run_daemon(borrowed, &backend, output, error) == 3);
    CHECK(backend.open_calls == 2);
    CHECK(backend.discover_calls == discoveries_before_fd);
    CHECK(read_file(error).find("hardware backend open failed") != std::string::npos);
    std::fclose(output);
    std::fclose(error);
}

}  // namespace

int main()
{
    test_parse();
    test_missing_backend();
    test_fake_backend();
    if (failures != 0) {
        std::fprintf(stderr, "%d checks failed\n", failures);
        return 1;
    }
    return 0;
}
