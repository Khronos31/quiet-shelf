// SPDX-License-Identifier: GPL-2.0-only
#include <torne/product_contract.h>
#include <torne/torne_device.h>
#include <dtv/userland/identity.h>
#include <dtv/userland/error.h>

extern "C" {
#include "torne_core.h"
}

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {
int failures = 0;
#define CHECK(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "%s:%d failed: %s\n", __FILE__, __LINE__, #condition); \
    ++failures; } } while (false)

void test_tuning()
{
    const auto t27 = torne::resolve_terrestrial_hz("T27");
    CHECK(t27.has_value());
    if (t27) CHECK(t27.value() == 557142857U);
    CHECK(!torne::resolve_terrestrial_hz("T12"));
    CHECK(!torne::resolve_terrestrial_hz("BS01"));
    CHECK(!torne::resolve_terrestrial_hz("27MHz"));
}

void test_discovery()
{
    const auto empty = torne::discovery_json({}, true);
    CHECK(empty.has_value());
    if (empty) CHECK(empty.value().find("\"enclosures\":[]") != std::string::npos);
    const auto one = torne::discovery_json({{1U, 22U, "1-2.3"}}, true);
    CHECK(one.has_value());
    if (one) {
        const auto& json = one.value();
        CHECK(json.find("\"product\":\"torne\"") != std::string::npos);
        CHECK(json.find("\"usb\":\"054c:04b2\"") != std::string::npos);
        CHECK(json.find("\"port\":\"1-2.3\"") != std::string::npos);
        CHECK(json.find("\"systems\":[\"ISDB-T\"]") != std::string::npos);
        CHECK(json.find("\"serial_unique\":null") != std::string::npos);
        CHECK(json.find("\"lnb_15v_supported\":false") != std::string::npos);
        CHECK(json.find("\"reader\":0") != std::string::npos);
    }
    CHECK(!torne::discovery_json({{1U, 22U, "01-2"}}, true));
    CHECK(!torne::discovery_json({{1U, 22U, "1-2"},{1U, 24U, "1-2"}}, true));
    const auto missing = torne::discovery_json({{1U, 22U, ""}}, false);
    CHECK(missing.has_value());
    if (missing) CHECK(missing.value().find("\"status\":\"incomplete\"") != std::string::npos);
}

void test_core_selector()
{
    using namespace dtv::userland::identity;
    const auto selected = parse_device_selector("usb:1-2.3");
    CHECK(selected.has_value());
    if (selected) CHECK(selected.value().topology.count == 1U);
    CHECK(!parse_device_selector("usb:01-2"));
    // v0.1.0 has no "torne" Product enum; do not fake an identity slug.
    CHECK(!parse_product("torne-userland"));
}

void test_errors()
{
    using dtv::userland::Error;
    CHECK(torne::map_driver_error(0) == Error::OK);
    CHECK(torne::map_driver_error(-ETIMEDOUT) == Error::TIMEOUT);
    CHECK(torne::map_driver_error(-ENODEV) == Error::DISCONNECTED);
    CHECK(torne::map_driver_error(-EBUSY) == Error::BUSY);
    CHECK(torne::map_driver_error(-EIO) == Error::USB_IO);
    struct torne_ops mock{};
    torne::Device inactive(mock, nullptr);
    CHECK(!inactive.tune(557142857U));
    CHECK(inactive.tune(557142857U).error() == Error::NOT_READY);
    CHECK(!inactive.start_stream());
}

void test_source_framer()
{
    std::array<std::uint8_t, 384> raw{};
    raw[4] = 0x47U;
    raw[196] = 0x47U;
    raw[5] = 0x40U;
    raw[197] = 0x40U;

    struct torne_framer fr{};
    torne_framer_reset(&fr);
    std::array<std::uint8_t, 384> plain{};
    CHECK(torne_framer_feed(&fr, raw.data(), 191U, plain.data()) == 0U);
    const auto total = torne_framer_feed(&fr, raw.data() + 191U, 193U, plain.data());
    CHECK(total == 376U);
    CHECK(plain[0] == 0x47U);
    CHECK(plain[188] == 0x47U);
    CHECK(plain[1] == 0x40U);
    CHECK(plain[189] == 0x40U);
    // All-zero extra four bytes are intentional: T27 hardware observations.
    CHECK(raw[0] == 0U);
    CHECK(raw[192] == 0U);
}

} // namespace

int main()
{
    test_tuning();
    test_discovery();
    test_core_selector();
    test_source_framer();
    test_errors();
    if (failures != 0) std::fprintf(stderr, "%d test failures\n", failures);
    return failures == 0 ? 0 : 1;
}
