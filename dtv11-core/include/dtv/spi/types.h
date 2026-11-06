/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DTV_SPI_TYPES_H
#define DTV_SPI_TYPES_H

#include <stdint.h>

#include <dtv/types.h>

typedef void dtv_context;

typedef struct dtv_open_args dtv_open_args;
typedef struct dtv_identify_info dtv_identify_info;
typedef struct dtv_firmware_result dtv_firmware_result;
typedef struct dtv_spi_tune_result dtv_spi_tune_result;
typedef struct dtv_signal_result dtv_signal_result;

typedef uint64_t dtv_device_id;
typedef uint64_t dtv_stream_id;
typedef uint32_t dtv_receiver;
typedef uint32_t dtv_function;
typedef uint64_t dtv_fw_key;

typedef struct dtv_timeout {
    uint64_t timeout_ms;
} dtv_timeout;

struct dtv_open_args {
    const struct dtv_model_info *model;
    dtv_function function;
    dtv_timeout timeout;
};

typedef struct dtv_internal_result {
    /* dtv_internal_result_code。dtv_resultとは別の番号空間。 */
    int32_t code;
    /* 操作固有値。USBの実長・libusb detailの既存意味を保つ。 */
    uint32_t detail;
} dtv_internal_result;

enum dtv_internal_result_code {
    DTV_INTERNAL_RESULT_OK = 0, /* 操作成功。 */
    DTV_INTERNAL_RESULT_INVALID_ARGUMENT = 2, /* callbackへの入力が不正。 */
    DTV_INTERNAL_RESULT_UNSUPPORTED = 3, /* 有効な操作だが機種・OS等で非対応。 */
    DTV_INTERNAL_RESULT_USB_IO = 7, /* USB I/O失敗。 */
    DTV_INTERNAL_RESULT_TIMEOUT = 10 /* 期限切れ、または即時確認時に未完了。 */
};

/*
 * 成功判定はcode == DTV_INTERNAL_RESULT_OKだけで行う。製品callbackはUSB結果を
 * dtv_internal_resultのまま伝播できる。detailは共通化・変換せず保持する。
 * USBではlibusb失敗のdetailはlibusb errorの絶対値、short OUTと成功bulk OUTの
 * detailは転送byte数、INの実長はbuffer->sizeであり、local拒否/即時timeoutは0。
 * 製品自身の拒否は同じ内部番号で作る。後続の公開API境界では明示的にmappingし、
 * 公開dtv_resultへ数値cast/aliasしてはならない。
 *
 *   dtv_internal_result result = io->bulk_out(io->user, endpoint, bytes, timeout);
 *   if (result.code != DTV_INTERNAL_RESULT_OK) return result;
 *   dtv_internal_result rejected = {DTV_INTERNAL_RESULT_UNSUPPORTED, 0};
 *   return rejected;
 */

typedef struct dtv_usb_io {
    void *user;
    dtv_internal_result (*control)(void *, uint8_t, uint8_t, uint16_t, uint16_t,
                          dtv_buffer *, dtv_timeout);
    dtv_internal_result (*bulk_out)(void *, uint8_t, dtv_bytes, dtv_timeout);
    dtv_internal_result (*bulk_in)(void *, uint8_t, dtv_buffer *, dtv_timeout);
    dtv_internal_result (*interrupt_in)(void *, uint8_t, dtv_buffer *, dtv_timeout);
} dtv_usb_io;

typedef struct dtv_pid_route {
    uint16_t slot;
    uint16_t pid;
    uint32_t enabled;
    uint16_t ecm_link;
} dtv_pid_route;

typedef struct dtv_stream_program {
    uint16_t program_number;
    uint8_t video_stream_type;
    uint8_t audio_stream_type;
    const dtv_pid_route *routes;
    uint32_t route_count;
} dtv_stream_program;

struct dtv_identify_info {
    dtv_fw_key firmware_key;
};

#define DTV_FIRMWARE_SAME_HANDLE 0u
#define DTV_FIRMWARE_REENUMERATE 1u
#define DTV_FIRMWARE_NEW_HANDLE 2u

struct dtv_firmware_result {
    dtv_fw_key firmware_key;
    uint32_t transition;
};

struct dtv_spi_tune_result {
    uint64_t revision;
};

struct dtv_signal_result {
    dtv_measurement *measurements;
    uint32_t capacity;
    uint32_t count;
};

#endif
