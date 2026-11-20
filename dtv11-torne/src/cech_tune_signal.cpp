/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cech_tune_signal.hpp"

#include <cstdint>
#include <span>

namespace torne::cech_zd1j {
namespace {

struct PllBand {
    std::uint32_t upper_khz;
    std::uint8_t vco_band;
    std::uint8_t hf_code;
    std::uint8_t hf_divider;
    std::uint8_t prescaler;
};

struct TunerBand {
    std::uint32_t upper_khz;
    std::uint16_t switch_trim;
    std::uint16_t lna_tune;
    std::uint16_t lna_bias;
    std::uint16_t v_to_i;
    std::uint16_t mixer;
    std::uint16_t load;
    std::uint16_t enable;
};

struct RegisterPair16 {
    std::uint16_t reg;
    std::uint16_t value;
};

struct AddressValue32 {
    std::uint32_t address;
    std::uint32_t value;
};

struct BandGain {
    std::uint32_t upper_mhz;
    std::uint16_t gain;
};

constexpr PllBand kStandardPll[]{
    {56000, 0, 9, 48, 6}, {70000, 1, 9, 48, 6}, {87000, 0, 8, 32, 4},
    {105000, 1, 8, 32, 4}, {115000, 0, 7, 24, 6}, {140000, 1, 7, 24, 6},
    {170000, 0, 6, 16, 4}, {200000, 1, 6, 16, 4}, {230000, 0, 5, 12, 6},
    {280000, 1, 5, 12, 6}, {340000, 0, 4, 8, 4}, {380000, 1, 4, 8, 4},
    {450000, 0, 3, 6, 6}, {580000, 1, 3, 6, 6}, {700000, 0, 2, 4, 4},
    {860000, 1, 2, 4, 4}};

constexpr PllBand kP1gPll[]{
    {57000, 0, 11, 48, 6}, {70000, 1, 11, 48, 6}, {86000, 0, 10, 32, 4},
    {105000, 1, 10, 32, 4}, {115000, 0, 9, 24, 6}, {140000, 1, 9, 24, 6},
    {170000, 0, 8, 16, 4}, {200000, 1, 8, 16, 4}, {230000, 0, 7, 12, 6},
    {280000, 1, 7, 12, 6}, {340000, 0, 6, 8, 4}, {380000, 1, 6, 8, 4},
    {455000, 0, 5, 6, 6}, {580000, 1, 5, 6, 6}, {680000, 0, 4, 4, 4},
    {860000, 1, 4, 4, 4}};

constexpr TunerBand kStandardTuner[]{
    {170000, 4, 1, 15, 0x280, 0x2912, 0xB94E, 0x88E9},
    {184000, 1, 1, 15, 0x300, 0x4D12, 0xB94E, 0x44E9},
    {227000, 1, 3, 15, 0x300, 0x4D12, 0xB94E, 0x44E9},
    {380000, 1, 7, 15, 0x300, 0x4D12, 0xB94E, 0x44E9},
    {520000, 2, 0, 15, 0x300, 0x1D12, 0xB9CE, 0x22E9},
    {550000, 2, 2, 15, 0x300, 0x1D12, 0xB9CE, 0x22E9},
    {650000, 2, 3, 15, 0x300, 0x1D12, 0xB9CE, 0x22E9},
    {750000, 2, 5, 15, 0x300, 0x1D12, 0xB9CE, 0x22E9},
    {850000, 2, 6, 15, 0x300, 0x1D12, 0xB9CE, 0x22E9},
    {900000, 2, 7, 15, 0x300, 0x1D12, 0xB9CE, 0x22E9}};

constexpr TunerBand kP1gTuner[]{
    {170000, 4, 1, 33295, 0x300, 0x2D22, 0x82CB, 0x88E9},
    {184000, 1, 1, 15, 0x300, 0x4D12, 0xB94E, 0x44E9},
    {227000, 1, 3, 15, 0x300, 0x4D12, 0xB94E, 0x44E9},
    {380000, 1, 7, 15, 0x300, 0x4D12, 0xB94E, 0x44E9},
    {510000, 2, 0, 15, 0x300, 0x1D12, 0xB9CE, 0x22E9},
    {540000, 2, 1, 15, 0x300, 0x1D12, 0xB9CE, 0x22E9},
    {600000, 2, 3, 15, 0x300, 0x1D12, 0xB9CE, 0x22E9},
    {630000, 2, 4, 15, 0x300, 0x1D12, 0xB9CE, 0x22E9},
    {680000, 2, 5, 15, 0x300, 0x1D12, 0xB9CE, 0x22E9},
    {720000, 2, 6, 15, 0x300, 0x1D12, 0xB9CE, 0x22E9},
    {900000, 2, 7, 15, 0x300, 0x1D12, 0xB9CE, 0x22E9}};

constexpr TunerBand kStandardLowBand[]{
    {184000, 4, 1, 15, 0x280, 0x2912, 0xB94E, 0x88E9},
    {227000, 4, 3, 15, 0x280, 0x2912, 0xB94E, 0x88E9},
    {380000, 4, 7, 15, 0x280, 0x2912, 0xB94E, 0x88E9},
    {520000, 2, 0, 15, 0x300, 0x1D12, 0xB9CE, 0x22E9},
    {550000, 2, 2, 15, 0x300, 0x1D12, 0xB9CE, 0x22E9},
    {650000, 2, 3, 15, 0x300, 0x1D12, 0xB9CE, 0x22E9},
    {750000, 2, 5, 15, 0x300, 0x1D12, 0xB9CE, 0x22E9},
    {850000, 2, 6, 15, 0x300, 0x1D12, 0xB9CE, 0x22E9},
    {900000, 2, 7, 15, 0x300, 0x1D12, 0xB9CE, 0x22E9}};

constexpr TunerBand kP1gLowBand[]{
    {184000, 4, 3, 16775, 0x2C0, 0x2D22, 0x81CB, 0x88E9},
    {227000, 4, 3, 16775, 0x2C0, 0x2D22, 0x81CB, 0x88E9},
    {380000, 4, 3, 16775, 0x2C0, 0x2D22, 0x81CB, 0x88E9},
    {520000, 2, 0, 15, 0x300, 0x1D12, 0xB9CE, 0x22E9},
    {550000, 2, 2, 15, 0x300, 0x1D12, 0xB9CE, 0x22E9},
    {650000, 2, 3, 15, 0x300, 0x1D12, 0xB9CE, 0x22E9},
    {750000, 2, 5, 15, 0x300, 0x1D12, 0xB9CE, 0x22E9},
    {850000, 2, 6, 15, 0x300, 0x1D12, 0xB9CE, 0x22E9},
    {900000, 2, 7, 15, 0x300, 0x1D12, 0xB9CE, 0x22E9}};

template <typename T, std::size_t N>
const T *find_band(const T (&bands)[N], std::uint32_t frequency_khz)
{
    for (const auto &band : bands)
        if (frequency_khz <= band.upper_khz) return &band;
    return nullptr;
}

bool known_package(std::uint8_t package)
{
    return package == 1 || package == 4 || package == 6 || package == 7;
}

bool p1g_revision(std::uint8_t revision)
{
    return revision == 7 || revision == 15;
}

bool known_revision(std::uint8_t revision)
{
    return revision == 1 || revision == 3 || p1g_revision(revision);
}

std::uint16_t wbd_gain(std::uint64_t frequency_hz)
{
    constexpr BandGain bands[]{
        {120, 4}, {170, 4}, {380, 6}, {860, 6}, {65535, 6}};
    const auto frequency_mhz = static_cast<std::uint32_t>(frequency_hz / 1000000);
    for (const auto &band : bands)
        if (frequency_mhz <= band.upper_mhz) return band.gain;
    return 4;
}

void append(TunePlan *plan, std::uint8_t address, std::uint16_t value)
{
    plan->writes[plan->write_count++] = {address, value};
}

void append(DemodInitPlan *plan, InitStepKind kind, std::uint16_t reg,
            std::uint32_t value = 0, std::uint32_t mask = 0)
{
    plan->steps[plan->step_count++] = {InitTarget::demodulator, kind, reg, value, mask};
}

void append(DemodInitPlan *plan, InitTarget target, InitStepKind kind,
            std::uint32_t reg, std::uint32_t value = 0, std::uint32_t mask = 0)
{
    plan->steps[plan->step_count++] = {target, kind, reg, value, mask};
}

void append(TunerInitPlan *plan, InitTarget target, InitStepKind kind,
            std::uint32_t reg, std::uint32_t value = 0, std::uint32_t mask = 0)
{
    plan->steps[plan->step_count++] = {target, kind, reg, value, mask};
}

void append(FrontendPowerPlan *plan, InitStepKind kind, std::uint32_t reg,
            std::uint32_t value = 0, std::uint32_t mask = 0)
{
    plan->steps[plan->step_count++] = {InitTarget::device_memory, kind, reg, value, mask};
}

void append(TunerCalibrationPlan *plan, InitStepKind kind, std::uint32_t reg,
            std::uint32_t value = 0, std::uint32_t mask = 0)
{
    plan->steps[plan->step_count++] = {InitTarget::tuner, kind, reg, value, mask};
}

bool append(StreamPlan *plan, StreamStepKind kind, std::uint32_t reg,
            std::uint32_t value = 0, std::uint32_t mask = 0,
            std::uint32_t timeout_ms = 0, std::uint32_t interval_ms = 0)
{
    if (plan->step_count == sizeof(plan->steps) / sizeof(plan->steps[0])) return false;
    plan->steps[plan->step_count++] = {
        kind, reg, value, mask, timeout_ms, interval_ms};
    return true;
}

void append_words(DemodInitPlan *plan, std::uint16_t first,
                  std::span<const std::uint16_t> words)
{
    for (std::size_t index = 0; index < words.size(); ++index)
        append(plan, InitStepKind::write, static_cast<std::uint16_t>(first + index), words[index]);
}

}

bool build_tune_plan(std::uint64_t frequency_hz, std::uint8_t package,
                     std::uint8_t revision, TunePlan *plan)
{
    if (plan == nullptr || frequency_hz < 45000000 || frequency_hz > 860000000 ||
        !known_package(package) || !known_revision(revision))
        return false;

    const auto frequency_khz = static_cast<std::uint32_t>(frequency_hz / 1000);
    const auto band = frequency_khz <= 170000 ? FrequencyBand::c_band
        : frequency_khz <= 250000 ? FrequencyBand::vhf : FrequencyBand::uhf;
    const auto rf_request_khz = frequency_khz - (band == FrequencyBand::uhf ? 63 : 143);
    const auto variant = p1g_revision(revision) ? TunerVariant::p1g : TunerVariant::standard;
    const bool low_band_table = (package == 4 || package == 7) && band != FrequencyBand::uhf;

    const auto *pll = variant == TunerVariant::p1g
        ? find_band(kP1gPll, rf_request_khz)
        : find_band(kStandardPll, rf_request_khz);
    const TunerBand *tuner = nullptr;
    if (low_band_table) {
        tuner = variant == TunerVariant::p1g
            ? find_band(kP1gLowBand, rf_request_khz)
            : find_band(kStandardLowBand, rf_request_khz);
    } else {
        tuner = variant == TunerVariant::p1g
            ? find_band(kP1gTuner, rf_request_khz)
            : find_band(kStandardTuner, rf_request_khz);
    }
    if (pll == nullptr || tuner == nullptr) return false;

    const std::uint64_t vco_khz = static_cast<std::uint64_t>(pll->hf_divider) *
        rf_request_khz * 2;
    const auto intermediate = vco_khz / pll->prescaler;
    auto feedback_divider = static_cast<std::uint32_t>(intermediate / 2000);
    auto fraction = static_cast<std::uint32_t>(intermediate - feedback_divider * 2000);
    if (fraction < 100) {
        fraction = 0;
    } else if (fraction < 200) {
        fraction = 200;
    } else if (fraction > 1900) {
        fraction = 0;
        ++feedback_divider;
    } else if (fraction > 1800) {
        fraction = 1800;
    }
    fraction = fraction * 6528 / 200;
    const auto denominator = fraction == 0 ? 1 : 255;
    std::uint32_t control = 0x042C;
    if (variant == TunerVariant::standard) {
        if (fraction == 0)
            control = pll->vco_band != 0 ? 0x049F : 0x041F;
        else
            control = pll->vco_band != 0 ? 0x049E : 0x041D;
    }
    control |= static_cast<std::uint32_t>(pll->hf_code) << 11;
    control |= static_cast<std::uint32_t>(pll->vco_band) << 7;
    auto loop_filter = variant == TunerVariant::p1g || fraction == 0 ? 0xFFF8 : 0xFF28;
    if (fraction != 0) loop_filter |= 0x0006;

    *plan = {};
    plan->frequency_khz = frequency_khz;
    plan->rf_request_khz = rf_request_khz;
    plan->band = band;
    plan->variant = variant;
    plan->pll_fraction_zero = fraction == 0;
    append(plan, 0x0B, static_cast<std::uint16_t>(0xB800 | tuner->switch_trim));
    append(plan, 0x15, static_cast<std::uint16_t>(feedback_divider));
    append(plan, 0x16, static_cast<std::uint16_t>((denominator << 8) | 6));
    append(plan, 0x17, static_cast<std::uint16_t>(fraction));
    append(plan, 0x19, static_cast<std::uint16_t>(control));
    append(plan, 0x1C, static_cast<std::uint16_t>(loop_filter));
    append(plan, 0x24, static_cast<std::uint16_t>((tuner->enable & 0xFF9F) | 0x0006));
    plan->captrim_post_write_offset = plan->write_count;
    append(plan, 0x1E, 0x07FF);
    append(plan, 0x10, static_cast<std::uint16_t>((wbd_gain(frequency_hz) << 13) | 0x1F83));
    if (variant == TunerVariant::p1g && band == FrequencyBand::c_band &&
        tuner->enable == 0x88E9) {
        append(plan, 0x09, tuner->lna_bias);
        append(plan, 0x0B, static_cast<std::uint16_t>(0xB800 |
            (tuner->lna_tune << 6) | tuner->switch_trim));
    } else {
        append(plan, 0x09, static_cast<std::uint16_t>((tuner->lna_tune << 5) | tuner->lna_bias));
    }
    append(plan, 0x0C, tuner->v_to_i);
    append(plan, 0x0D, tuner->mixer);
    append(plan, 0x0E, tuner->load);
    return true;
}

bool build_demod_init_plan(DemodInitPlan *plan)
{
    if (plan == nullptr) return false;
    *plan = {};
    append(plan, InitStepKind::write, 0x0507, 0x0003);
    append(plan, InitStepKind::read, 896);
    append(plan, InitStepKind::read, 896);
    append(plan, InitStepKind::read, 897);
    append(plan, InitStepKind::write, 1287, 0x0003);
    append(plan, InitStepKind::write, 1025, 0x0100);
    append(plan, InitStepKind::write, 1028, 0);
    append(plan, InitStepKind::write, 774, 0);
    append(plan, InitStepKind::write, 775, 0);
    append(plan, InitStepKind::write, 776, 0);
    append(plan, InitStepKind::masked_write, 900, 0, 0xFFFC);
    append(plan, InitStepKind::masked_write, 1280, 0, 0x00FF);
    append(plan, InitStepKind::write, 907, 0x3000);
    append(plan, InitStepKind::write, 908, 0x007A);
    constexpr RegisterPair16 initial_registers[]{
        {770, 0xFFFF}, {771, 0xFFFF}, {772, 0xFFFC}, {898, 0x000C},
        {1280, 0x004D}, {1281, 0x000C}};
    for (const auto [reg, value] : initial_registers)
        append(plan, InitStepKind::write, reg, value);
    constexpr RegisterPair16 reset_registers[]{
        {770, 0}, {771, 0}, {772, 0}, {898, 0x0004}, {1280, 0}, {1281, 0}};
    for (const auto [reg, value] : reset_registers)
        append(plan, InitStepKind::write, reg, value);
    append(plan, InitStepKind::write, 906, 0x2D98);
    constexpr RegisterPair16 pll_registers[]{
        {901, 0x0112}, {902, 0x050F}, {902, 0x0507}, {904, 0x1E80},
        {23, 0x0337}, {24, 0xF980}, {27, 0}, {28, 0}, {26, 0}};
    for (const auto [reg, value] : pll_registers)
        append(plan, InitStepKind::write, reg, value);
    append(plan, InitStepKind::write, 922, 0xD257);
    append(plan, InitStepKind::write, 898, 0x0004);
    constexpr RegisterPair16 gpio_registers[]{
        {1029, 0xFFFF}, {1030, 0}, {1032, 0xFFFF}, {1037, 0}};
    for (const auto [reg, value] : gpio_registers)
        append(plan, InitStepKind::write, reg, value);
    append(plan, InitStepKind::masked_write, 299, 0x0022, 0x0050);
    append(plan, InitStepKind::write, 300, 0x0700);
    append(plan, InitStepKind::write, 1286, 0);
    append(plan, InitStepKind::write, 1291, 0x0205);
    append(plan, InitStepKind::write, 40, 0x0755);
    constexpr std::uint16_t words_7[]{4, 1024, 2068};
    append_words(plan, 7, words_7);
    constexpr std::uint16_t words_11[]{
        27, 30528, 91, 36224, 457, 50048, 0, 128, 0, 144, 1, 54464};
    append_words(plan, 11, words_11);
    constexpr std::uint16_t words_80[]{
        7250, 7238, 7264, 7309, 7338, 7382, 7427, 7456, 7500, 7544, 7574};
    append_words(plan, 80, words_80);
    constexpr std::uint16_t words_108[]{0, 0, 0, 0};
    append_words(plan, 108, words_108);
    append(plan, InitStepKind::write, 175, 1040);
    append(plan, InitStepKind::write, 179, 8192);
    constexpr std::uint16_t words_181[]{10240, 10240, 10240, 10240, 10240, 10240};
    append_words(plan, 181, words_181);
    constexpr std::uint16_t words_193[]{1638, 0};
    append_words(plan, 193, words_193);
    constexpr std::uint16_t words_205[]{8207, 15};
    append_words(plan, 205, words_205);
    constexpr std::uint16_t words_215[]{573, 164, 164, 32752, 15564};
    append_words(plan, 215, words_215);
    append(plan, InitStepKind::write, 230, 0);
    append(plan, InitStepKind::write, 263, 2048);
    append(plan, InitStepKind::write, 268, 1063);
    append(plan, InitStepKind::write, 270, 1);
    append(plan, InitStepKind::write, 285, 32);
    append(plan, InitStepKind::write, 299, 98);
    append(plan, InitStepKind::write, 338, 5217);
    append(plan, InitStepKind::write, 903, 0x0031);
    append(plan, InitStepKind::masked_write, 1285, 0, 0xFFFD);
    append(plan, InitStepKind::write, 29, 0x0134);
    append(plan, InitStepKind::write, 30, 0x392F);
    append(plan, InitStepKind::masked_write, 908, 0x0003, 0xFFFF);
    append(plan, InitStepKind::masked_write, 908, 0x0001, 0xFFFD);
    append(plan, InitStepKind::masked_write, 907, 0, 0xFFFF);
    append(plan, InitStepKind::write, 923, 0);
    append(plan, InitStepKind::write, 924, 0x0308);
    append(plan, InitStepKind::write, 923, 1);
    append(plan, InitStepKind::write, 923, 0);
    append(plan, InitStepKind::delay_ms, 0, 1);
    append(plan, InitStepKind::masked_write, 907, 0, 0xFFFF);
    append(plan, InitStepKind::masked_write, 908, 0x0003, 0xFFFF);
    append(plan, InitStepKind::masked_write, 285, 0x0060, 0xFF9F);
    append(plan, InitStepKind::write, 774, 0x3FFF);
    append(plan, InitStepKind::write, 775, 0xFFFF);
    append(plan, InitStepKind::write, 776, 0xFFFF);
    append(plan, InitStepKind::masked_write, 900, 0x0003, 0xFFFC);
    append(plan, InitStepKind::masked_write, 1280, 0, 0x00FF);
    append(plan, InitStepKind::read, 565);
    return plan->step_count <= sizeof(plan->steps) / sizeof(plan->steps[0]);
}

bool build_demod_agc_profile_plan(bool c_band, std::uint16_t wbd_reference,
                                 DemodInitPlan *plan)
{
    if (plan == nullptr || wbd_reference > 4095) return false;
    *plan = {};
    append(plan, InitStepKind::masked_write, 907, 0, 0x0FFF);
    append(plan, InitStepKind::masked_write, 908, 0, 0x0003);
    append(plan, InitStepKind::write, 76, 10346);
    append(plan, InitStepKind::write, 77, 787);
    append(plan, InitStepKind::write, 78, 10);
    append(plan, InitStepKind::write, 101, 0x0076);
    append(plan, InitStepKind::write, 102, 922);
    append(plan, InitStepKind::write, 103, 2035);
    append(plan, InitStepKind::write, 106, wbd_reference);
    append(plan, InitStepKind::write, 107, 0x0A00);
    append(plan, InitStepKind::write, 108, c_band ? 0 : 0xFFFF);
    append(plan, InitStepKind::write, 109, 0);
    append(plan, InitStepKind::write, 110, 0xFFFF);
    append(plan, InitStepKind::write, 111, 0);
    append(plan, InitStepKind::write, 112, 0x0020);
    append(plan, InitStepKind::write, 113, 0x8F90);
    append(plan, InitStepKind::write, 114, 0x72E3);
    append(plan, InitStepKind::write, 115, 0x7475);
    append(plan, InitStepKind::write, 75, 114);
    append(plan, InitStepKind::masked_write, 923, 0x0004, 0xFFE3);
    return plan->step_count <= sizeof(plan->steps) / sizeof(plan->steps[0]);
}

bool calculate_demod_agc_timing(std::uint64_t frequency_hz,
                                DemodAgcTiming *timing)
{
    if (timing == nullptr || frequency_hz < 45000000 || frequency_hz > 860000000)
        return false;
    constexpr std::uint32_t timfs[]{
        21387946, 20199727, 19136583, 18179756};
    const auto dividend = (frequency_hz / 1000 * 256) / 1000;
    std::uint32_t selected_ratio = 20;
    std::uint64_t selected_remainder = 717;
    for (std::uint32_t ratio = 17; ratio <= 20; ++ratio) {
        const auto divisor = 192 * ratio;
        auto remainder = dividend % divisor;
        if (remainder * 2 > divisor) remainder = divisor - remainder;
        if (remainder > selected_remainder) {
            selected_remainder = remainder;
            selected_ratio = ratio;
        }
    }
    timing->ratio = static_cast<std::uint8_t>(selected_ratio);
    timing->timf = timfs[selected_ratio - 17];
    return true;
}

bool begin_demod_agc(bool c_band, std::uint16_t wbd_reference,
                     DemodAgcState *state, DemodInitPlan *plan)
{
    if (state == nullptr || plan == nullptr) return false;
    *state = {};
    if (!build_demod_agc_profile_plan(c_band, wbd_reference, plan)) return false;
    append(plan, InitStepKind::delay_ms, 0, 16);
    state->started = true;
    return true;
}

DemodAgcStatus advance_demod_agc(DemodAgcState *state,
                                 bool previous_plan_succeeded,
                                 DemodInitPlan *plan)
{
    if (plan == nullptr) return DemodAgcStatus::failed;
    *plan = {};
    if (state == nullptr || state->failed || !state->started || state->stage > 3 ||
        !previous_plan_succeeded) {
        if (state != nullptr) state->failed = true;
        return DemodAgcStatus::failed;
    }
    if (state->complete) return DemodAgcStatus::complete;
    if (state->stage == 0) {
        append(plan, InitTarget::tuner, InitStepKind::write, 0x04, 0);
        append(plan, InitStepKind::write, 770, 0x0A00);
        append(plan, InitStepKind::write, 770, 0);
        append(plan, InitStepKind::delay_ms, 0, 14);
        ++state->stage;
        return DemodAgcStatus::in_progress;
    }
    if (state->stage == 1) {
        append(plan, InitStepKind::delay_ms, 0, 16);
        ++state->stage;
        return DemodAgcStatus::in_progress;
    }
    if (state->stage == 2) {
        append(plan, InitTarget::tuner, InitStepKind::write, 0x04, 1);
        append(plan, InitTarget::tuner, InitStepKind::write, 0x04, 1);
        ++state->stage;
        return DemodAgcStatus::in_progress;
    }
    state->complete = true;
    return DemodAgcStatus::complete;
}

bool build_demod_frequency_plan(std::uint64_t frequency_hz,
                                std::uint32_t tuner_actual_khz,
                                DemodFrequencyPlan *plan)
{
    if (plan == nullptr) return false;
    *plan = {};
    if (!calculate_demod_agc_timing(frequency_hz, &plan->timing)) return false;
    const auto requested_khz = frequency_hz / 1000;
    const auto offset = static_cast<std::int64_t>(tuner_actual_khz) -
        static_cast<std::int64_t>(requested_khz);
    const auto magnitude = offset < 0 ? -offset : offset;
    if (magnitude <= 27000) {
        plan->dds = static_cast<std::uint32_t>(
            static_cast<std::uint64_t>(offset * 1242) & 0x01FFFFFF);
        append(&plan->registers, InitStepKind::write, 26, 0);
        append(&plan->registers, InitStepKind::write, 27,
            (plan->dds >> 16) & 0x01FF);
        append(&plan->registers, InitStepKind::write, 28, plan->dds & 0xFFFF);
        plan->dds_written = true;
    }
    append(&plan->registers, InitStepKind::write, 29, plan->timing.timf >> 16);
    append(&plan->registers, InitStepKind::write, 30, plan->timing.timf & 0xFFFF);
    return true;
}

bool calculate_demod_edge_masks(std::uint16_t segment_mask,
                                std::uint16_t *left_edge_mask,
                                std::uint16_t *right_edge_mask)
{
    if (left_edge_mask == nullptr || right_edge_mask == nullptr || segment_mask > 0x1FFF)
        return false;
    *left_edge_mask = static_cast<std::uint16_t>(segment_mask &
        static_cast<std::uint16_t>(-segment_mask));
    *right_edge_mask = 0;
    for (std::uint8_t index = 0; index < 13; ++index)
        if ((segment_mask & (1U << index)) != 0)
            *right_edge_mask = static_cast<std::uint16_t>(1U << index);
    return true;
}

bool build_demod_channel_plan(const DemodChannelConfig *config,
                              DemodInitPlan *plan)
{
    if (config == nullptr || plan == nullptr || config->guard > 3 ||
        config->segment_mask > 0x1FFF)
        return false;
    std::uint16_t left_edge_mask = 0;
    std::uint16_t right_edge_mask = 0;
    if (!calculate_demod_edge_masks(config->segment_mask, &left_edge_mask,
                                    &right_edge_mask))
        return false;
    std::uint16_t mode = 0;
    switch (config->mode) {
    case DemodTransmissionMode::mode_2k: mode = 1; break;
    case DemodTransmissionMode::mode_4k: mode = 2; break;
    case DemodTransmissionMode::mode_8k: mode = 3; break;
    default: return false;
    }

    constexpr std::uint8_t permutation[]{6, 5, 7, 4, 8, 3, 9, 2, 10, 1, 11, 0, 12};
    std::uint16_t differential_mask = 0;
    std::size_t differential_count = 0;
    const auto partial_reception = config->searching || config->partial_reception;
    if (partial_reception) {
        differential_count = 1;
        if (config->layers[0].constellation == DemodConstellation::dqpsk) {
            differential_mask |= static_cast<std::uint16_t>(1U << permutation[0]);
        }
        for (std::size_t layer = 1; layer < 3; ++layer)
            if (config->layers[layer].constellation == DemodConstellation::dqpsk)
                differential_count += config->layers[layer].segments;
    } else {
        for (const auto &layer : config->layers)
            if (layer.constellation == DemodConstellation::dqpsk)
                differential_count += layer.segments;
    }
    if (differential_count > sizeof(permutation) / sizeof(permutation[0])) return false;
    const auto first_index = partial_reception ? 1U : 0U;
    for (std::size_t index = first_index; index < differential_count; ++index)
        differential_mask |= static_cast<std::uint16_t>(1U << permutation[index]);

    constexpr std::uint16_t adp_qam64[]{328, 65520, 164, 65528};
    constexpr std::uint16_t adp_qam16[]{573, 65503, 164, 65520};
    constexpr std::uint16_t adp_other[]{2458, 65454, 819, 65528};
    constexpr std::uint16_t adc_target[]{
        7250, 7238, 7264, 7309, 7338, 7382, 7427, 7456, 7500, 7544};
    constexpr std::uint16_t adc_target_no_ana_gain[]{
        6895, 6883, 6909, 6954, 6983, 7027, 7072, 7101, 7145, 7189};
    constexpr std::uint16_t ana_fe_one[]{
        249, 226, 164, 82, 5, 981, 970, 988, 1018, 20, 31, 26,
        8, 1012, 1000, 1018, 1012, 8, 15, 14, 9, 3, 1017, 1003};
    constexpr std::uint16_t ana_fe_three[]{
        81, 80, 78, 74, 68, 61, 54, 45, 37, 28, 19, 11,
        4, 1022, 1017, 1013, 1010, 1008, 1008, 1008, 1008, 1010, 1014, 1017};
    constexpr std::int16_t ana_fe_thirteen[]{
        396, 305, 105, -51, -77, -12, 41, 31, -11, -30, -11, 14,
        15, -2, -13, -7, 5, 8, 1, -6, -7, -3, 0, 1};

    *plan = {};
    append(plan, InitStepKind::write, 10,
        static_cast<std::uint16_t>(config->sequence << 4));
    append(plan, InitStepKind::masked_write, 1, config->guard, 0xFFFC);
    append(plan, InitStepKind::masked_write, 274,
        (static_cast<std::uint16_t>(partial_reception) << 5) |
            (static_cast<std::uint16_t>(config->sb_mode) << 4), 0xFFCF);
    if (differential_mask != 0)
        append(plan, InitStepKind::masked_write, 268, 0x0200, 0xF9FF);
    else
        append(plan, InitStepKind::write, 268, 1063);

    std::uint16_t max_constellation = 0;
    std::uint32_t tmcc_weighted_segments = 0;
    for (std::size_t index = 0; index < 3; ++index) {
        const auto &layer = config->layers[index];
        std::uint16_t constellation = 0;
        switch (layer.constellation) {
        case DemodConstellation::dqpsk: constellation = 0; break;
        case DemodConstellation::qpsk: constellation = 1; break;
        case DemodConstellation::qam16: constellation = 2; break;
        case DemodConstellation::qam64: constellation = 3; break;
        default: return false;
        }
        max_constellation = std::max(max_constellation, constellation);
        std::uint16_t fec = 7;
        switch (layer.fec) {
        case DemodFec::rate_1_2: fec = 1; break;
        case DemodFec::rate_2_3: fec = 2; break;
        case DemodFec::rate_3_4: fec = 3; break;
        case DemodFec::rate_5_6: fec = 5; break;
        case DemodFec::other: break;
        default: return false;
        }
        std::uint16_t interleave = 0;
        for (auto value = layer.interleave; value != 0; value >>= 1) ++interleave;
        if (interleave > 3 && !(layer.interleave == 4 && config->sb_mode))
            interleave = 0;
        append(plan, InitStepKind::write, static_cast<std::uint16_t>(2 + index),
            static_cast<std::uint16_t>((constellation << 10) |
                ((layer.segments & 0x0F) << 6) | (fec << 3) | interleave));
        tmcc_weighted_segments += (layer.constellation == DemodConstellation::dqpsk ? 5U : 1U) *
            layer.segments;
    }
    append(plan, InitStepKind::write, 0,
        static_cast<std::uint16_t>((mode << 13) | differential_mask));
    const std::span<const std::uint16_t> adp = max_constellation == 3
        ? std::span<const std::uint16_t>(adp_qam64)
        : max_constellation == 2 ? std::span<const std::uint16_t>(adp_qam16)
                                 : std::span<const std::uint16_t>(adp_other);
    append_words(plan, 215, adp);
    const auto ana_gain = max_constellation >= 2 ? 7 : 0;
    append(plan, InitStepKind::write, 116, ana_gain);
    const std::span<const std::uint16_t> adc = ana_gain == 0
        ? std::span<const std::uint16_t>(adc_target_no_ana_gain)
        : std::span<const std::uint16_t>(adc_target);
    append_words(plan, 80, adc);
    if (!config->configuration_loaded) {
        append_words(plan, 117, partial_reception ? ana_fe_three : ana_fe_one);
        std::uint16_t coefficients[24]{};
        for (std::size_t index = 0; index < 24; ++index)
            coefficients[index] = static_cast<std::uint16_t>(ana_fe_thirteen[index]);
        append_words(plan, 117, coefficients);
    }
    append(plan, InitStepKind::write, 180, 0x0409);
    append(plan, InitStepKind::write, 187, 0x4102);
    constexpr std::uint16_t words_181[]{
        0x2800, 0x2800, 0x2800, 0x2800, 0x2800, 0x2800};
    append_words(plan, 181, words_181);
    append(plan, InitStepKind::write, 338, 0x1461);
    append(plan, InitStepKind::write, 340, 0x0206);
    append(plan, InitStepKind::write, 341, 0x0027);
    append(plan, InitStepKind::write, 228, 0);
    append(plan, InitStepKind::write, 265, 31);
    append(plan, InitStepKind::write, 205, 0x200F);
    append(plan, InitStepKind::write, 266,
        static_cast<std::uint16_t>(~config->segment_mask | differential_mask | 0x0040));
    append(plan, InitStepKind::write, 352, differential_mask);
    append(plan, InitStepKind::write, 353, 0x1FFF);
    append(plan, InitStepKind::write, 351, 0x00D5);
    append(plan, InitStepKind::write, 1,
        static_cast<std::uint16_t>(0x3FFC | config->guard));
    append(plan, InitStepKind::write, 222, left_edge_mask);
    append(plan, InitStepKind::write, 223, right_edge_mask);
    const auto segment_complement = static_cast<std::uint16_t>(
        ~config->segment_mask | differential_mask);
    append(plan, InitStepKind::write, 189, segment_complement);
    append(plan, InitStepKind::write, 192, segment_complement);
    append(plan, InitStepKind::write, 225, segment_complement);
    append(plan, InitStepKind::write, 288, config->searching
        ? 0x1FFF : static_cast<std::uint16_t>(segment_complement & 0x1FFF));
    append(plan, InitStepKind::write, 211,
        static_cast<std::uint16_t>(config->segment_mask & ~differential_mask));
    append(plan, InitStepKind::write, 287,
        static_cast<std::uint16_t>(~config->segment_mask | 0x1000));
    append(plan, InitStepKind::write, 178, 32);
    const auto tmcc_threshold = static_cast<std::uint16_t>(tmcc_weighted_segments * 128);
    append(plan, InitStepKind::write, 290, tmcc_threshold);
    append(plan, InitStepKind::write, 291, tmcc_threshold);
    append(plan, InitStepKind::write, 292, tmcc_threshold);
    append(plan, InitStepKind::write, 250, 3285);
    return plan->step_count <= sizeof(plan->steps) / sizeof(plan->steps[0]);
}

bool select_demod_search(std::uint16_t revision, bool mode_auto, bool guard_auto,
                         DemodTransmissionMode requested_mode,
                         std::uint8_t requested_guard,
                         DemodSearchSelection *selection)
{
    if (selection == nullptr || (revision != 0x8000 && revision != 0x8001 &&
        revision != 0x8002) || (!guard_auto && requested_guard > 3))
        return false;
    if (!mode_auto) {
        switch (requested_mode) {
        case DemodTransmissionMode::mode_2k:
        case DemodTransmissionMode::mode_4k:
        case DemodTransmissionMode::mode_8k: break;
        default: return false;
        }
    }
    selection->strategy = revision == 0x8002 && (mode_auto || guard_auto)
        ? DemodSearchStrategy::two_pass : DemodSearchStrategy::one_pass;
    selection->mode = mode_auto ? DemodTransmissionMode::mode_8k : requested_mode;
    selection->guard = guard_auto ? 2 : requested_guard;
    selection->list = mode_auto && guard_auto ? 7
        : guard_auto ? 3 : mode_auto ? 2 : 0;
    selection->restart_mode = mode_auto;
    return true;
}

bool build_demod_fft_search_plan(DemodInitPlan *plan)
{
    if (plan == nullptr) return false;
    *plan = {};
    append(plan, InitStepKind::write, 37, 0x0065);
    append(plan, InitStepKind::write, 116, 0);
    append(plan, InitStepKind::masked_write, 0, 0x8000, 0x1FFF);
    append(plan, InitStepKind::masked_write, 1, 0, 0xFFFC);
    append(plan, InitStepKind::write, 6, 0);
    append(plan, InitStepKind::write, 7, 0);
    append(plan, InitStepKind::write, 8, 0);
    append(plan, InitStepKind::masked_write, 10, 0x0100, 0x0200);
    append(plan, InitStepKind::write, 11, 0x0010);
    append(plan, InitStepKind::write, 12, 0x7AC0);
    append(plan, InitStepKind::write, 13, 0x0010);
    append(plan, InitStepKind::write, 14, 0x7AC0);
    append(plan, InitStepKind::write, 15, 0x0010);
    append(plan, InitStepKind::write, 16, 0x7AC0);
    append(plan, InitStepKind::write, 17, 0);
    append(plan, InitStepKind::write, 18, 0x00C8);
    append(plan, InitStepKind::write, 19, 0);
    append(plan, InitStepKind::write, 20, 0x0190);
    append(plan, InitStepKind::write, 21, 0x0010);
    append(plan, InitStepKind::write, 22, 0x7AC0);
    append(plan, InitStepKind::masked_write, 32, 0x0900, 0xF0FF);
    append(plan, InitStepKind::write, 355, 0x0002);
    append(plan, InitStepKind::write, 356, 0x0000);
    append(plan, InitStepKind::write, 357, 0x0111);
    append(plan, InitStepKind::masked_write, 770, 0x2000, 0xDFFF);
    append(plan, InitStepKind::masked_write, 770, 0x0000, 0xDFFF);
    append(plan, InitStepKind::masked_write, 0, 0x2000, 0x07FF);
    append(plan, InitStepKind::delay_ms, 0, 10);
    append(plan, InitStepKind::read, 594);
    append(plan, InitStepKind::read, 595);
    append(plan, InitStepKind::read, 596);
    append(plan, InitStepKind::read, 597);
    append(plan, InitStepKind::read, 598);
    append(plan, InitStepKind::read, 599);
    append(plan, InitStepKind::read, 1284);
    return plan->step_count <= sizeof(plan->steps) / sizeof(plan->steps[0]);
}

namespace {

void append_demod_search_restart(DemodInitPlan *plan)
{
    append(plan, InitStepKind::read, 0);
    append(plan, InitStepKind::masked_write, 0, 0x8000, 0x7FFF);
    append(plan, InitStepKind::read, 1284);
    append(plan, InitStepKind::write_saved, 0);
}

void append_demod_search_wait_registers(DemodInitPlan *plan, bool include_100_ms)
{
    append(plan, InitStepKind::write, 11, 0x0029);
    append(plan, InitStepKind::write, 12, 0x32E0);
    if (include_100_ms) {
        append(plan, InitStepKind::write, 13, 0x0052);
        append(plan, InitStepKind::write, 14, 0x65C0);
    } else {
        append(plan, InitStepKind::write, 13, 0x00A4);
        append(plan, InitStepKind::write, 14, 0xCB80);
    }
    append(plan, InitStepKind::write, 15, 0x0337);
    append(plan, InitStepKind::write, 16, 0xF980);
}

bool append_demod_channel_plan(const DemodChannelConfig *channel,
                               DemodInitPlan *plan)
{
    DemodInitPlan channel_plan{};
    if (!build_demod_channel_plan(channel, &channel_plan) ||
        channel_plan.step_count + 16 > sizeof(plan->steps) / sizeof(plan->steps[0]))
        return false;
    for (std::size_t index = 0; index < channel_plan.step_count; ++index) {
        const auto &step = channel_plan.steps[index];
        append(plan, step.target, step.kind, step.reg, step.value, step.mask);
    }
    return true;
}

void apply_demod_search_channel(DemodChannelConfig *channel,
                                DemodTransmissionMode mode,
                                std::uint8_t guard,
                                std::uint8_t sequence)
{
    channel->mode = mode;
    channel->guard = guard;
    channel->sequence = sequence;
    channel->partial_reception = true;
    channel->sb_mode = false;
    channel->searching = true;
    channel->layers[0] = {DemodConstellation::qam64, DemodFec::rate_2_3, 13, 0};
    channel->layers[1] = {DemodConstellation::qam64, DemodFec::other, 0, 0};
    channel->layers[2] = {DemodConstellation::qam64, DemodFec::other, 0, 0};
    channel->segment_mask = 0x1FFF;
}

}

bool build_demod_one_pass_search_plan(const DemodSearchSelection *selection,
                                      const DemodChannelConfig *channel,
                                      DemodInitPlan *plan)
{
    if (selection == nullptr || channel == nullptr || plan == nullptr ||
        selection->strategy != DemodSearchStrategy::one_pass || selection->guard > 3)
        return false;
    auto search_channel = *channel;
    if (selection->list != 0)
        apply_demod_search_channel(&search_channel, selection->mode,
                                   selection->guard, selection->list);
    else {
        search_channel.mode = selection->mode;
        search_channel.guard = selection->guard;
        search_channel.sequence = selection->list;
        search_channel.searching = true;
    }
    *plan = {};
    if (!append_demod_channel_plan(&search_channel, plan)) return false;
    append(plan, InitStepKind::write, 6, 0x0004);
    append(plan, InitStepKind::write, 7, 0x0008);
    append(plan, InitStepKind::write, 8, 0x1000);
    append_demod_search_wait_registers(plan, true);
    append_demod_search_restart(plan);
    return plan->step_count <= sizeof(plan->steps) / sizeof(plan->steps[0]);
}

bool build_demod_guard_search_plan(DemodTransmissionMode mode,
                                   const DemodChannelConfig *search_channel,
                                   DemodInitPlan *plan)
{
    if (search_channel == nullptr || plan == nullptr) return false;
    auto channel = *search_channel;
    apply_demod_search_channel(&channel, mode, 2, 16);
    *plan = {};
    if (!append_demod_channel_plan(&channel, plan)) return false;
    append(plan, InitStepKind::write, 6, 0x0004);
    append(plan, InitStepKind::write, 7, 0x0008);
    append(plan, InitStepKind::write, 8, 0x1000);
    append_demod_search_wait_registers(plan, false);
    append(plan, InitStepKind::write, 355, 0x0003);
    append(plan, InitStepKind::write, 356, 0x0000);
    append(plan, InitStepKind::write, 357, 0x000F);
    append_demod_search_restart(plan);
    append(plan, InitStepKind::read, 570);
    return plan->step_count <= sizeof(plan->steps) / sizeof(plan->steps[0]);
}

DemodFftSearchResult resolve_demod_fft_search(
    bool completion_transition_observed,
    const std::uint32_t (&correlations)[3])
{
    if (!completion_transition_observed) return DemodFftSearchResult::pending;
    std::size_t maximum_index = 0;
    for (std::size_t index = 1; index < 3; ++index)
        if (correlations[index] > correlations[maximum_index]) maximum_index = index;
    switch (maximum_index) {
    case 0: return DemodFftSearchResult::mode_2k;
    case 1: return DemodFftSearchResult::mode_4k;
    default: return DemodFftSearchResult::mode_8k;
    }
}

void decode_demod_search_result(
    std::uint16_t register_570, std::uint16_t register_505,
    const std::uint16_t (&register_493)[3],
    const std::uint16_t (&register_499)[3],
    const std::uint16_t (&register_481)[3],
    const std::uint16_t (&register_487)[3],
    DemodSearchResult *result)
{
    if (result == nullptr) return;
    result->inversion = (register_570 & 0x0040) != 0;
    switch ((register_570 >> 4) & 0x3) {
    case 1: result->mode = DemodTransmissionMode::mode_2k; break;
    case 2: result->mode = DemodTransmissionMode::mode_4k; break;
    default: result->mode = DemodTransmissionMode::mode_8k; break;
    }
    result->guard = static_cast<std::uint8_t>(register_570 & 0x0003);
    result->partial_reception = (register_505 & 0x0001) != 0;
    for (std::size_t index = 0; index < 3; ++index) {
        result->layers[index].segments = static_cast<std::uint8_t>(register_493[index] & 0x000F);
        const auto interleave = static_cast<std::uint8_t>(register_499[index] & 0x0003);
        result->layers[index].interleave = interleave == 3 ? 4 : interleave;
        switch (register_481[index] & 0x0007) {
        case 1: result->layers[index].fec = DemodFec::rate_1_2; break;
        case 2: result->layers[index].fec = DemodFec::rate_2_3; break;
        case 3: result->layers[index].fec = DemodFec::rate_3_4; break;
        case 5: result->layers[index].fec = DemodFec::rate_5_6; break;
        default: result->layers[index].fec = DemodFec::other; break;
        }
        switch (register_487[index] & 0x0003) {
        case 0: result->layers[index].constellation = DemodConstellation::dqpsk; break;
        case 1: result->layers[index].constellation = DemodConstellation::qpsk; break;
        case 2: result->layers[index].constellation = DemodConstellation::qam16; break;
        default: result->layers[index].constellation = DemodConstellation::qam64; break;
        }
    }
}

bool calculate_demod_loop_values(DemodTransmissionMode mode, bool tracking,
                                 std::uint16_t *register_32,
                                 std::uint16_t *register_37)
{
    if (register_32 == nullptr || register_37 == nullptr) return false;
    std::uint16_t mode_number = 0;
    switch (mode) {
    case DemodTransmissionMode::mode_2k: mode_number = 1; break;
    case DemodTransmissionMode::mode_4k: mode_number = 2; break;
    case DemodTransmissionMode::mode_8k: mode_number = 3; break;
    default: return false;
    }
    *register_32 = static_cast<std::uint16_t>(
        ((tracking ? 11U : 9U) - mode_number) << 12 | 0x0680);
    *register_37 = tracking
        ? static_cast<std::uint16_t>(((5U + mode_number) << 5) | (10U - mode_number))
        : static_cast<std::uint16_t>((3U << 5) | (8U - mode_number));
    return true;
}

bool calculate_demod_sync_wait(DemodTransmissionMode mode, std::uint8_t guard,
                               std::uint16_t *sync_wait)
{
    if (sync_wait == nullptr || guard > 3) return false;
    std::uint16_t base = 0;
    switch (mode) {
    case DemodTransmissionMode::mode_2k: base = 64; break;
    case DemodTransmissionMode::mode_4k: base = 128; break;
    case DemodTransmissionMode::mode_8k: base = 256; break;
    default: return false;
    }
    *sync_wait = static_cast<std::uint16_t>(base * (1U << guard) * 3 / 2 + 48);
    return true;
}

bool select_demod_mpeg_lock_layer(const DemodLayerConfig (&layers)[3],
                                  std::uint8_t *layer_index)
{
    if (layer_index == nullptr) return false;
    bool found = false;
    for (std::uint8_t index = 0; index < 3; ++index)
        if (layers[index].segments >= 1 && layers[index].interleave >= layers[0].interleave) {
            *layer_index = index;
            found = true;
        }
    return found;
}

std::uint32_t demod_mpeg_lock_timeout_ms(std::uint8_t layer_a_interleave)
{
    const auto coefficient = layer_a_interleave == 0 ? 2U
        : layer_a_interleave == 3 ? 8U : 2U * layer_a_interleave;
    return 200U * coefficient;
}

bool build_frontend_power_plan(FrontendPowerPlan *plan)
{
    if (plan == nullptr) return false;
    *plan = {};
    append(plan, InitStepKind::masked_write, 0x00090018,
        1U << 22, ~(1U << 16 | 1U << 22));
    append(plan, InitStepKind::masked_write, 0x00090014,
        (1U << 16) | (1U << 22), ~(1U << 16 | 1U << 22));
    append(plan, InitStepKind::delay_ms, 0, 50);
    append(plan, InitStepKind::masked_write, 0x00090018,
        (1U << 16) | (1U << 22), ~(1U << 16 | 1U << 22));
    append(plan, InitStepKind::delay_ms, 0, 10);
    return true;
}

bool decode_tuner_identity(std::uint16_t identity, TunerIdentityValues *values)
{
    if (values == nullptr || ((identity >> 8) & 0x7) != 2) return false;
    const auto version = static_cast<std::uint8_t>(identity);
    const auto package = static_cast<std::uint8_t>((version >> 5) & 0x7);
    const auto revision = static_cast<std::uint8_t>(version & 0x1F);
    if ((version & 0x3) == 2 || !known_package(package) || !known_revision(revision))
        return false;
    *values = {package, revision, version,
        p1g_revision(revision) ? TunerVariant::p1g : TunerVariant::standard};
    return true;
}

bool tuner_init_needs_calibration_measurement(const TunerIdentityValues &identity,
                                             std::uint16_t redundancy_a)
{
    return identity.revision >= 3 &&
        (identity.version_byte == 3 || identity.version_byte == 7 || redundancy_a == 0xFFFF);
}

bool build_tuner_init_gate_plan(TunerInitPlan *plan)
{
    if (plan == nullptr) return false;
    *plan = {};
    append(plan, InitTarget::demodulator, InitStepKind::write, 1025, 0xC000);
    return true;
}

bool build_tuner_init_reset_plan(TunerInitPlan *plan)
{
    if (plan == nullptr) return false;
    *plan = {};
    append(plan, InitTarget::demodulator, InitStepKind::masked_write, 1030, 0, 0xFFFE);
    append(plan, InitTarget::demodulator, InitStepKind::delay_ms, 0, 10);
    append(plan, InitTarget::demodulator, InitStepKind::masked_write, 1030, 0, 0xFFDF);
    append(plan, InitTarget::demodulator, InitStepKind::delay_ms, 0, 10);
    append(plan, InitTarget::demodulator, InitStepKind::masked_write, 1030, 0x0020, 0xFFDF);
    append(plan, InitTarget::demodulator, InitStepKind::delay_ms, 0, 10);
    append(plan, InitTarget::tuner, InitStepKind::write, 0x24, 0x0022);
    append(plan, InitTarget::tuner, InitStepKind::write, 0x1B, 0x00A2);
    append(plan, InitTarget::tuner, InitStepKind::write, 0x20, 0x3B00);
    append(plan, InitTarget::tuner, InitStepKind::write, 0x23, 0x0A30);
    append(plan, InitTarget::tuner, InitStepKind::masked_write, 0x21, 0x8000, 0x7FFF);
    return true;
}

bool build_tuner_init_defaults_plan(const TunerIdentityValues &identity,
                                    std::uint16_t second_identity,
                                    TunerInitPlan *plan)
{
    if (plan == nullptr || !known_package(identity.package_code) ||
        !known_revision(identity.revision)) return false;
    *plan = {};
    append(plan, InitTarget::tuner, InitStepKind::write, 0x1B,
        (second_identity & 0x0040) != 0 ? 0x00EB : 0x00A2);
    constexpr std::uint16_t standard_defaults[]{
        0x0000, 0x99A0, 0x6008, 0x0000, 0x8BCB, 0x0000, 0x0405, 0x0000,
        0x0000, 0x0000, 0xB802, 0x0300, 0x2D12, 0xBAC0, 0x7C00, 0xDBB9,
        0x0954, 0x0743, 0x8000, 0x0001, 0x0040, 0x0100, 0x0000, 0xE910, 0x149E};
    for (std::size_t index = 0; index < sizeof(standard_defaults) / sizeof(standard_defaults[0]); ++index)
        append(plan, InitTarget::tuner, InitStepKind::write,
            static_cast<std::uint16_t>(0x01 + index), standard_defaults[index]);
    append(plan, InitTarget::tuner, InitStepKind::write, 0x1C, 0xFF2D);
    append(plan, InitTarget::tuner, InitStepKind::write, 0x39, 0);
    append(plan, InitTarget::tuner, InitStepKind::write, 0x1E, 0x07FF);
    append(plan, InitTarget::tuner, InitStepKind::write, 0x1F, 0x0007);
    append(plan, InitTarget::tuner, InitStepKind::write, 0x24, 0x22EB);
    append(plan, InitTarget::tuner, InitStepKind::write, 0x3C, 0x03FF);
    append(plan, InitTarget::tuner, InitStepKind::write, 0x3D, 0x0111);
    if (identity.variant == TunerVariant::p1g) {
        append(plan, InitTarget::tuner, InitStepKind::write, 0x05, 0xABCD);
        append(plan, InitTarget::tuner, InitStepKind::write, 0x11, 0x00B4);
        append(plan, InitTarget::tuner, InitStepKind::write, 0x1C, 0xFFFD);
        append(plan, InitTarget::tuner, InitStepKind::write, 0x40, 0x0108);
    }
    return true;
}

bool build_tuner_init_finish_plan(const TunerIdentityValues &identity,
                                 std::uint16_t redundancy_a,
                                 std::uint16_t redundancy_b,
                                 std::uint16_t calibration_raw,
                                 TunerInitPlan *plan)
{
    if (plan == nullptr || !known_package(identity.package_code) ||
        !known_revision(identity.revision)) return false;
    *plan = {};
    // 測定条件は冗長値A/Bの統合前に判定する。
    if (tuner_init_needs_calibration_measurement(identity, redundancy_a)) {
        auto measurement = static_cast<std::uint16_t>((calibration_raw >> 6) & 0x03FF);
        if (measurement < 670 || measurement == 1023) measurement = 850;
        const auto poly = static_cast<std::uint16_t>(165 - ((measurement * 10) >> 6));
        redundancy_a = static_cast<std::uint16_t>((3 << 12) | (34 << 6) | poly);
        redundancy_b = redundancy_a;
    }
    const auto redundancy = redundancy_a == redundancy_b
        ? redundancy_a
        : static_cast<std::uint16_t>(redundancy_a & redundancy_b);
    if (redundancy != 0xFFFF) {
        auto cap_trim = static_cast<std::uint16_t>(redundancy & 0x003F);
        auto poly = static_cast<std::uint16_t>((redundancy >> 12) & 0x000F);
        auto high_resistance = static_cast<std::uint16_t>((redundancy >> 6) & 0x003F);
        cap_trim = cap_trim <= 9 || cap_trim >= 40
            ? 32 : static_cast<std::uint16_t>(cap_trim + 14);
        if (high_resistance <= 25 || high_resistance >= 40) high_resistance = 34;
        if (poly == 0 || poly >= 8) poly = 3;
        append(plan, InitTarget::tuner, InitStepKind::write, 0x13,
            static_cast<std::uint16_t>(high_resistance << 10));
        append(plan, InitTarget::tuner, InitStepKind::write, 0x02,
            static_cast<std::uint16_t>((poly << 11) | ((high_resistance >> 2) << 6) | cap_trim));
    }
    append(plan, InitTarget::tuner, InitStepKind::write, 0x14, 2);
    append(plan, InitTarget::demodulator, InitStepKind::masked_write, 1030, 0, 0xFFFE);
    append(plan, InitTarget::tuner, InitStepKind::masked_write, 0x23, 0x4000, 0xBFFF);
    return plan->step_count <= sizeof(plan->steps) / sizeof(plan->steps[0]);
}

bool build_tuner_init_plan(std::uint16_t identity, std::uint16_t second_identity,
                           std::uint16_t redundancy_a, std::uint16_t redundancy_b,
                           std::uint16_t calibration_raw, TunerInitPlan *plan)
{
    TunerIdentityValues values{};
    if (plan == nullptr || !decode_tuner_identity(identity, &values)) return false;
    *plan = {};
    const auto append_segment = [plan](const TunerInitPlan &segment) {
        if (segment.step_count > sizeof(plan->steps) / sizeof(plan->steps[0]) -
                plan->step_count) return false;
        for (std::size_t index = 0; index < segment.step_count; ++index)
            plan->steps[plan->step_count++] = segment.steps[index];
        return true;
    };
    TunerInitPlan segment{};
    if (!build_tuner_init_gate_plan(&segment) || !append_segment(segment) ||
        !build_tuner_init_reset_plan(&segment) || !append_segment(segment) ||
        !build_tuner_init_defaults_plan(values, second_identity, &segment) ||
        !append_segment(segment) ||
        !build_tuner_init_finish_plan(values, redundancy_a, redundancy_b,
            calibration_raw, &segment) || !append_segment(segment)) return false;
    return true;
}

bool build_wbd_calibration_plan(std::uint64_t frequency_hz,
                                std::uint16_t previous_gain,
                                TunerCalibrationPlan *plan)
{
    if (plan == nullptr || frequency_hz < 45000000 || frequency_hz > 860000000)
        return false;
    *plan = {};
    plan->wbd_calibration = true;
    plan->calibration_gain = wbd_gain(frequency_hz);
    if (plan->calibration_gain == previous_gain) {
        plan->calibration_flags_to_clear = tuner_calibration_wbd;
        plan->complete = true;
        return true;
    }

    append(plan, InitStepKind::write, 0x10,
        static_cast<std::uint16_t>((plan->calibration_gain << 13) | 0x1F89));
    append(plan, InitStepKind::write, 0x24, 0x02EB);
    plan->wait_tenth_ms = 90;
    plan->measurement_register = 0x1D;
    plan->needs_measurement = true;
    return true;
}

bool complete_wbd_calibration(const TunerCalibrationPlan *plan,
                              bool measurement_available,
                              std::uint16_t measurement,
                              std::uint16_t previous_offset,
                              WbdCalibrationResult *result)
{
    if (plan == nullptr || result == nullptr || !plan->wbd_calibration ||
        plan->complete == plan->needs_measurement ||
        (plan->needs_measurement && !measurement_available))
        return false;
    result->gain = plan->calibration_gain;
    result->offset = plan->needs_measurement ? measurement : previous_offset;
    result->calibration_flags_to_clear = tuner_calibration_wbd;
    return true;
}

bool dc_calibration_wait(std::uint8_t stage, std::uint16_t *wait_tenth_ms)
{
    constexpr std::uint16_t waits[]{50, 50, 0, 100, 100, 0, 0, 0};
    if (wait_tenth_ms == nullptr || stage >= sizeof(waits) / sizeof(waits[0])) return false;
    *wait_tenth_ms = waits[stage];
    return true;
}

std::int8_t dc_initial_direction(std::int32_t difference)
{
    return difference < 0 ? -1 : difference > 0 ? 1 : 0;
}

bool dc_sign_matches(std::int32_t difference, std::int32_t minimum_difference)
{
    return (difference < 0) == (minimum_difference < 0);
}

bool begin_dc_calibration(TunerVariant variant, std::uint16_t register_24,
                          std::uint16_t wbd_mux, std::uint16_t register_23,
                          DcCalibrationState *state,
                          TunerCalibrationPlan *plan)
{
    if (state == nullptr || plan == nullptr ||
        (variant != TunerVariant::standard && variant != TunerVariant::p1g))
        return false;
    *state = {};
    state->variant = variant;
    state->bb6 = 0;
    state->bb7 = 0x040D;
    state->minimum_adc_difference = 1023;
    *plan = {};
    append(plan, InitStepKind::write, 0x24,
        static_cast<std::uint16_t>(register_24 & 0x0FFB));
    append(plan, InitStepKind::write, 0x10,
        static_cast<std::uint16_t>((wbd_mux & ~0x07F8) | 0x003B));
    append(plan, InitStepKind::write, 0x23,
        static_cast<std::uint16_t>(register_23 & ~0x4000));
    plan->wait_tenth_ms = 50;
    return true;
}

DcCalibrationStatus advance_dc_calibration(DcCalibrationState *state,
                                           bool measurement_available,
                                           std::uint16_t measurement,
                                           TunerCalibrationPlan *plan)
{
    if (plan == nullptr) return DcCalibrationStatus::failed;
    *plan = {};
    if (state == nullptr || state->failed)
        return DcCalibrationStatus::failed;
    if (state->complete) return DcCalibrationStatus::complete;
    if ((state->variant != TunerVariant::standard && state->variant != TunerVariant::p1g) ||
        state->row > 4 || state->stage > 6 || state->search_step > 31 ||
        (state->row == 4 && state->stage != 6) ||
        (state->stage == 6 && state->row != 4)) {
        state->failed = true;
        return DcCalibrationStatus::failed;
    }

    struct DcRow {
        std::uint8_t reg;
        std::uint8_t trim_position;
        std::uint8_t pga;
        std::uint16_t bb1;
        std::uint8_t component;
    };
    constexpr DcRow standard_rows[]{
        {0x06, 5, 1, 0x20D0, 0}, {0x07, 11, 1, 0x20D0, 1},
        {0x06, 0, 0, 0x3DD0, 0}, {0x06, 10, 0, 0x3DD0, 1}};
    constexpr DcRow p1g_rows[]{
        {0x06, 5, 1, 0x2078, 0}, {0x07, 11, 1, 0x2078, 1},
        {0x06, 0, 0, 0x3D78, 0}, {0x06, 10, 0, 0x3D78, 1}};
    constexpr std::size_t row_count = 4;
    const DcRow *rows = state->variant == TunerVariant::p1g ? p1g_rows : standard_rows;
    const auto &row = rows[state->row < row_count ? state->row : row_count - 1];
    *plan = {};

    const auto write_trim = [&]() {
        auto &base = row.reg == 0x06 ? state->bb6 : state->bb7;
        const auto mask = static_cast<std::uint16_t>(0x001F << row.trim_position);
        base = static_cast<std::uint16_t>((base & ~mask) |
            ((static_cast<std::uint16_t>(state->search_step) << row.trim_position) & mask));
        append(plan, InitStepKind::write, row.reg, base);
    };
    const auto magnitude = [](std::int32_t value) {
        return value < 0 ? -static_cast<std::int64_t>(value)
                         : static_cast<std::int64_t>(value);
    };

    switch (state->stage) {
    case 0:
        append(plan, InitStepKind::write, 0x01, row.bb1);
        state->bb7 = static_cast<std::uint16_t>(0x040D | (row.component << 7));
        append(plan, InitStepKind::write, 0x07, state->bb7);
        state->search_step = 0;
        state->minimum_adc_difference = 1023;
        state->stage = 1;
        plan->wait_tenth_ms = 50;
        break;
    case 1:
        write_trim();
        state->stage = 2;
        break;
    case 2:
        append(plan, InitStepKind::write, 0x1F, 7);
        state->stage = 3;
        plan->wait_tenth_ms = 100;
        plan->measurement_register = 0x1D;
        plan->needs_measurement = true;
        break;
    case 3:
        if (!measurement_available) {
            state->failed = true;
            return DcCalibrationStatus::failed;
        }
        state->positive_adc = measurement;
        append(plan, InitStepKind::write, 0x1F, 4);
        state->stage = 4;
        plan->wait_tenth_ms = 100;
        plan->measurement_register = 0x1D;
        plan->needs_measurement = true;
        break;
    case 4: {
        if (!measurement_available) {
            state->failed = true;
            return DcCalibrationStatus::failed;
        }
        const auto difference = state->positive_adc -
            static_cast<std::int32_t>(measurement);
        state->adc_difference = difference;
        state->stage = 5;
        break;
    }
    case 5: {
        const auto difference = state->adc_difference;
        const auto direction = dc_initial_direction(difference);
        if (state->search_step == 0 && difference < 0)
            state->minimum_adc_difference = -1023;
        if (state->search_step == 0 &&
            ((row.pga == 1 && direction < 0) || (row.pga == 0 && direction > 0)))
            state->search_step = 0x10;
        const auto wrapped_step = state->search_step >= 16
            ? state->search_step - 16 : state->search_step;
        if (dc_sign_matches(difference, state->minimum_adc_difference) &&
            wrapped_step < 15) {
            ++state->search_step;
            state->minimum_adc_difference = difference;
            state->stage = 1;
        } else {
            if (magnitude(difference) > magnitude(state->minimum_adc_difference)) {
                if (state->search_step == 0) {
                    state->failed = true;
                    return DcCalibrationStatus::failed;
                }
                --state->search_step;
            }
            write_trim();
            ++state->row;
            state->stage = state->row == row_count ? 6 : 0;
        }
        break;
    }
    case 6:
        append(plan, InitStepKind::write, 0x07,
            static_cast<std::uint16_t>(state->bb7 & ~0x0008));
        append(plan, InitStepKind::write, 0x1F, 7);
        plan->calibration_flags_to_clear = tuner_calibration_dc;
        plan->complete = true;
        state->complete = true;
        return DcCalibrationStatus::complete;
    default:
        state->failed = true;
        return DcCalibrationStatus::failed;
    }
    return DcCalibrationStatus::in_progress;
}

std::int16_t calculate_tuner_temperature(std::int16_t first,
                                         std::int16_t second)
{
    const auto difference = static_cast<std::int32_t>(second) - first;
    const auto product = difference * 180;
    // C++20 width and signed-shift behavior; physical ADC limits are unverified.
    const auto scaled = static_cast<std::int16_t>(product) >> 8;
    return static_cast<std::int16_t>(scaled + 55);
}

bool begin_temperature_calibration(std::uint16_t wbd_mux,
                                   std::uint16_t register_13,
                                   TemperatureCalibrationState *state,
                                   TunerCalibrationPlan *plan)
{
    if (state == nullptr || plan == nullptr) return false;
    *state = {register_13, wbd_mux, 0, true, false, false};
    *plan = {};
    append(plan, InitStepKind::write, 0x10,
        static_cast<std::uint16_t>((wbd_mux & ~0x07F8) | 0x0040));
    append(plan, InitStepKind::write, 0x13,
        static_cast<std::uint16_t>(register_13 | 0x0300));
    plan->wait_tenth_ms = 15;
    plan->measurement_register = 0x1D;
    plan->needs_measurement = true;
    return true;
}

bool continue_temperature_calibration(TemperatureCalibrationState *state,
                                     std::int16_t first_sample,
                                     TunerCalibrationPlan *plan)
{
    if (state == nullptr || plan == nullptr || !state->started ||
        state->first_sample_available || state->complete)
        return false;
    state->first_sample = first_sample;
    state->first_sample_available = true;
    *plan = {};
    append(plan, InitStepKind::write, 0x13,
        static_cast<std::uint16_t>((state->saved_register_13 & ~0x0300) | 0x0200));
    plan->wait_tenth_ms = 15;
    plan->measurement_register = 0x1D;
    plan->needs_measurement = true;
    return true;
}

bool complete_temperature_calibration(TemperatureCalibrationState *state,
                                      bool second_sample_available,
                                      std::int16_t second_sample,
                                      std::int16_t *temperature,
                                      TunerCalibrationPlan *plan)
{
    if (state == nullptr || temperature == nullptr || plan == nullptr ||
        !state->started || !state->first_sample_available || state->complete ||
        !second_sample_available)
        return false;
    *temperature = calculate_tuner_temperature(state->first_sample, second_sample);
    *plan = {};
    append(plan, InitStepKind::write, 0x13, state->saved_register_13);
    append(plan, InitStepKind::write, 0x10, state->wbd_mux);
    plan->calibration_flags_to_clear = tuner_calibration_temp;
    plan->complete = true;
    state->complete = true;
    return true;
}

std::uint16_t captrim_register_value(std::int16_t candidate)
{
    const auto stored_candidate = static_cast<std::int8_t>(candidate);
    return static_cast<std::uint16_t>(0xE900 |
        static_cast<std::uint16_t>(stored_candidate));
}

bool begin_captrim_calibration(bool retune, std::uint16_t current_register,
                               CapTrimCalibrationState *state)
{
    if (state == nullptr) return false;
    *state = {};
    state->candidate = retune
        ? static_cast<std::int16_t>(current_register & 0x007F) : 64;
    state->best_candidate = state->candidate;
    state->step = retune ? 4 : 64;
    state->minimum_difference = 3000;
    state->updates_remaining = retune ? 2 : 0;
    state->retune = retune;
    state->started = true;
    return true;
}

bool build_captrim_start_plan(TunerCalibrationPlan *plan)
{
    if (plan == nullptr) return false;
    *plan = {};
    append(plan, InitStepKind::write, 0x10, 0x02B1);
    append(plan, InitStepKind::write, 0x1E, 0x0032);
    plan->complete = true;
    return true;
}

bool build_p1g_captrim_calibration_plan(TunerCalibrationPlan *plan)
{
    if (plan == nullptr) return false;
    *plan = {};
    append(plan, InitStepKind::write, 0x40, 0x01FF);
    append(plan, InitStepKind::read, 0x40);
    plan->wait_tenth_ms = 50;
    return true;
}

bool continue_p1g_captrim_calibration(bool previous_io_succeeded,
                                      TunerCalibrationPlan *plan)
{
    if (plan == nullptr || !previous_io_succeeded) return false;
    *plan = {};
    append(plan, InitStepKind::write, 0x40, 0x018C);
    append(plan, InitStepKind::read, 0x40);
    append(plan, InitStepKind::read, 0x18, 0, 0x007F);
    plan->calibration_flags_to_clear = tuner_calibration_captrim;
    plan->complete = true;
    return true;
}

bool complete_captrim_measurement(CapTrimCalibrationState *state,
                                  bool measurement_available,
                                  std::uint16_t measurement,
                                  bool pll_fraction_zero,
                                  CapTrimCalibrationResult *result)
{
    if (state == nullptr || result == nullptr || !state->started || state->complete ||
        !measurement_available)
        return false;
    *result = {};
    const auto target = static_cast<std::uint16_t>(pll_fraction_zero ? 200 : 400);
    const auto difference = static_cast<std::uint16_t>(measurement >= target
        ? measurement - target : target - measurement);
    const auto direction = static_cast<std::int16_t>(measurement >= target ? -1 : 1);
    if (difference < state->minimum_difference) {
        state->minimum_difference = difference;
        state->best_candidate = state->candidate;
    }

    if (state->retune) {
        if (state->updates_remaining != 0) {
            state->step >>= 1;
            state->candidate = static_cast<std::int8_t>(state->candidate +
                direction * state->step);
            --state->updates_remaining;
            result->register_value = captrim_register_value(state->candidate);
            result->wait_tenth_ms = 25;
            result->needs_measurement = true;
            result->complete = false;
            return true;
        }
    } else if (state->step > 1) {
        state->step >>= 1;
        state->candidate = static_cast<std::int8_t>(state->candidate +
            direction * state->step);
        result->register_value = captrim_register_value(state->candidate);
        result->wait_tenth_ms = 25;
        result->needs_measurement = true;
        result->complete = false;
        return true;
    }

    state->complete = true;
    result->register_value = captrim_register_value(state->best_candidate);
    result->calibration_flags_to_clear = tuner_calibration_captrim;
    result->needs_measurement = false;
    result->complete = true;
    return true;
}

bool build_tuner_step1_plan(bool boost, TunerCalibrationPlan *plan)
{
    if (plan == nullptr) return false;
    *plan = {};
    plan->low_tracking_gain = 0x7C00;
    append(plan, InitStepKind::write, 0x01,
        static_cast<std::uint16_t>(0x8000 | (boost ? 0x2000 : 0)));
    append(plan, InitStepKind::write, 0x03, 0x6008);
    append(plan, InitStepKind::write, 0x04, 0x0001);
    append(plan, InitStepKind::write, 0x02, 0x2A16);
    append(plan, InitStepKind::write, 0x05, 0xABCD);
    plan->calibration_flags_to_set = tuner_calibration_wbd | tuner_calibration_temp;
    plan->tuner_state_stop = true;
    plan->complete = true;
    return true;
}

namespace {

struct SignalStrengthPoint {
    std::uint32_t input;
    std::uint32_t value;
};

constexpr SignalStrengthPoint kSignalStrengthPoints[]{
    {55953, 108500}, {55394, 108000}, {53834, 107000}, {52863, 106000},
    {52239, 105000}, {52012, 104000}, {51803, 103000}, {51566, 102000},
    {51356, 101000}, {51112, 100000}, {50869, 99000}, {50600, 98000},
    {50363, 97000}, {50117, 96000}, {49889, 95000}, {49680, 94000},
    {49493, 93000}, {49302, 92000}, {48929, 91000}, {48416, 90000},
    {48035, 89000}, {47593, 88000}, {47282, 87000}, {46953, 86000},
    {46698, 85000}, {45617, 84000}, {44773, 83000}, {43845, 82000},
    {43020, 81000}, {42010, 80000}, {0, 0}};

std::uint64_t decode_cnr_q16_16(std::uint16_t encoded)
{
    const auto mantissa = static_cast<std::uint64_t>((encoded >> 6) & 0x00FF);
    const auto exponent_bits = static_cast<std::int32_t>(encoded & 0x003F);
    const auto exponent = (exponent_bits & 0x0020) != 0
        ? exponent_bits - 0x0040 : exponent_bits;
    const auto shift = exponent + 16;
    return shift >= 0 ? mantissa << shift : mantissa >> -shift;
}

std::int64_t log2_q20(std::uint64_t value)
{
    std::uint32_t integer = 0;
    auto normalized = value;
    while (normalized >= 2) {
        normalized >>= 1;
        ++integer;
    }
    normalized = integer <= 31
        ? value << (31 - integer) : value >> (integer - 31);

    std::uint32_t fraction = 0;
    for (std::uint32_t bit = 0; bit < 20; ++bit) {
        normalized = (normalized * normalized) >> 31;
        if (normalized >= 0x100000000ULL) {
            normalized >>= 1;
            fraction |= 1U << (19 - bit);
        }
    }
    return (static_cast<std::int64_t>(integer) << 20) + fraction;
}

}

std::int32_t calculate_signal_strength_millidbm(std::uint16_t register_value)
{
    const auto input = 65535U - static_cast<std::uint32_t>(register_value);
    std::uint32_t value = 0;
    if (input >= kSignalStrengthPoints[0].input) {
        value = kSignalStrengthPoints[0].value;
    } else {
        for (std::size_t index = 1;
             index < sizeof(kSignalStrengthPoints) / sizeof(kSignalStrengthPoints[0]);
             ++index) {
            const auto &lower = kSignalStrengthPoints[index];
            const auto &upper = kSignalStrengthPoints[index - 1];
            if (input == lower.input) {
                value = lower.value;
                break;
            }
            if (input > lower.input) {
                const auto input_delta = static_cast<std::uint64_t>(input - lower.input);
                const auto value_delta = static_cast<std::uint64_t>(upper.value - lower.value);
                const auto input_span = static_cast<std::uint64_t>(upper.input - lower.input);
                value = lower.value + static_cast<std::uint32_t>(
                    input_delta * value_delta / input_span);
                break;
            }
        }
    }
    return static_cast<std::int32_t>(value) - 131000;
}

bool demod_fec_mpeg_locked(std::uint16_t register_568)
{
    return (register_568 & 0x1000) != 0 && (register_568 & 0x00E0) != 0;
}

bool calculate_demod_cnr(std::uint16_t noise_register,
                         std::uint16_t signal_register,
                         std::uint64_t *noise_q16_16,
                         std::uint64_t *signal_q16_16,
                         std::int32_t *hundredth_db)
{
    if (noise_q16_16 == nullptr || signal_q16_16 == nullptr || hundredth_db == nullptr)
        return false;
    *noise_q16_16 = decode_cnr_q16_16(noise_register);
    *signal_q16_16 = decode_cnr_q16_16(signal_register);
    if (*noise_q16_16 == 0 || *signal_q16_16 == 0) return false;

    // 内部Q20 log2。specに出力丸め・誤差上限の規定はない。
    constexpr std::int64_t kCentiDbPerLog2Q20 = 315652829;
    const auto log2_ratio_q20 = log2_q20(*signal_q16_16) - log2_q20(*noise_q16_16);
    const auto scaled = log2_ratio_q20 * kCentiDbPerLog2Q20;
    *hundredth_db = static_cast<std::int32_t>(scaled / (1LL << 40));
    return true;
}

LockReadResult observe_lock(bool read_succeeded, std::uint16_t register_568,
                            const std::uint16_t (&viterbi_registers)[3],
                            LockObservation *observation)
{
    if (!read_succeeded || observation == nullptr) return LockReadResult::read_error;
    observation->signal = (register_568 & 0x2000) != 0;
    observation->carrier = (register_568 & 0x0100) != 0;
    observation->tmcc_sync = (register_568 & 0x001E) == 0x001E;
    observation->coff_cpil = (register_568 & 0x1800) != 0;
    observation->lmod4 = (register_568 & 0x0400) != 0;
    observation->fec_mpeg = demod_fec_mpeg_locked(register_568);
    observation->layer_mpeg[0] = (register_568 & 0x0080) != 0;
    observation->layer_mpeg[1] = (register_568 & 0x0040) != 0;
    observation->layer_mpeg[2] = (register_568 & 0x0020) != 0;
    for (std::size_t index = 0; index < 3; ++index)
        observation->layer_viterbi[index] = (viterbi_registers[index] & 0x0001) != 0;
    return observation->fec_mpeg ? LockReadResult::locked : LockReadResult::unlocked;
}

DemodMpegLockStatus resolve_demod_mpeg_lock(bool wait_enabled, bool timed_out,
                                             std::uint8_t selected_layer,
                                             const LockObservation *observation)
{
    if (observation == nullptr || selected_layer >= 3)
        return DemodMpegLockStatus::unlocked;
    if (observation->layer_mpeg[selected_layer]) return DemodMpegLockStatus::data_lock;
    if (!wait_enabled) return DemodMpegLockStatus::unlocked;
    if (timed_out) {
        for (const auto locked : observation->layer_mpeg)
            if (locked) return DemodMpegLockStatus::data_lock;
        return DemodMpegLockStatus::unlocked;
    }
    return DemodMpegLockStatus::pending;
}

namespace {

bool append_demod_viterbi_start(DemodTransmissionMode mode, DemodInitPlan *plan)
{
    std::uint16_t register_32 = 0;
    std::uint16_t register_37 = 0;
    if (!calculate_demod_loop_values(mode, true, &register_32, &register_37))
        return false;
    append(plan, InitStepKind::masked_write, 771, 0, 0xFFFD);
    append(plan, InitStepKind::write, 32, register_32);
    append(plan, InitStepKind::write, 37, register_37);
    return true;
}

bool begin_demod_viterbi(DemodSyncState *state, DemodInitPlan *plan)
{
    if (!select_demod_mpeg_lock_layer(state->layers, &state->selected_layer) ||
        !append_demod_viterbi_start(state->mode, plan))
        return false;
    state->stage = DemodSyncStage::viterbi_start;
    return true;
}

void begin_demod_sync_failure(DemodSyncState *state, DemodInitPlan *plan)
{
    if (!append_demod_viterbi_start(state->mode, plan)) {
        state->stage = DemodSyncStage::failed;
        return;
    }
    state->stage = DemodSyncStage::failure_recovery;
}

}

bool begin_demod_sync(DemodTransmissionMode mode, std::uint8_t guard,
                      bool differential,
                      bool wait_mpeg,
                      const DemodLayerConfig (&layers)[3],
                      std::uint64_t now_ms, DemodSyncState *state,
                      DemodInitPlan *plan, std::uint32_t *poll_interval_ms)
{
    if (state == nullptr || plan == nullptr || poll_interval_ms == nullptr)
        return false;
    std::uint16_t register_32 = 0;
    std::uint16_t register_37 = 0;
    std::uint16_t sync_wait = 0;
    if (!calculate_demod_loop_values(mode, false, &register_32, &register_37) ||
        !calculate_demod_sync_wait(mode, guard, &sync_wait))
        return false;
    *state = {};
    state->stage = DemodSyncStage::await_coff_cpil;
    state->mode = mode;
    state->differential = differential;
    state->wait_mpeg = wait_mpeg;
    for (std::size_t index = 0; index < 3; ++index)
        state->layers[index] = layers[index];
    state->coff_started_ms = now_ms;
    *plan = {};
    append(plan, InitStepKind::write, 32, register_32);
    append(plan, InitStepKind::write, 37, register_37);
    append(plan, InitStepKind::masked_write, 273, sync_wait << 4, 0x000F);
    *poll_interval_ms = 10;
    return true;
}

DemodSyncStatus advance_demod_sync(DemodSyncState *state,
                                   bool read_succeeded,
                                   bool register_188_read_succeeded,
                                   bool previous_plan_succeeded,
                                   std::uint64_t now_ms,
                                   std::uint16_t register_188,
                                   const LockObservation *observation,
                                   DemodInitPlan *plan,
                                   std::uint32_t *poll_interval_ms)
{
    if (plan != nullptr) *plan = {};
    if (poll_interval_ms != nullptr) *poll_interval_ms = 0;
    if (state == nullptr || plan == nullptr || poll_interval_ms == nullptr)
        return DemodSyncStatus::failed;
    if (state->stage == DemodSyncStage::complete) return DemodSyncStatus::complete;
    if (state->stage == DemodSyncStage::failed) return DemodSyncStatus::failed;
    if (state->stage == DemodSyncStage::recovery_failed)
        return DemodSyncStatus::recovery_failed;
    if (state->stage == DemodSyncStage::failure_recovery) {
        state->stage = previous_plan_succeeded ? DemodSyncStage::failed
                                               : DemodSyncStage::recovery_failed;
        return previous_plan_succeeded ? DemodSyncStatus::failed
                                       : DemodSyncStatus::recovery_failed;
    }
    if (state->stage == DemodSyncStage::viterbi_start) {
        if (!previous_plan_succeeded) {
            state->stage = DemodSyncStage::failed;
            return DemodSyncStatus::failed;
        }
        state->mpeg_deadline_ms = now_ms +
            demod_mpeg_lock_timeout_ms(state->layers[0].interleave);
        state->stage = DemodSyncStage::await_mpeg;
        *poll_interval_ms = 10;
        return DemodSyncStatus::pending;
    }
    if (observation == nullptr) {
        state->stage = DemodSyncStage::failed;
        return DemodSyncStatus::failed;
    }
    if (!read_succeeded) {
        state->stage = DemodSyncStage::failed;
        return DemodSyncStatus::failed;
    }

    switch (state->stage) {
    case DemodSyncStage::await_coff_cpil:
        if (now_ms < state->coff_started_ms + 10U) {
            *poll_interval_ms = 10;
            return DemodSyncStatus::pending;
        }
        if (observation->coff_cpil) {
            if (!state->differential && !observation->lmod4) {
                if (!register_188_read_succeeded) {
                    state->stage = DemodSyncStage::failed;
                    return DemodSyncStatus::failed;
                }
                const auto window_symbols = 20U * ((register_188 >> 5) & 0x001FU);
                state->lmod4_deadline_ms = now_ms + (window_symbols + 9U) / 10U;
                state->stage = DemodSyncStage::await_lmod4;
                *poll_interval_ms = 10;
                return DemodSyncStatus::pending;
            }
            if (!begin_demod_viterbi(state, plan)) {
                state->stage = DemodSyncStage::failed;
                return DemodSyncStatus::failed;
            }
            return DemodSyncStatus::pending;
        }
        if (now_ms > state->coff_started_ms + 4U) {
            begin_demod_sync_failure(state, plan);
            return state->stage == DemodSyncStage::failure_recovery
                ? DemodSyncStatus::pending : DemodSyncStatus::failed;
        }
        *poll_interval_ms = 10;
        return DemodSyncStatus::pending;
    case DemodSyncStage::await_lmod4:
        if (observation->lmod4) {
            if (!begin_demod_viterbi(state, plan)) {
                state->stage = DemodSyncStage::failed;
                return DemodSyncStatus::failed;
            }
            return DemodSyncStatus::pending;
        }
        if (now_ms > state->lmod4_deadline_ms) {
            begin_demod_sync_failure(state, plan);
            return state->stage == DemodSyncStage::failure_recovery
                ? DemodSyncStatus::pending : DemodSyncStatus::failed;
        }
        *poll_interval_ms = 10;
        return DemodSyncStatus::pending;
    case DemodSyncStage::await_mpeg: {
        const auto lock_status = resolve_demod_mpeg_lock(
            state->wait_mpeg, now_ms >= state->mpeg_deadline_ms,
            state->selected_layer, observation);
        if (lock_status == DemodMpegLockStatus::data_lock) {
            state->stage = DemodSyncStage::complete;
            return DemodSyncStatus::complete;
        }
        if (lock_status == DemodMpegLockStatus::unlocked) {
            state->stage = DemodSyncStage::failed;
            return DemodSyncStatus::failed;
        }
        *poll_interval_ms = 10;
        return DemodSyncStatus::pending;
    }
    case DemodSyncStage::viterbi_start:
        return DemodSyncStatus::pending;
    case DemodSyncStage::failure_recovery:
        return previous_plan_succeeded ? DemodSyncStatus::failed
                                       : DemodSyncStatus::recovery_failed;
    case DemodSyncStage::complete: return DemodSyncStatus::complete;
    case DemodSyncStage::failed: return DemodSyncStatus::failed;
    case DemodSyncStage::recovery_failed: return DemodSyncStatus::recovery_failed;
    }
    state->stage = DemodSyncStage::failed;
    return DemodSyncStatus::failed;
}

bool build_stream_engine_plan(StreamPlan *plan)
{
    if (plan == nullptr) return false;
    plan->step_count = 0;
    constexpr AddressValue32 writes[]{
        {0x00D000C4, 0}, {0x00D00000, 0xFFFFFFFF},
        {0x00D00004, 0x001F33FF}, {0x00D00000, 0x30030FFF},
        {0x00D00000, 0x00030000}, {0x00D00004, 0x00003305},
        {0x00D01040, 0}, {0x00D01064, 0}, {0x00D010A0, 0},
        {0x00D010A4, 0}, {0x00D010A8, 0}, {0x00D01020, 0x02000000},
        {0x00D01004, 0}, {0x00D01044, 0x00BC0000},
        {0x00D01048, 0x47000000}, {0x00D0104C, 0x00FFFFFF},
        {0x00D01050, 0}, {0x00D01060, 0}, {0x00D01000, 0x06070000},
        {0x00D01080, 0x01000000}, {0x00D01088, 0},
        {0x00D0108C, 0x000000FF}, {0x00D01084, 0},
        {0x00D01090, 0x01000000}, {0x00D02028, 0},
        {0x00D0202C, 0}, {0x00D02030, 0}, {0x00D02038, 0},
        {0x00D02000, 0x03020300}, {0x00D02020, 0x00AE4000},
        {0x00D02024, 0x00600000}, {0x00D00004, 0x00043305}};
    for (const auto &[reg, value] : writes)
        if (!append(plan, StreamStepKind::write24, reg, value)) return false;
    if (!append(plan, StreamStepKind::masked_write24, 0x00FD8010,
                0x000F0000, 0xFFF0FFFF) ||
        !append(plan, StreamStepKind::masked_write24, 0x00FD8014,
                0, 0xFFF0FFFF))
        return false;
    constexpr AddressValue32 commands[]{
        {0x00FD8200, 1}, {0x00FD8220, 1}, {0x00FD8200, 0},
        {0x00FD8204, 0xB09F0000}, {0x00FD8200, 2},
        {0x00FD8220, 0}, {0x00FD8224, 0xB09F2000},
        {0x00FD8220, 2}, {0x00FDBD08, 0x00FF6001},
        {0x00FDBD08, 0x00FF6000}, {0x00FDBC14, 0x00060030}};
    for (std::size_t index = 0; index < sizeof(commands) / sizeof(commands[0]); ++index) {
        if (index == 9 &&
            !append(plan, StreamStepKind::poll_mask_clear24, 0x00FDBD08,
                    0, 1, 100, 1))
            return false;
        const auto [reg, value] = commands[index];
        const bool normal = index == 3 || index == 6 || index >= 9;
        if (!append(plan, normal ? StreamStepKind::write24
                                 : StreamStepKind::trigger_write24,
                    reg, value))
            return false;
    }
    return true;
}

bool build_stream_core0_run_plan(StreamPlan *plan)
{
    if (plan == nullptr) return false;
    plan->step_count = 0;
    return append(plan, StreamStepKind::trigger_wide_write32,
                  0x01600000, 0xFFFCFFFF);
}

bool build_stream_input_start_plan(StreamPlan *plan)
{
    if (plan == nullptr) return false;
    plan->step_count = 0;
    constexpr AddressValue32 writes[]{
        {0x00D01020, 0}, {0x00D01040, 0x03000000},
        {0x00D010A4, 0x13170400}, {0x00D00484, 0x01000000},
        {0x00D00488, 0x206C0000}};
    for (const auto &[reg, value] : writes)
        if (!append(plan, StreamStepKind::write24, reg, value)) return false;
    return append(plan, StreamStepKind::trigger_write24,
                  0x00D02010, 0x02000000);
}

bool build_stream_input_close_plan(StreamPlan *plan)
{
    if (plan == nullptr) return false;
    plan->step_count = 0;
    return append(plan, StreamStepKind::wide_write32,
                  0x01600000, 0xFFFFFFFF) &&
        append(plan, StreamStepKind::write24, 0x00D01040, 0) &&
        append(plan, StreamStepKind::write24, 0x00D01020, 0x02000000) &&
        append(plan, StreamStepKind::trigger_write24,
               0x00D02010, 0x04000000);
}

}
