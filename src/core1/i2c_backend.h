/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef TORNE_CORE1_I2C_BACKEND_H
#define TORNE_CORE1_I2C_BACKEND_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* opcodes (§3.5) */
#define TORNE_I2C_WRITE      1
#define TORNE_I2C_READ       2
#define TORNE_I2C_WRITE_READ 3

/* status codes (§3.6) plus host-only outcomes */
#define TORNE_I2C_OK              0x00
#define TORNE_I2C_ADDR_NACK       0x01
#define TORNE_I2C_DATA_NACK       0x02
#define TORNE_I2C_BUS_ERROR       0x03
#define TORNE_I2C_TIMEOUT         0x04
#define TORNE_I2C_BUS_BUSY        0x05
#define TORNE_I2C_INVALID         0x10
#define TORNE_I2C_SKIPPED         0x11
#define TORNE_I2C_TORN            0x12
#define TORNE_I2C_NO_COMPLETION   0x20
#define TORNE_I2C_DEVICE_GONE     0x21
#define TORNE_I2C_CORE_STALL      0x22

/* API return values (negative only for API/transport failures) */
#define TORNE_I2C_ERR_INVALID   (-1)
#define TORNE_I2C_ERR_BUSY      (-2)
#define TORNE_I2C_ERR_HALTED    (-3)
#define TORNE_I2C_ERR_PROTOCOL  (-4)
#define TORNE_I2C_ERR_DEVICE    (-5)
#define TORNE_I2C_ERR_IO        (-6)
#define TORNE_I2C_ERR_TIMEOUT   (-7)
#define TORNE_I2C_ERR_INTERNAL  (-8)

typedef struct torne_i2c_req {
    uint8_t op;
    uint8_t address;      /* 8-bit form, bit0 = 0 */
    uint8_t wlen;
    uint8_t rlen;
    uint8_t wdata[32];
} torne_i2c_req;

typedef struct torne_i2c_result {
    uint8_t code;
    uint8_t phase;
    uint8_t acked;
    uint8_t bus_idle;     /* 1, 0, or 0xFF when unknown */
    uint8_t rlen;
    uint8_t rdata[32];
} torne_i2c_result;

typedef struct torne_i2c_backend {
    void *ctx;
    uint32_t caps;
    int (*submit)(void *ctx, const torne_i2c_req *req, uint32_t *ticket);
    int (*wait)(void *ctx, uint32_t ticket, torne_i2c_result *res,
                uint32_t timeout_ms);
    int (*flush)(void *ctx, uint32_t timeout_ms,
                 torne_i2c_result *first_failure);
    int (*resume)(void *ctx, int discard);
} torne_i2c_backend;

#ifdef __cplusplus
}
#endif

#endif  /* TORNE_CORE1_I2C_BACKEND_H */
