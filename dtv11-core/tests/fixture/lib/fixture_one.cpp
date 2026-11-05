// SPDX-License-Identifier: GPL-2.0-or-later
#include <dtv/spi/device_ops.hpp>

namespace dtv::spi {

constexpr DeviceOps fixture_one_ops{
    .open = nullptr,
    .close = nullptr,
};

static_assert(fixture_one_ops.open == nullptr &&
              fixture_one_ops.identify == nullptr &&
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
};

}
