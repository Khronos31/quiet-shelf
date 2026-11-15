// SPDX-License-Identifier: GPL-2.0-or-later
#include <dtv/internal/firmware.hpp>

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

int main()
{
    using namespace dtv::internal;

    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() /
                      ("dtv11-fw-preflight-test-" + std::to_string(suffix));
    std::filesystem::create_directories(root);
    const auto first_path = root / "first.bin";
    const auto second_path = root / "second.bin";
    {
        std::ofstream first(first_path, std::ios::binary);
        first << "abc";
        std::ofstream second(second_path, std::ios::binary);
        second << "abd";
    }

    constexpr auto kAbcHash = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    constexpr auto kWrongHash = "a52d159f262b2c6ddb724a61840befc36eb30c88877a4030b65cbe86298449c9";
    VerifiedFirmwareBundle bundle;
    const std::array valid_images{
        FirmwareImageSpec{first_path, 3, kAbcHash},
        FirmwareImageSpec{second_path, 3, kWrongHash},
    };
    if (!load_firmware_bundle(valid_images, &bundle) || bundle.size() != 2 ||
        bundle.image(0).size() != 3 || bundle.image(0)[0] != 'a' ||
        bundle.image(0)[2] != 'c' || !bundle.image(2).empty()) {
        return 1;
    }

    const std::array corrupted_images{
        FirmwareImageSpec{first_path, 3, kAbcHash},
        FirmwareImageSpec{second_path, 3, kAbcHash},
    };
    const auto rejected = load_firmware_bundle(corrupted_images, &bundle);
    if (rejected.error != FirmwarePreflightError::HashMismatch || rejected.image_index != 1 ||
        bundle.size() != 0) {
        return 2;
    }

    const std::array wrong_length{FirmwareImageSpec{first_path, 4, kAbcHash}};
    if (load_firmware_bundle(wrong_length, &bundle).error != FirmwarePreflightError::SizeMismatch ||
        bundle.size() != 0) {
        return 3;
    }

    std::filesystem::remove_all(root);
    return 0;
}
