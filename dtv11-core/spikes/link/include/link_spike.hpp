// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace dtv::link_spike {

enum class Result {
    ok,
    unsupported,
    error,
};

enum class Feature {
    send_firmware,
    get_signal,
};

template<Feature... features>
struct feature_set {
    template<Feature feature>
    static constexpr bool contains = ((features == feature) || ...);
};

template<class ModelFeatures, class ProductFeatures>
inline constexpr bool model_features_fit_product =
    (!ModelFeatures::template contains<Feature::send_firmware> ||
     ProductFeatures::template contains<Feature::send_firmware>) &&
    (!ModelFeatures::template contains<Feature::get_signal> ||
     ProductFeatures::template contains<Feature::get_signal>);

struct FirmwareSample {
    const std::uint8_t *data;
    std::size_t size;
};

using SendFirmware = Result (*)(const std::uint8_t *, std::size_t);
using GetSignal = Result (*)();
using SendDefaultFirmware = Result (*)();

struct ModelEntry {
    const char *model;
    const char *firmware;
    SendFirmware send_firmware;
    GetSignal get_signal;
    SendDefaultFirmware send_default_firmware;
};

struct ProductEntry {
    const char *product;
    const char *command;
    const char *version;
    const ModelEntry *model;
};

struct CliConfig {
    const char *product;
    const char *command;
    const char *version;
};

Result unsupported();
void register_product(const ProductEntry *product);
int cli_main(int argc, char **argv, const CliConfig *config);

}

#define DTV_SPIKE_PRODUCT(product, ...) \
    using product##_product_features = ::dtv::link_spike::feature_set<__VA_ARGS__>

#define DTV_SPIKE_FIRMWARE(name) ::dtv::link_spike::FirmwareSample

#define DTV_SPIKE_MODEL(product, model, features, firmware) \
    using model##_model_features = ::dtv::link_spike::feature_set<features>; \
    using model##_model_firmware = firmware; \
    static_assert(std::is_same_v<model##_model_firmware, ::dtv::link_spike::FirmwareSample>); \
    static_assert(::dtv::link_spike::model_features_fit_product< \
                  model##_model_features, product##_product_features>)
