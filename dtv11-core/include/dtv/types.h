/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DTV_TYPES_H
#define DTV_TYPES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dtv_device dtv_device;
typedef struct dtv_service dtv_service;
typedef struct dtv_discovery dtv_discovery;
typedef struct dtv_status_snapshot dtv_status_snapshot;
typedef struct dtv_stream dtv_stream;
typedef struct dtv_card dtv_card;
typedef struct dtv_cancel dtv_cancel;

typedef struct dtv_result {
    /* DTV_ERROR_*。CLI終了値とは別の番号空間。 */
    int32_t code;
    /* 2.0.0では常に0。 */
    uint32_t detail;
} dtv_result;

typedef struct dtv_bytes {
    const uint8_t *data;
    uint64_t size;
} dtv_bytes;

typedef struct dtv_buffer {
    uint8_t *data;
    uint64_t capacity;
    uint64_t size;
} dtv_buffer;

typedef struct dtv_measurement {
    uint64_t value;
    uint32_t valid;
    uint32_t unit;
} dtv_measurement;

typedef struct dtv_model_info {
    /* 製品が所有し、製品moduleの寿命中有効なname。 */
    const char *name;
    uint16_t vendor_id;
    uint16_t product_id;
} dtv_model_info;

typedef struct dtv_call_options {
    uint32_t struct_size;
    uint64_t timeout_ms;
    dtv_cancel *cancel;
} dtv_call_options;

typedef struct dtv_device_options {
    uint32_t struct_size;
    dtv_call_options call;
} dtv_device_options;

typedef struct dtv_connect_options {
    uint32_t struct_size;
    dtv_call_options call;
    const char *runtime_dir;
    const char *instance;
    uint32_t flags;
} dtv_connect_options;

typedef struct dtv_service_options {
    uint32_t struct_size;
    dtv_call_options call;
    const char *runtime_dir;
    const char *instance;
    uint32_t flags;
} dtv_service_options;

typedef struct dtv_tune_options {
    uint32_t struct_size;
    dtv_call_options call;
    uint32_t selector_kind;
    uint32_t selector_value;
} dtv_tune_options;

typedef struct dtv_stream_options {
    uint32_t struct_size;
    dtv_call_options call;
    uint32_t receiver;
} dtv_stream_options;

typedef struct dtv_json_producer {
    uint32_t struct_size;
    const char *product;
    const char *version;
    const char *core_version;
} dtv_json_producer;

typedef struct dtv_wait_options {
    uint32_t struct_size;
    uint64_t timeout_ms;
} dtv_wait_options;

typedef struct dtv_discovery_options {
    uint32_t struct_size;
    dtv_call_options call;
} dtv_discovery_options;

typedef struct dtv_firmware_options {
    uint32_t struct_size;
    dtv_call_options call;
} dtv_firmware_options;

typedef struct dtv_firmware_state {
    uint32_t struct_size;
    uint32_t transition;
    uint32_t reenumeration_required;
} dtv_firmware_state;

typedef struct dtv_l3_tune_result {
    uint32_t struct_size;
    uint64_t revision;
} dtv_l3_tune_result;

typedef struct dtv_read_result {
    uint32_t struct_size;
    uint64_t bytes_read;
    uint32_t end_of_stream;
} dtv_read_result;

typedef struct dtv_device_info {
    uint32_t struct_size;
    const char *model;
    const char *serial;
    const char *usb_path;
    uint16_t vendor_id;
    uint16_t product_id;
} dtv_device_info;

typedef struct dtv_signal_info {
    uint32_t struct_size;
    dtv_measurement lock;
    dtv_measurement cn;
    dtv_measurement agc;
} dtv_signal_info;

typedef struct dtv_capabilities {
    uint32_t struct_size;
    uint32_t state;
} dtv_capabilities;

typedef struct dtv_device_status {
    uint32_t struct_size;
    uint32_t state;
    uint32_t valid;
    uint64_t firmware_key;
} dtv_device_status;

typedef struct dtv_card_options {
    uint32_t struct_size;
    dtv_call_options call;
    uint32_t reader;
} dtv_card_options;

typedef struct dtv_card_status {
    uint32_t struct_size;
    uint32_t present;
    uint32_t valid;
    uint32_t generation;
} dtv_card_status;

enum dtv_error {
    DTV_ERROR_OK = 0,
    DTV_ERROR_INVALID_ARGUMENT = 1,
    DTV_ERROR_NOT_FOUND = 2,
    DTV_ERROR_NOT_READY = 3,
    DTV_ERROR_UNSUPPORTED = 4,
    DTV_ERROR_BUSY = 5,
    DTV_ERROR_TIMEOUT = 6,
    DTV_ERROR_VERSION_MISMATCH = 7,
    DTV_ERROR_PROTOCOL_ERROR = 8,
    DTV_ERROR_USB_IO = 9,
    DTV_ERROR_DISCONNECTED = 10,
    DTV_ERROR_TS_ERROR = 11,
    DTV_ERROR_NO_CARD = 12,
    DTV_ERROR_CARD_REMOVED = 13,
    DTV_ERROR_CARD_IO = 14,
    DTV_ERROR_BUFFER_TOO_SMALL = 15,
    DTV_ERROR_FIRMWARE_INVALID = 16,
    DTV_ERROR_PERMISSION_DENIED = 17,
    DTV_ERROR_RESOURCE_LIMIT = 18,
    DTV_ERROR_OUTPUT_ERROR = 19,
    DTV_ERROR_INTERNAL = 20
};

enum dtv_system { DTV_SYSTEM_ISDB_T = 1, DTV_SYSTEM_ISDB_S = 2 };
enum dtv_selector_kind { DTV_SELECTOR_NONE = 0, DTV_SELECTOR_SLOT = 1, DTV_SELECTOR_TSID = 2 };
enum dtv_capability_state {
    DTV_CAPABILITY_UNKNOWN = 0,
    DTV_CAPABILITY_SUPPORTED = 1,
    DTV_CAPABILITY_UNSUPPORTED = 2
};
enum dtv_device_state {
    DTV_DEVICE_UNKNOWN = 0,
    DTV_DEVICE_READY = 1,
    DTV_DEVICE_BUSY = 2,
    DTV_DEVICE_STOPPING = 3,
    DTV_DEVICE_STOPPED = 4,
    DTV_DEVICE_ERROR = 5
};
enum dtv_firmware_transition {
    DTV_FW_SAME_HANDLE = 0,
    DTV_FW_REENUMERATE = 1,
    DTV_FW_NEW_HANDLE = 2
};
enum dtv_unit { DTV_UNIT_UNSPECIFIED = 0 };

#define DTV_CONNECT_GROUP_ACCESS UINT32_C(0x00000001)
#define DTV_SERVICE_GROUP_ACCESS UINT32_C(0x00000001)
#define DTV_SERVICE_ALLOW_LNB_POWER UINT32_C(0x00000002)

#ifdef __cplusplus
}
#endif

#endif
