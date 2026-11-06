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
struct dtv_usb_candidate_info;

typedef uint64_t dtv_device_id;
typedef uint64_t dtv_stream_id;
typedef uint32_t dtv_spi_receiver_id;
typedef uint32_t dtv_function;
typedef uint64_t dtv_fw_key;

typedef struct dtv_spi_measurement {
    uint64_t value;
    uint32_t valid;
    uint32_t unit;
} dtv_spi_measurement;

typedef struct dtv_timeout {
    uint64_t timeout_ms;
    /* 全callback/転送で共有する単調時計上の絶対期限。0は指定なし。 */
    uint64_t operation_deadline_ns;
} dtv_timeout;

struct dtv_open_args {
    const struct dtv_model_info *model;
    dtv_function function;
    dtv_timeout timeout;
    /* Borrowed snapshot observation; valid for this open callback. */
    const struct dtv_usb_candidate_info *usb_observation;
    uint8_t interface_number;
    uint8_t alternate_setting;
};

typedef struct dtv_internal_result {
    /* dtv_internal_result_code。dtv_resultとは別の番号空間。 */
    int32_t code;
    /* 操作固有値。USBの実長・libusb detailの既存意味を保つ。 */
    uint32_t detail;
} dtv_internal_result;

/* Descriptor fields are fixed-width observations borrowed for callback duration only. */
typedef struct dtv_usb_endpoint_info {
    uint8_t address;
    uint8_t attributes;
    uint16_t max_packet_size;
    uint8_t interval;
} dtv_usb_endpoint_info;

typedef struct dtv_usb_interface_info {
    uint8_t configuration;
    uint8_t interface_number;
    uint8_t alternate_setting;
    uint8_t class_code;
    uint8_t subclass_code;
    uint8_t protocol_code;
    const dtv_usb_endpoint_info *endpoints;
    uint32_t endpoint_count;
} dtv_usb_interface_info;

typedef struct dtv_usb_candidate_info {
    uint64_t candidate_id;
    uint16_t vendor_id;
    uint16_t product_id;
    uint16_t device_version;
    uint8_t bus_number;
    uint8_t device_address;
    const uint8_t *port_path;
    uint32_t port_path_length;
    const dtv_usb_interface_info *interfaces;
    uint32_t interface_count;
} dtv_usb_candidate_info;

typedef struct dtv_usb_function_selection {
    uint32_t candidate_index;
    dtv_function function;
    uint8_t interface_number;
    uint8_t alternate_setting;
} dtv_usb_function_selection;

typedef enum dtv_usb_group_match {
    DTV_USB_GROUP_NO_MATCH = 0,
    DTV_USB_GROUP_COMPLETE = 1,
    DTV_USB_GROUP_AMBIGUOUS = 2,
    DTV_USB_GROUP_INCOMPLETE = 3
} dtv_usb_group_match;

typedef struct dtv_usb_identity {
    uint16_t vendor_id;
    uint16_t product_id;
} dtv_usb_identity;

/* capacity is the complete one-shot selection capacity; outputs are borrowed for this call. */
typedef dtv_internal_result (*dtv_select_usb_group_fn)(
    const dtv_usb_candidate_info *, uint32_t, uint32_t,
    dtv_usb_function_selection *, uint32_t, uint32_t *, dtv_usb_group_match *);

enum dtv_internal_result_code {
    DTV_INTERNAL_RESULT_OK = 0, /* 操作成功。 */
    DTV_INTERNAL_RESULT_INVALID_ARGUMENT = 2, /* callbackへの入力が不正。 */
    DTV_INTERNAL_RESULT_UNSUPPORTED = 3, /* 有効な操作だが機種・OS等で非対応。 */
    DTV_INTERNAL_RESULT_USB_IO = 7, /* USB I/O失敗。 */
    DTV_INTERNAL_RESULT_TIMEOUT = 10, /* 期限切れ、または即時確認時に未完了。 */
    DTV_INTERNAL_RESULT_RESOURCE_EXHAUSTED = 11, /* Coreまたは製品の資源不足。 */
    DTV_INTERNAL_RESULT_INTERNAL = 12, /* その他の予期しない例外。 */
    DTV_INTERNAL_RESULT_BUSY = 13, /* 機器controller使用中。候補曖昧さには使わない。 */
    DTV_INTERNAL_RESULT_NOT_READY = 14 /* operationに必要なfunctionがまだ見えない。 */
};

typedef struct dtv_runtime_clock {
    void *user;
    uint64_t (*monotonic_now_ns)(void *);
    /* wake、期限、cancelのいずれかまで待つ。cancel時はcancelledを非0にする。 */
    dtv_internal_result (*delay_until)(void *, uint64_t, uint64_t,
                                       const dtv_cancel *, uint32_t *);
    /* Core runtimeが所有するcancel状態の照会。未接続tokenはUNSUPPORTEDを返す。 */
    dtv_internal_result (*is_cancelled)(void *, const dtv_cancel *, uint32_t *);
} dtv_runtime_clock;

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
    /* transport generationの寿命中だけ借用できるCore clock view。 */
    const dtv_runtime_clock *clock;
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
    /* DTV_IDENTIFY_*。firmware_keyはRUNTIME時だけ有効。 */
    uint32_t state;
};

#define DTV_IDENTIFY_UNKNOWN 0u
#define DTV_IDENTIFY_LOADER 1u
#define DTV_IDENTIFY_RUNTIME 2u

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
    dtv_spi_measurement *measurements;
    uint32_t capacity;
    uint32_t count;
};

#endif
