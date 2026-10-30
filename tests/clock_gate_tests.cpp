// SPDX-License-Identifier: GPL-2.0-only
#include "product/clock_gate.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace {

using torne::widen::BlockReadResult;
using torne::widen::Ep0Io;
using torne::widen::ReadResult;
using torne::widen::WriteResult;

int failures = 0;

#define CHECK(value) do { if (!(value)) { ++failures; \
    std::fprintf(stderr, "failed: %s:%d: %s\n", __FILE__, __LINE__, #value); } } while (false)

struct RecordedWrite final {
    std::uint32_t address;
    std::uint32_t value;
    bool wide;
};

class FakeIo final : public Ep0Io {
public:
    std::array<RecordedWrite, 8U> writes{};
    std::size_t write_count = 0U;
    std::size_t fail_at = 8U;
    std::size_t reads = 0U;

    ReadResult read32(std::uint32_t) noexcept override
    {
        ++reads;
        return ReadResult{};
    }
    ReadResult read32_wide(std::uint32_t) noexcept override
    {
        ++reads;
        return ReadResult{};
    }
    WriteResult write32(std::uint32_t address, std::uint32_t value,
                        bool wide) noexcept override
    {
        if (write_count == fail_at) return WriteResult{};
        if (write_count < writes.size())
            writes[write_count] = RecordedWrite{address, value, wide};
        ++write_count;
        return WriteResult{true, {}};
    }
    WriteResult write32_trigger(std::uint32_t, std::uint32_t,
                                bool) noexcept override
    {
        return WriteResult{};
    }
    BlockReadResult read_block_wide(std::uint32_t,
                                    std::size_t) noexcept override
    {
        ++reads;
        return BlockReadResult{};
    }
    WriteResult write_block_wide(std::uint32_t, const std::uint8_t*,
                                 std::size_t) noexcept override
    {
        return WriteResult{};
    }
};

void test_clock_gate_sequence()
{
    FakeIo io;
    CHECK(torne::product::clock_gate_init(io));
    CHECK(io.write_count == 6U);
    CHECK(io.reads == 0U);
    const std::array<RecordedWrite, 6U> expected{{
        {0x0F1010CU, 0x82010000U, false},
        {0x0F10114U, 0x80080000U, false},
        {0x0F10118U, 0xFFFF0000U, false},
        {0x0F1011CU, 0x00060000U, false},
        {0x0F10128U, 0x60000000U, false},
        {0x0F1012CU, 0x40070000U, false},
    }};
    for (std::size_t index = 0U; index < expected.size(); ++index) {
        CHECK(io.writes[index].address == expected[index].address);
        CHECK(io.writes[index].value == expected[index].value);
        CHECK(io.writes[index].wide == expected[index].wide);
    }
}

void test_failure_stops_sequence()
{
    FakeIo io;
    io.fail_at = 2U;
    CHECK(!torne::product::clock_gate_init(io));
    CHECK(io.write_count == 2U);
    CHECK(io.reads == 0U);
}

}  // namespace

int main()
{
    test_clock_gate_sequence();
    test_failure_stops_sequence();
    return failures == 0 ? 0 : 1;
}
