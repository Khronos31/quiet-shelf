// SPDX-License-Identifier: GPL-2.0-or-later
#include <dtv/spi/device_ops.hpp>
#include <dtv/spi/feature.hpp>

namespace dtv::spi {

namespace {

struct FixtureState {
    dtv_device_id device;
};

uint32_t fixture_one_destroy_count;

dtv_internal_result fixture_one_create_state(dtv_device_id device, dtv_context **out_context)
{
    *out_context = new FixtureState{device};
    return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
}

void fixture_one_destroy_state(dtv_context *context)
{
    delete static_cast<FixtureState *>(context);
    ++fixture_one_destroy_count;
}

dtv_internal_result fixture_one_identify(dtv_context *context, dtv_identify_info *info, dtv_timeout)
{
    info->firmware_key = static_cast<FixtureState *>(context)->device;
    return dtv_internal_result_make(DTV_INTERNAL_RESULT_OK, 0);
}

}

uint32_t fixture_one_destroy_count_for_test()
{
    return fixture_one_destroy_count;
}

constexpr DeviceOps fixture_one_ops{
    .open = nullptr,
    .identify = fixture_one_identify,
    .close = nullptr,
};

static_assert(fixture_one_ops.open == nullptr &&
              fixture_one_ops.identify != nullptr &&
              fixture_one_ops.firmware == nullptr &&
              fixture_one_ops.tune_t == nullptr &&
              fixture_one_ops.signal == nullptr &&
              fixture_one_ops.ts_prepare == nullptr &&
              fixture_one_ops.service_tick == nullptr &&
              fixture_one_ops.ts_transform == nullptr &&
              fixture_one_ops.ts_stop_device == nullptr &&
              fixture_one_ops.close == nullptr &&
              fixture_one_ops.card_uart_write_byte == nullptr &&
              fixture_one_ops.card_uart_read_byte == nullptr &&
              fixture_one_ops.card_frame_write == nullptr &&
              fixture_one_ops.card_frame_read == nullptr);

const FirmwareEntry fixture_one_firmware[]{{0, &fixture_one_ops}};

extern const ModelEntry fixture_one_model{
    .model = {"Fixture One", 0x1234, 0x0001},
    .firmware = fixture_one_firmware,
    .firmware_count = 1,
    .create_state = fixture_one_create_state,
    .destroy_state = fixture_one_destroy_state,
};

}

DTV_PRODUCT(fixture, DTV_FEATURES(::dtv::spi::Feature::LnbPower));
DTV_MODEL(fixture, fixture_one, DTV_FEATURES(::dtv::spi::Feature::LnbPower), DTV_NO_FIRMWARE,
          (::dtv::spi::model_information{"Fixture One", 0x1234, 0x0001}));
DTV_DEFINE_MODEL(fixture, fixture_one);

extern "C" int32_t dtv_model_impl_lnb_on_fixture_fixture_one(
    dtv_enclosure *, const dtv_call_options *, dtv_error *)
{
    return DTV_ERROR_OK;
}

extern "C" int32_t dtv_model_impl_lnb_off_fixture_fixture_one(
    dtv_enclosure *, const dtv_call_options *, dtv_error *)
{
    return DTV_ERROR_OK;
}
