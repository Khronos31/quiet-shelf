/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DTV_SPI_FEATURE_HPP
#define DTV_SPI_FEATURE_HPP

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include <dtv/dtv.h>

namespace dtv::spi {

enum class Feature : std::uint32_t {
    LnbPower,
    Card,
    OutputPidFilter,
    TuneIsdbT,
    TuneIsdbS,
    SelectSlot,
    SelectTsid,
    Signal,
    Lna,
    Firmware,
};

enum class PublicModelOperation : std::uint32_t {
    LnbOn,
    LnbOff,
    TuneByFrequency,
    GetSignal,
    SetPidFilter,
    SendFirmware,
    SendDefaultFirmware,
};

template <Feature... Features>
struct feature_set {
    static constexpr std::uint32_t mask =
        ((std::uint32_t{1} << static_cast<std::uint32_t>(Features)) | ... | 0u);
    static constexpr bool contains(Feature feature)
    {
        return ((Features == feature) || ...);
    }
};

template <typename Set>
struct is_feature_set : std::false_type {};

template <Feature... Features>
struct is_feature_set<feature_set<Features...>> : std::true_type {};

template <typename Subset, typename Superset>
struct is_subset;

template <Feature... Subset, Feature... Superset>
struct is_subset<feature_set<Subset...>, feature_set<Superset...>>
    : std::bool_constant<(feature_set<Superset...>::contains(Subset) && ...)> {};

template <typename Set, typename Required>
struct has_any_feature;

template <Feature... Features, Feature... Required>
struct has_any_feature<feature_set<Features...>, feature_set<Required...>>
    : std::bool_constant<(feature_set<Features...>::contains(Required) || ...)> {};

struct model_information {
    const char *name;
    std::uint16_t vendor_id;
    std::uint16_t product_id;
};

struct model_operation_table {
    int32_t (*lnb_on)(dtv_enclosure *, const dtv_call_options *, dtv_error *);
    int32_t (*lnb_off)(dtv_enclosure *, const dtv_call_options *, dtv_error *);
    int32_t (*tune_by_frequency)(dtv_receiver *, uint32_t, uint64_t,
                                 const dtv_tune_options *, const dtv_call_options *, dtv_error *);
    int32_t (*get_signal)(dtv_receiver *, const dtv_call_options *, dtv_signal *, dtv_error *);
    int32_t (*set_pid_filter)(dtv_stream *, const uint16_t *, uint32_t, dtv_error *);
    int32_t (*send_firmware)(dtv_device *, const uint8_t *, uint64_t,
                             const dtv_call_options *, dtv_error *);
    int32_t (*send_default_firmware)(dtv_device *, const dtv_call_options *, dtv_error *);
};

struct l3_model_record {
    const dtv_model *model;
    std::uint32_t features;
    const model_operation_table *operations;
};

template <typename Firmware>
struct is_firmware_declaration : std::false_type {};

struct no_firmware {};
template <typename Asset>
struct firmware_asset {
    using asset_type = Asset;
};

template <>
struct is_firmware_declaration<no_firmware> : std::true_type {};
template <typename Asset>
struct is_firmware_declaration<firmware_asset<Asset>> : std::true_type {};

template <typename T>
inline constexpr bool is_feature_set_v = is_feature_set<T>::value;

template <typename Subset, typename Superset>
inline constexpr bool is_subset_v = is_subset<Subset, Superset>::value;

template <typename Set, typename Required>
inline constexpr bool has_any_feature_v = has_any_feature<Set, Required>::value;

template <typename T>
inline constexpr bool is_firmware_declaration_v = is_firmware_declaration<T>::value;

template <typename ModelFeatures, typename RequiredFeatures, typename Function>
int32_t call_if_supported(Function function, dtv_error *error)
{
    if constexpr (is_subset_v<RequiredFeatures, ModelFeatures>) {
        return function();
    }
    if (error != nullptr && error->struct_size >= offsetof(dtv_error, code) + sizeof(error->code)) {
        error->code = DTV_ERROR_UNSUPPORTED;
    }
    return DTV_ERROR_UNSUPPORTED;
}

template <typename Firmware, typename Function>
int32_t call_firmware_operation(Function function, dtv_error *)
{
    if constexpr (std::is_same_v<Firmware, no_firmware>) {
        return DTV_ERROR_OK;
    } else {
        return function();
    }
}

template <typename Firmware, typename Function>
int32_t call_default_firmware(dtv_device *device, const dtv_call_options *options,
                              dtv_error *error, Function function)
{
    if constexpr (std::is_same_v<Firmware, no_firmware>) {
        return DTV_ERROR_OK;
    } else {
        return function(device, Firmware::asset_type::data, Firmware::asset_type::size, options, error);
    }
}

template <PublicModelOperation Operation, typename ModelFeatures, typename Firmware,
          Feature Required, typename Function>
int32_t call_model_operation(Function function, dtv_error *error)
{
    if constexpr (Operation == PublicModelOperation::SendFirmware) {
        return call_firmware_operation<Firmware>(function, error);
    }
    if constexpr (Operation == PublicModelOperation::TuneByFrequency) {
        using tune_features = feature_set<Feature::TuneIsdbT, Feature::TuneIsdbS>;
        if constexpr (has_any_feature_v<ModelFeatures, tune_features>) {
            return function();
        }
        if (error != nullptr && error->struct_size >= offsetof(dtv_error, code) + sizeof(error->code)) {
            error->code = DTV_ERROR_UNSUPPORTED;
        }
        return DTV_ERROR_UNSUPPORTED;
    }
    return call_if_supported<ModelFeatures, feature_set<Required>>(function, error);
}

}

#define DTV_FEATURES(...) ::dtv::spi::feature_set<__VA_ARGS__>
#define DTV_NO_FIRMWARE ::dtv::spi::no_firmware
#define DTV_FIRMWARE(asset) ::dtv::spi::firmware_asset<::dtv::spi::firmware_assets::asset>
#define DTV_PRODUCT(name, features) \
    using product_features_##name = features; \
    static_assert(::dtv::spi::is_feature_set_v<product_features_##name>)
#define DTV_MODEL(product, model, features, firmware, info) \
    using model_features_##product##_##model = features; \
    using model_firmware_##product##_##model = firmware; \
    static_assert(::dtv::spi::is_subset_v<model_features_##product##_##model, \
                                           product_features_##product>, \
                  "model features must be a subset of product features"); \
    static_assert(::dtv::spi::is_firmware_declaration_v<firmware>, \
                  "each model must declare firmware or no firmware"); \
    inline constexpr auto model_info_##product##_##model = info; \
    inline constexpr dtv_model model_public_##product##_##model{ \
        #product, model_info_##product##_##model.name, \
        model_info_##product##_##model.vendor_id, model_info_##product##_##model.product_id }

#define DTV_PUBLIC_MODEL_OPS(X, product, model) \
    X(product, model, lnb_on, LnbOn, ::dtv::spi::Feature::LnbPower, \
      (dtv_enclosure *enclosure, const dtv_call_options *options, dtv_error *err), \
      (enclosure, options, err)) \
    X(product, model, lnb_off, LnbOff, ::dtv::spi::Feature::LnbPower, \
      (dtv_enclosure *enclosure, const dtv_call_options *options, dtv_error *err), \
      (enclosure, options, err)) \
    X(product, model, tune_by_frequency, TuneByFrequency, ::dtv::spi::Feature::TuneIsdbT, \
      (dtv_receiver *receiver, uint32_t system, uint64_t frequency_hz, const dtv_tune_options *tune, \
       const dtv_call_options *options, dtv_error *err), \
      (receiver, system, frequency_hz, tune, options, err)) \
    X(product, model, get_signal, GetSignal, ::dtv::spi::Feature::Signal, \
      (dtv_receiver *receiver, const dtv_call_options *options, dtv_signal *out, dtv_error *err), \
      (receiver, options, out, err)) \
    X(product, model, set_pid_filter, SetPidFilter, ::dtv::spi::Feature::OutputPidFilter, \
      (dtv_stream *stream, const uint16_t *pids, uint32_t count, dtv_error *err), \
      (stream, pids, count, err)) \
    X(product, model, send_firmware, SendFirmware, ::dtv::spi::Feature::Firmware, \
      (dtv_device *device, const uint8_t *firmware, uint64_t size, const dtv_call_options *options, \
       dtv_error *err), \
      (device, firmware, size, options, err)) \
    X(product, model, send_default_firmware, SendDefaultFirmware, ::dtv::spi::Feature::Firmware, \
      (dtv_device *device, const dtv_call_options *options, dtv_error *err), \
      (device, options, err))

#define DTV_DETAIL_DEFINE_MODEL_OP(product, model, name, operation, feature, signature, arguments) \
    DTV_DETAIL_DEFINE_MODEL_OP_##operation(product, model, name, operation, feature, signature, arguments)
#define DTV_DETAIL_DEFINE_MODEL_OP_LnbOn(product, model, name, operation, feature, signature, arguments) \
    DTV_DETAIL_DEFINE_MODEL_OP_GENERIC(product, model, name, operation, feature, signature, arguments)
#define DTV_DETAIL_DEFINE_MODEL_OP_LnbOff(product, model, name, operation, feature, signature, arguments) \
    DTV_DETAIL_DEFINE_MODEL_OP_GENERIC(product, model, name, operation, feature, signature, arguments)
#define DTV_DETAIL_DEFINE_MODEL_OP_TuneByFrequency(product, model, name, operation, feature, signature, arguments) \
    DTV_DETAIL_DEFINE_MODEL_OP_GENERIC(product, model, name, operation, feature, signature, arguments)
#define DTV_DETAIL_DEFINE_MODEL_OP_GetSignal(product, model, name, operation, feature, signature, arguments) \
    DTV_DETAIL_DEFINE_MODEL_OP_GENERIC(product, model, name, operation, feature, signature, arguments)
#define DTV_DETAIL_DEFINE_MODEL_OP_SetPidFilter(product, model, name, operation, feature, signature, arguments) \
    DTV_DETAIL_DEFINE_MODEL_OP_GENERIC(product, model, name, operation, feature, signature, arguments)
#define DTV_DETAIL_DEFINE_MODEL_OP_GENERIC(product, model, name, operation, feature, signature, arguments) \
    extern "C" int32_t dtv_model_impl_##name##_##product##_##model signature; \
    extern "C" int32_t dtv_##name##_##product##_##model signature \
    { \
        using model_features = model_features_##product##_##model; \
        return ::dtv::spi::call_model_operation< \
            ::dtv::spi::PublicModelOperation::operation, model_features, \
            model_firmware_##product##_##model, feature>( \
            [&] { return dtv_model_impl_##name##_##product##_##model arguments; }, err); \
    }
#define DTV_DETAIL_DEFINE_MODEL_OP_SendFirmware(product, model, name, operation, feature, signature, arguments) \
    extern "C" int32_t dtv_model_impl_send_firmware_##product##_##model signature; \
    extern "C" int32_t dtv_send_firmware_##product##_##model signature \
    { \
        using firmware_declaration = model_firmware_##product##_##model; \
        return ::dtv::spi::call_firmware_operation<firmware_declaration>( \
            [&] { return dtv_model_impl_send_firmware_##product##_##model arguments; }, err); \
    }
#define DTV_DETAIL_DEFINE_MODEL_OP_SendDefaultFirmware(product, model, name, operation, feature, signature, arguments) \
    extern "C" int32_t dtv_send_firmware_##product##_##model \
        (dtv_device *, const uint8_t *, uint64_t, const dtv_call_options *, dtv_error *); \
    extern "C" int32_t dtv_send_default_firmware_##product##_##model signature \
    { \
        using firmware_declaration = model_firmware_##product##_##model; \
        return ::dtv::spi::call_default_firmware<model_firmware_##product##_##model>(device, options, err, \
            [](dtv_device *target, const uint8_t *data, uint64_t size, \
               const dtv_call_options *call_options, dtv_error *failure) { \
                return dtv_send_firmware_##product##_##model( \
                    target, data, size, call_options, failure); \
            }); \
    }
#define DTV_DETAIL_MODEL_OPERATION_POINTER(product, model, name, operation, feature, signature, arguments) \
    &dtv_##name##_##product##_##model,
#define DTV_DEFINE_MODEL(product, model) \
    DTV_PUBLIC_MODEL_OPS(DTV_DETAIL_DEFINE_MODEL_OP, product, model) \
    extern const ::dtv::spi::model_operation_table dtv_l3_operations_##product##_##model{ \
        DTV_PUBLIC_MODEL_OPS(DTV_DETAIL_MODEL_OPERATION_POINTER, product, model) \
    }; \
    extern const ::dtv::spi::l3_model_record dtv_l3_model_##product##_##model{ \
        &model_public_##product##_##model, model_features_##product##_##model::mask, \
        &dtv_l3_operations_##product##_##model }

#endif
