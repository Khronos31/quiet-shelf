/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DTV_SPI_TYPES_H
#define DTV_SPI_TYPES_H

#include <stdint.h>

typedef void dtv_context;

typedef struct dtv_open_args dtv_open_args;
typedef struct dtv_identify_info dtv_identify_info;
typedef struct dtv_firmware_result dtv_firmware_result;
typedef struct dtv_tune_result dtv_tune_result;
typedef struct dtv_signal_result dtv_signal_result;
struct dtv_model_info;

typedef uint64_t dtv_device;
typedef uint64_t dtv_stream;
typedef uint32_t dtv_receiver;
typedef uint32_t dtv_function;
typedef uint64_t dtv_fw_key;

typedef struct dtv_bytes {
    const uint8_t *data;
    uint64_t size;
} dtv_bytes;

typedef struct dtv_buffer {
    uint8_t *data;
    uint64_t capacity;
    uint64_t size;
} dtv_buffer;

typedef struct dtv_timeout {
    uint64_t timeout_ms;
} dtv_timeout;

struct dtv_open_args {
    const struct dtv_model_info *model;
    dtv_function function;
    dtv_timeout timeout;
};

typedef struct dtv_result {
    int32_t code;
    uint32_t detail;
} dtv_result;

typedef struct dtv_measurement {
    uint64_t value;
    uint32_t valid;
    uint32_t unit;
} dtv_measurement;

typedef struct dtv_usb_io {
    void *user;
    dtv_result (*control)(void *, uint8_t, uint8_t, uint16_t, uint16_t,
                          dtv_buffer *, dtv_timeout);
    dtv_result (*bulk_out)(void *, uint8_t, dtv_bytes, dtv_timeout);
    dtv_result (*bulk_in)(void *, uint8_t, dtv_buffer *, dtv_timeout);
    dtv_result (*interrupt_in)(void *, uint8_t, dtv_buffer *, dtv_timeout);
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

struct dtv_tune_result {
    uint64_t revision;
};

struct dtv_signal_result {
    dtv_measurement *measurements;
    uint32_t capacity;
    uint32_t count;
};

#endif
