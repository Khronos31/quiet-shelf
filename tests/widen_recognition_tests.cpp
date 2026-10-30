// SPDX-License-Identifier: GPL-2.0-only
#include "widen/bridge.h"
#include "widen/recognition.h"

#include <array>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

namespace {

namespace w = torne::widen;

int failures = 0;

#define CHECK(value) do { if (!(value)) { ++failures; \
    std::fprintf(stderr, "failed: %s:%d: %s\n", __FILE__, __LINE__, #value); } } while (false)

constexpr std::array<std::uint32_t, 5U> kPatchAddresses{
    0x0E9AC0U, 0x0E9AC4U, 0x0E9E0CU, 0x0E9E10U, 0x0E9D94U};

// GPL-2.0-only identification fixture from torne_drv
// fc662902a3e4ad56566b00cae4a0ead922e3dd02 driver/torne_fw_gen.h
// torne_addressing0_code[85]. It is only a read-only recognition reference and
// is not a product firmware image or a load target.
constexpr std::array<std::uint32_t, 85U> kLegacyImage{
    0xC40D0280U, 0xCFEDAAD0U, 0xE30F1781U, 0x8E3FD85CU, 0x8B448B40U, 0x8CFC0781U,
    0x9F80017AU, 0x3E40C44DU, 0x9BEC9AE6U, 0x9F0C1080U, 0xC40D0280U, 0xA800E20CU,
    0x8B401700U, 0xC40D0280U, 0x0701B580U, 0x92018B14U, 0x8B40C44DU, 0xF0151080U,
    0x9B8D0000U, 0xAAD4E410U, 0x9BAD0000U, 0xAAD4E50CU, 0x8B409B3DU, 0xFFFF82D0U,
    0x9F8D00F0U, 0x0000A6D0U, 0x8B048B40U, 0xC44D1080U, 0x9BEC9AE6U, 0x970CC40DU,
    0x02A0A800U, 0xE20E8B40U, 0x1700C40DU, 0x02A00701U, 0xB5809201U, 0x8B14C000U,
    0x12A08B40U, 0xC44DF015U, 0x10A09B8DU, 0x0000AAD4U, 0xE4109BADU, 0x0000AAD4U,
    0xE50C8B40U, 0x9B3DFFFFU, 0x82D09F8DU, 0x00F00000U, 0xA6D08B04U, 0x8B40C44DU,
    0x10A09BECU, 0x9E2E970CU, 0x0F058B40U, 0xB100CFFDU, 0x82D03FE0U, 0x9F804000U,
    0x00008B41U, 0x9B0D07FFU, 0x82D1B501U, 0x92103FD0U, 0x2FE0C01DU, 0xB04082D0U,
    0xE2062FD0U, 0x9F8D8000U, 0x000092D0U, 0x3FD02FE0U, 0xC01DB050U, 0x82D0E206U,
    0x2FD09F8DU, 0x20000000U, 0x92D03FD0U, 0x2FD02FE2U, 0x9F81017AU, 0x3E00C0FDU,
    0x82D2B422U, 0xA6211410U, 0x9B400000U, 0x3FC09F8DU, 0x017A3E40U, 0x06D02FE1U,
    0xC1FD82D0U, 0x82D1AA10U, 0xE2072FC0U, 0xA800E204U, 0x2FC0A5F0U, 0x3FC0E0EFU,
    0x9F909720U,
};

class FakeReader final : public w::Ep0Reader {
public:
    std::map<std::uint32_t, std::uint32_t> words;
    std::map<std::uint32_t, w::ReadResult> failures;
    std::vector<std::uint32_t> read_addresses;

    w::ReadResult read32(std::uint32_t address) noexcept override
    {
        read_addresses.push_back(address);
        const auto failure = failures.find(address);
        if (failure != failures.end()) return failure->second;
        const auto word = words.find(address);
        if (word == words.end())
            return w::ReadResult{false, 0U,
                                 torne::usb::TransferOutcome{
                                     torne::usb::TransferState::failed,
                                     torne::usb::TransferCause::invalid, 0U,
                                     false}};
        return w::ReadResult{true, word->second,
                             torne::usb::TransferOutcome{
                                 torne::usb::TransferState::complete,
                                 torne::usb::TransferCause::none, 4U, false}};
    }
};

w::ReadResult failure(torne::usb::TransferCause cause)
{
    return w::ReadResult{false, 0U,
                         torne::usb::TransferOutcome{
                             torne::usb::TransferState::failed, cause, 0U,
                             false}};
}

void load_five(FakeReader& reader, const std::array<std::uint32_t, 5U>& values)
{
    for (std::size_t index = 0U; index < values.size(); ++index)
        reader.words[kPatchAddresses[index]] = values[index];
}

void load_bytes(FakeReader& reader, std::uint32_t base,
                const std::vector<std::uint8_t>& bytes)
{
    for (std::size_t offset = 0U; offset < bytes.size(); offset += 4U) {
        const std::uint32_t value =
            (static_cast<std::uint32_t>(bytes[offset]) << 24U) |
            (static_cast<std::uint32_t>(bytes[offset + 1U]) << 16U) |
            (static_cast<std::uint32_t>(bytes[offset + 2U]) << 8U) |
            static_cast<std::uint32_t>(bytes[offset + 3U]);
        reader.words[base + static_cast<std::uint32_t>(offset)] = value;
    }
}

std::vector<std::uint8_t> legacy_bytes()
{
    std::vector<std::uint8_t> bytes(kLegacyImage.size() * 4U);
    for (std::size_t index = 0U; index < kLegacyImage.size(); ++index) {
        const std::uint32_t value = kLegacyImage[index];
        bytes[index * 4U] = static_cast<std::uint8_t>(value >> 24U);
        bytes[index * 4U + 1U] = static_cast<std::uint8_t>(value >> 16U);
        bytes[index * 4U + 2U] = static_cast<std::uint8_t>(value >> 8U);
        bytes[index * 4U + 3U] = static_cast<std::uint8_t>(value);
    }
    return bytes;
}

std::array<std::uint32_t, 5U> legacy_five()
{
    std::size_t count = 0U;
    const w::RegisteredImage* registry = w::legacy_registry(count);
    CHECK(count == 1U);
    std::array<std::uint32_t, 5U> five{};
    for (std::size_t index = 0U; index < five.size(); ++index)
        five[index] = registry[0].patches[index].value;
    return five;
}

void load_legacy(FakeReader& reader)
{
    const std::vector<std::uint8_t> bytes = legacy_bytes();
    load_bytes(reader, 0x0E1000U, bytes);
    const auto five = legacy_five();
    load_five(reader, five);
}

struct OwnFixture final {
    std::vector<std::uint8_t> bytes;
    w::RegisteredImage record;
    std::array<std::uint32_t, 5U> five;
};

OwnFixture make_own_at(std::uint32_t base, std::uint32_t entry_read,
                        std::uint32_t entry_write, std::uint32_t build_id,
                        std::uint8_t code_seed)
{
    OwnFixture fixture;
    fixture.bytes.assign(0x40U, 0x00U);
    const auto set_word = [&](std::size_t offset, std::uint32_t value) {
        fixture.bytes[offset] = static_cast<std::uint8_t>(value >> 24U);
        fixture.bytes[offset + 1U] = static_cast<std::uint8_t>(value >> 16U);
        fixture.bytes[offset + 2U] = static_cast<std::uint8_t>(value >> 8U);
        fixture.bytes[offset + 3U] = static_cast<std::uint8_t>(value);
    };
    set_word(0x00U, 0x5457444EU);
    set_word(0x04U, 0x00010000U);
    set_word(0x08U, build_id);
    set_word(0x0CU, 0x00000001U);
    set_word(0x10U, 0x00000040U);
    set_word(0x14U, entry_read);
    set_word(0x18U, entry_write);
    set_word(0x1CU, 0U);
    for (std::size_t offset = 0x20U; offset < fixture.bytes.size(); ++offset)
        fixture.bytes[offset] = static_cast<std::uint8_t>(offset + code_seed);

    fixture.record.format = w::ImageFormat::twdn;
    fixture.record.bank_base = base;
    fixture.record.image_bytes = 0x40U;
    fixture.record.entry_read = entry_read;
    fixture.record.entry_write = entry_write;
    fixture.record.expected_image = fixture.bytes.data();
    fixture.five = {w::make_jump_word(entry_read), 0x970C9FA0U,
                    w::make_jump_word(entry_write), 0x970C9FA0U,
                    0x9FA09F8DU};
    return fixture;
}

OwnFixture make_own()
{
    return make_own_at(0x0E1800U, 0x0E1820U, 0x0E1824U, 0x11223344U, 0U);
}

void load_own(FakeReader& reader, const OwnFixture& fixture)
{
    load_bytes(reader, fixture.record.bank_base, fixture.bytes);
    load_five(reader, fixture.five);
}

w::RecognitionResult recognize_legacy(FakeReader& reader)
{
    std::size_t count = 0U;
    const w::RegisteredImage* registry = w::legacy_registry(count);
    return w::recognize_read_only(reader, nullptr, registry, count);
}

bool has_read_at_or_above(const FakeReader& reader, std::uint32_t address)
{
    for (const std::uint32_t read : reader.read_addresses) {
        if (read >= address) return true;
    }
    return false;
}

void load_patches(FakeReader& reader, const w::RegisteredImage& image)
{
    for (const w::PatchWord& patch : image.patches)
        reader.words[patch.address] = patch.value;
}

void test_shifted_bank_records_are_unknown_without_high_reads()
{
    const OwnFixture shifted = make_own_at(
        0x1E1800U, 0x1E1820U, 0x1E1824U, 0x11223344U, 0U);
    FakeReader twdn_reader;
    load_own(twdn_reader, shifted);
    const auto twdn = w::recognize_read_only(
        twdn_reader, &shifted.record, nullptr, 0U);
    CHECK(twdn.classified);
    CHECK(twdn.state == w::WidenState::unknown);
    CHECK(!has_read_at_or_above(twdn_reader, 0x1E1800U));

    std::size_t count = 0U;
    const w::RegisteredImage* registry = w::legacy_registry(count);
    w::RegisteredImage shifted_legacy = registry[0];
    shifted_legacy.bank_base = 0x1E1000U;
    shifted_legacy.entry_read = 0x1E1000U;
    shifted_legacy.entry_write = 0x1E1076U;
    shifted_legacy.patches[0].value = w::make_jump_word(0x1E1000U);
    shifted_legacy.patches[2].value = w::make_jump_word(0x1E1076U);
    FakeReader legacy_reader;
    load_patches(legacy_reader, shifted_legacy);
    const auto legacy = w::recognize_read_only(
        legacy_reader, nullptr, &shifted_legacy, 1U);
    CHECK(legacy.classified);
    CHECK(legacy.state == w::WidenState::unknown);
    CHECK(!has_read_at_or_above(legacy_reader, 0x1E1000U));
}

void test_current_and_known_twdn_roles()
{
    const OwnFixture current = make_own();
    const OwnFixture known = make_own_at(
        0x0E1000U, 0x0E1020U, 0x0E1024U, 0x55667788U, 0x10U);

    FakeReader known_reader;
    load_own(known_reader, known);
    auto result = w::recognize_read_only(
        known_reader, &current.record, &known.record, 1U);
    CHECK(result.classified);
    CHECK(result.state == w::WidenState::foreign_known);

    FakeReader current_reader;
    load_own(current_reader, current);
    result = w::recognize_read_only(
        current_reader, &current.record, &known.record, 1U);
    CHECK(result.classified);
    CHECK(result.state == w::WidenState::ours_active);

    FakeReader duplicate_reader;
    load_own(duplicate_reader, current);
    result = w::recognize_read_only(
        duplicate_reader, &current.record, &current.record, 1U);
    CHECK(result.classified);
    CHECK(result.state == w::WidenState::unknown);
}

void test_legacy_current_is_not_ours()
{
    FakeReader reader;
    load_legacy(reader);
    std::size_t count = 0U;
    const w::RegisteredImage* registry = w::legacy_registry(count);
    const auto result = w::recognize_read_only(
        reader, registry, nullptr, 0U);
    CHECK(result.classified);
    CHECK(result.state == w::WidenState::unknown);
}

void test_bridge_helpers()
{
    const std::array<std::uint8_t, 4U> bytes{0x12U, 0x34U, 0x56U, 0x78U};
    CHECK(w::decode_be32(bytes) == 0x12345678U);
    CHECK(w::is_readable_24bit_address(0x0E1000U));
    CHECK(!w::is_readable_24bit_address(0x01000000U));
    CHECK(!w::is_readable_24bit_address(0x0E1002U));
}

void test_pristine()
{
    FakeReader reader;
    const std::array<std::uint32_t, 5U> pristine{
        0x9B800000U, 0xAA04E40FU, 0x9B800000U, 0xAA04E40DU, 0xB4279F8DU};
    load_five(reader, pristine);
    const auto result = recognize_legacy(reader);
    CHECK(result.classified);
    CHECK(result.state == w::WidenState::pristine);
    CHECK(!result.introduction_allowed);
}

void test_legacy_is_foreign_known()
{
    FakeReader reader;
    load_legacy(reader);
    const auto result = recognize_legacy(reader);
    CHECK(result.classified);
    CHECK(result.state == w::WidenState::foreign_known);
    CHECK(!result.introduction_allowed);
}

void test_legacy_image_byte_mutation_is_unknown()
{
    FakeReader reader;
    load_legacy(reader);
    reader.words[0x0E1000U] ^= 0x00000001U;
    const auto result = recognize_legacy(reader);
    CHECK(result.classified);
    CHECK(result.state == w::WidenState::unknown);
    CHECK(!result.introduction_allowed);
}

void test_legacy_entry_mutation_is_unknown()
{
    FakeReader reader;
    load_legacy(reader);
    std::size_t count = 0U;
    const w::RegisteredImage* registry = w::legacy_registry(count);
    w::RegisteredImage read_mutated = registry[0];
    read_mutated.entry_read = 0x0E1004U;
    auto result = w::recognize_read_only(reader, nullptr, &read_mutated, 1U);
    CHECK(result.classified);
    CHECK(result.state == w::WidenState::unknown);

    w::RegisteredImage write_mutated = registry[0];
    write_mutated.entry_write = 0x0E1078U;
    result = w::recognize_read_only(reader, nullptr, &write_mutated, 1U);
    CHECK(result.classified);
    CHECK(result.state == w::WidenState::unknown);
}

void test_legacy_five_word_mutation_is_unknown()
{
    for (const std::uint32_t address : kPatchAddresses) {
        FakeReader reader;
        load_legacy(reader);
        reader.words[address] ^= 0x00000001U;
        const auto result = recognize_legacy(reader);
        CHECK(result.classified);
        CHECK(result.state == w::WidenState::unknown);
    }
}

void test_unregistered_is_unknown()
{
    FakeReader reader;
    const std::array<std::uint32_t, 5U> words{
        0x11111111U, 0x22222222U, 0x33333333U, 0x44444444U, 0x55555555U};
    load_five(reader, words);
    const auto result = recognize_legacy(reader);
    CHECK(result.classified);
    CHECK(result.state == w::WidenState::unknown);
}

void test_read_failure_is_not_unknown()
{
    FakeReader five_failure;
    load_legacy(five_failure);
    five_failure.failures[kPatchAddresses[2]] =
        failure(torne::usb::TransferCause::timeout);
    const auto first = recognize_legacy(five_failure);
    CHECK(!first.classified);
    CHECK(first.stage == w::RecognitionStage::five_words);
    CHECK(first.failure.cause == torne::usb::TransferCause::timeout);

    FakeReader image_failure;
    load_legacy(image_failure);
    image_failure.failures[0x0E1004U] =
        failure(torne::usb::TransferCause::pipe);
    const auto second = recognize_legacy(image_failure);
    CHECK(!second.classified);
    CHECK(second.stage == w::RecognitionStage::image);
    CHECK(second.failure.cause == torne::usb::TransferCause::pipe);
}

void test_own_image_is_ours_active()
{
    FakeReader reader;
    const OwnFixture fixture = make_own();
    load_own(reader, fixture);
    const auto result = w::recognize_read_only(reader, &fixture.record, nullptr, 0U);
    CHECK(result.classified);
    CHECK(result.state == w::WidenState::ours_active);
    CHECK(!result.introduction_allowed);
}

void test_own_header_and_image_mutations_are_unknown()
{
    FakeReader header_reader;
    const OwnFixture header_fixture = make_own();
    load_own(header_reader, header_fixture);
    header_reader.words[header_fixture.record.bank_base] = 0x00000000U;
    const auto header = w::recognize_read_only(
        header_reader, &header_fixture.record, nullptr, 0U);
    CHECK(header.classified);
    CHECK(header.state == w::WidenState::unknown);

    FakeReader image_reader;
    const OwnFixture image_fixture = make_own();
    load_own(image_reader, image_fixture);
    image_reader.words[image_fixture.record.bank_base + 0x20U] ^= 0x00000001U;
    const auto image = w::recognize_read_only(
        image_reader, &image_fixture.record, nullptr, 0U);
    CHECK(image.classified);
    CHECK(image.state == w::WidenState::unknown);
}

void test_own_entry_mutation_is_unknown()
{
    FakeReader reader;
    const OwnFixture fixture = make_own();
    load_own(reader, fixture);
    w::RegisteredImage mutated = fixture.record;
    mutated.entry_read = 0x0E1824U;
    const auto result = w::recognize_read_only(reader, &mutated, nullptr, 0U);
    CHECK(result.classified);
    CHECK(result.state == w::WidenState::unknown);
}

}  // namespace

int main()
{
    test_bridge_helpers();
    test_pristine();
    test_legacy_is_foreign_known();
    test_legacy_image_byte_mutation_is_unknown();
    test_legacy_entry_mutation_is_unknown();
    test_legacy_five_word_mutation_is_unknown();
    test_unregistered_is_unknown();
    test_read_failure_is_not_unknown();
    test_own_image_is_ours_active();
    test_own_header_and_image_mutations_are_unknown();
    test_own_entry_mutation_is_unknown();
    test_shifted_bank_records_are_unknown_without_high_reads();
    test_current_and_known_twdn_roles();
    test_legacy_current_is_not_ours();
    if (failures != 0) {
        std::fprintf(stderr, "%d checks failed\n", failures);
        return 1;
    }
    return 0;
}
