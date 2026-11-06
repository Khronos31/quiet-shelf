// SPDX-License-Identifier: GPL-2.0-or-later
#include <dtv/internal/firmware.hpp>

#include <array>
#include <fstream>
#include <limits>
#include <new>
#include <system_error>

namespace dtv::internal {

namespace {

constexpr std::array<uint32_t, 64> kSha256RoundConstants{
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

constexpr uint32_t rotate_right(uint32_t value, unsigned int bits)
{
    return (value >> bits) | (value << (32 - bits));
}

class Sha256 {
public:
    void update(std::span<const uint8_t> bytes)
    {
        total_bytes_ += bytes.size();
        for (const auto byte : bytes) {
            block_[block_size_++] = byte;
            if (block_size_ == block_.size()) {
                transform();
                block_size_ = 0;
            }
        }
    }

    std::array<uint8_t, 32> finish()
    {
        const auto bit_count = static_cast<uint64_t>(total_bytes_) * 8;
        block_[block_size_++] = 0x80;
        if (block_size_ > 56) {
            while (block_size_ < block_.size()) {
                block_[block_size_++] = 0;
            }
            transform();
            block_size_ = 0;
        }
        while (block_size_ < 56) {
            block_[block_size_++] = 0;
        }
        for (int shift = 56; shift >= 0; shift -= 8) {
            block_[block_size_++] = static_cast<uint8_t>(bit_count >> shift);
        }
        transform();

        std::array<uint8_t, 32> digest{};
        for (std::size_t i = 0; i < state_.size(); ++i) {
            for (std::size_t byte = 0; byte < 4; ++byte) {
                digest[i * 4 + byte] = static_cast<uint8_t>(state_[i] >> (24 - byte * 8));
            }
        }
        return digest;
    }

private:
    void transform()
    {
        std::array<uint32_t, 64> words{};
        for (std::size_t i = 0; i < 16; ++i) {
            const auto offset = i * 4;
            words[i] = (static_cast<uint32_t>(block_[offset]) << 24) |
                       (static_cast<uint32_t>(block_[offset + 1]) << 16) |
                       (static_cast<uint32_t>(block_[offset + 2]) << 8) |
                       static_cast<uint32_t>(block_[offset + 3]);
        }
        for (std::size_t i = 16; i < words.size(); ++i) {
            const auto s0 = rotate_right(words[i - 15], 7) ^ rotate_right(words[i - 15], 18) ^
                            (words[i - 15] >> 3);
            const auto s1 = rotate_right(words[i - 2], 17) ^ rotate_right(words[i - 2], 19) ^
                            (words[i - 2] >> 10);
            words[i] = words[i - 16] + s0 + words[i - 7] + s1;
        }

        auto a = state_[0];
        auto b = state_[1];
        auto c = state_[2];
        auto d = state_[3];
        auto e = state_[4];
        auto f = state_[5];
        auto g = state_[6];
        auto h = state_[7];
        for (std::size_t i = 0; i < words.size(); ++i) {
            const auto sum1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^ rotate_right(e, 25);
            const auto choice = (e & f) ^ (~e & g);
            const auto temp1 = h + sum1 + choice + kSha256RoundConstants[i] + words[i];
            const auto sum0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^ rotate_right(a, 22);
            const auto majority = (a & b) ^ (a & c) ^ (b & c);
            const auto temp2 = sum0 + majority;
            h = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }
        state_[0] += a;
        state_[1] += b;
        state_[2] += c;
        state_[3] += d;
        state_[4] += e;
        state_[5] += f;
        state_[6] += g;
        state_[7] += h;
    }

    std::array<uint32_t, 8> state_{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::array<uint8_t, 64> block_{};
    std::size_t block_size_ = 0;
    std::size_t total_bytes_ = 0;
};

int hex_value(char value)
{
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    return -1;
}

bool parse_sha256(std::string_view hex, std::array<uint8_t, 32> *out)
{
    if (hex.size() != 64 || !out) {
        return false;
    }
    for (std::size_t i = 0; i < out->size(); ++i) {
        const auto high = hex_value(hex[i * 2]);
        const auto low = hex_value(hex[i * 2 + 1]);
        if (high < 0 || low < 0) {
            return false;
        }
        (*out)[i] = static_cast<uint8_t>((high << 4) | low);
    }
    return true;
}

FirmwarePreflightResult fail(FirmwarePreflightError error, std::size_t index)
{
    return {error, index};
}

}

std::span<const uint8_t> VerifiedFirmwareBundle::image(std::size_t index) const
{
    if (index >= images_.size()) {
        return {};
    }
    return images_[index];
}

std::span<const uint8_t> VerifiedFirmwarePackage::image(std::size_t index) const
{
    if (index >= images_.size()) {
        return {};
    }
    return images_[index];
}

FirmwarePreflightResult load_firmware_bundle(
    std::span<const FirmwareImageSpec> images, VerifiedFirmwareBundle *out_bundle)
{
    if (out_bundle) {
        out_bundle->images_.clear();
    }
    if (!out_bundle || images.empty()) {
        return fail(FirmwarePreflightError::InvalidArgument, 0);
    }

    VerifiedFirmwareBundle bundle;
    try {
        bundle.images_.reserve(images.size());
        for (std::size_t index = 0; index < images.size(); ++index) {
            const auto &spec = images[index];
            std::array<uint8_t, 32> expected_hash{};
            if (spec.path.empty()) {
                return fail(FirmwarePreflightError::InvalidArgument, index);
            }
            if (!parse_sha256(spec.expected_sha256, &expected_hash)) {
                return fail(FirmwarePreflightError::InvalidHash, index);
            }
            if (spec.expected_size > std::numeric_limits<std::size_t>::max() ||
                spec.expected_size > static_cast<uint64_t>(std::numeric_limits<std::streamsize>::max())) {
                return fail(FirmwarePreflightError::ResourceExhausted, index);
            }
            if (spec.expected_size > std::vector<uint8_t>{}.max_size()) {
                return fail(FirmwarePreflightError::ResourceExhausted, index);
            }

            std::error_code error;
            const auto file_size = std::filesystem::file_size(spec.path, error);
            if (error) {
                return fail(FirmwarePreflightError::ReadFailed, index);
            }
            if (file_size != spec.expected_size) {
                return fail(FirmwarePreflightError::SizeMismatch, index);
            }

            std::vector<uint8_t> data(static_cast<std::size_t>(spec.expected_size));
            std::ifstream file(spec.path, std::ios::binary);
            if (!file) {
                return fail(FirmwarePreflightError::ReadFailed, index);
            }
            if (!data.empty()) {
                file.read(reinterpret_cast<char *>(data.data()),
                          static_cast<std::streamsize>(data.size()));
                if (file.gcount() != static_cast<std::streamsize>(data.size())) {
                    return fail(FirmwarePreflightError::ReadFailed, index);
                }
            }
            char extra_byte = 0;
            if (file.get(extra_byte)) {
                return fail(FirmwarePreflightError::SizeMismatch, index);
            }
            if (!file.eof()) {
                return fail(FirmwarePreflightError::ReadFailed, index);
            }

            Sha256 sha256;
            sha256.update(data);
            if (sha256.finish() != expected_hash) {
                return fail(FirmwarePreflightError::HashMismatch, index);
            }
            bundle.images_.push_back(std::move(data));
        }
    } catch (const std::bad_alloc &) {
        return fail(FirmwarePreflightError::ResourceExhausted, bundle.images_.size());
    }

    out_bundle->images_ = std::move(bundle.images_);
    return {};
}

FirmwarePreflightResult load_firmware_package(
    std::span<const uint8_t> package, std::span<const FirmwarePackageImageSpec> images,
    VerifiedFirmwarePackage *out_package)
{
    if (out_package) out_package->images_.clear();
    if (!out_package || images.empty()) return fail(FirmwarePreflightError::InvalidArgument, 0);
    VerifiedFirmwarePackage verified;
    try {
        verified.images_.reserve(images.size());
        for (std::size_t index = 0; index < images.size(); ++index) {
            const auto &spec = images[index];
            std::array<uint8_t, 32> expected_hash{};
            if (spec.size == 0) return fail(FirmwarePreflightError::InvalidArgument, index);
            if (!parse_sha256(spec.expected_sha256, &expected_hash)) {
                return fail(FirmwarePreflightError::InvalidHash, index);
            }
            if (spec.offset > package.size() || spec.size > package.size() - spec.offset ||
                spec.size > std::numeric_limits<std::size_t>::max()) {
                return fail(FirmwarePreflightError::SizeMismatch, index);
            }
            const auto first = package.begin() + static_cast<std::size_t>(spec.offset);
            const auto last = first + static_cast<std::size_t>(spec.size);
            Sha256 sha256;
            sha256.update(std::span<const uint8_t>(first, last));
            if (sha256.finish() != expected_hash) {
                return fail(FirmwarePreflightError::HashMismatch, index);
            }
            verified.images_.emplace_back(first, last);
        }
    } catch (const std::bad_alloc &) {
        return fail(FirmwarePreflightError::ResourceExhausted, verified.images_.size());
    }
    out_package->images_ = std::move(verified.images_);
    return {};
}

}
