/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <dtv/dtv.h>
#include <dtv/spi/feature.hpp>
#include <dtv/spi/device_ops.hpp>

DTV_PRODUCT(fixture, DTV_FEATURES(::dtv::spi::Feature::LnbPower,
                                  ::dtv::spi::Feature::Firmware));
DTV_MODEL(fixture, test_model,
          DTV_FEATURES(::dtv::spi::Feature::LnbPower),
          DTV_NO_FIRMWARE,
          (::dtv::spi::model_information{"Test Model", 0x1234, 0x5678}));
DTV_DEFINE_MODEL(fixture, test_model);

static_assert(model_info_fixture_test_model.vendor_id == 0x1234);
static_assert(DTV_MAX_ENCLOSURES == 128);

#define CHECK_OPERATION(product, model, name, operation, feature, signature, arguments) \
    static_assert(std::is_same_v<decltype(feature), ::dtv::spi::Feature>);
DTV_PUBLIC_MODEL_OPS(CHECK_OPERATION, fixture, test_model)
#undef CHECK_OPERATION

extern "C" int32_t dtv_model_impl_lnb_on_fixture_test_model(
    dtv_enclosure *, const dtv_call_options *, dtv_error *)
{
    return DTV_ERROR_OK;
}

extern "C" int32_t dtv_model_impl_lnb_off_fixture_test_model(
    dtv_enclosure *, const dtv_call_options *, dtv_error *)
{
    return DTV_ERROR_OK;
}

extern "C" int32_t dtv_lnb_on_fixture_test_model(
    dtv_enclosure *, const dtv_call_options *, dtv_error *);
extern "C" int32_t dtv_set_pid_filter_fixture_test_model(
    dtv_stream *, const uint16_t *, uint32_t, dtv_error *);
extern "C" int32_t dtv_send_firmware_fixture_test_model(
    dtv_device *, const uint8_t *, uint64_t, const dtv_call_options *, dtv_error *);
extern "C" int32_t dtv_send_default_firmware_fixture_test_model(
    dtv_device *, const dtv_call_options *, dtv_error *);

int main()
{
    dtv_error error{sizeof(dtv_error), 0, DTV_ERROR_DOMAIN_NONE, 0, nullptr, 0};
    if (dtv_lnb_on_fixture_test_model(nullptr, nullptr, &error) != DTV_ERROR_OK) return 1;
    if (dtv_set_pid_filter_fixture_test_model(nullptr, nullptr, 0, &error) != DTV_ERROR_UNSUPPORTED ||
        error.code != DTV_ERROR_UNSUPPORTED) return 2;
    if (dtv_send_firmware_fixture_test_model(nullptr, nullptr, 0, nullptr, &error) != DTV_ERROR_OK ||
        dtv_send_default_firmware_fixture_test_model(nullptr, nullptr, &error) != DTV_ERROR_OK) return 3;
    return 0;
}
