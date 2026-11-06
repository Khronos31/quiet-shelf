/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DTV_INTERNAL_FIRMWARE_HPP
#define DTV_INTERNAL_FIRMWARE_HPP

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string_view>
#include <vector>

namespace dtv::internal {

struct FirmwareImageSpec {
    std::filesystem::path path;
    uint64_t expected_size;
    std::string_view expected_sha256;
};

struct FirmwarePackageImageSpec {
    std::uint64_t offset;
    std::uint64_t size;
    std::string_view expected_sha256;
};

enum class FirmwarePreflightError {
    None,
    InvalidArgument,
    ReadFailed,
    SizeMismatch,
    InvalidHash,
    HashMismatch,
    ResourceExhausted,
};

struct FirmwarePreflightResult {
    FirmwarePreflightError error = FirmwarePreflightError::None;
    std::size_t image_index = 0;

    explicit operator bool() const { return error == FirmwarePreflightError::None; }
};

class VerifiedFirmwareBundle {
public:
    std::size_t size() const { return images_.size(); }
    std::span<const uint8_t> image(std::size_t index) const;

private:
    friend FirmwarePreflightResult load_firmware_bundle(
        std::span<const FirmwareImageSpec>, VerifiedFirmwareBundle *);

    std::vector<std::vector<uint8_t>> images_;
};

class VerifiedFirmwarePackage {
public:
    std::size_t size() const { return images_.size(); }
    std::span<const uint8_t> image(std::size_t index) const;

private:
    friend FirmwarePreflightResult load_firmware_package(
        std::span<const uint8_t>, std::span<const FirmwarePackageImageSpec>,
        VerifiedFirmwarePackage *);
    std::vector<std::vector<uint8_t>> images_;
};

// image() のviewはbundleが所有し、bundleの破棄またはmove後に無効となる。
FirmwarePreflightResult load_firmware_bundle(
    std::span<const FirmwareImageSpec> images, VerifiedFirmwareBundle *out_bundle);
FirmwarePreflightResult load_firmware_package(
    std::span<const uint8_t> package, std::span<const FirmwarePackageImageSpec> images,
    VerifiedFirmwarePackage *out_package);

}

#endif
