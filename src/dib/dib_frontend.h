/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef TORNE_DIB_FRONTEND_H
#define TORNE_DIB_FRONTEND_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "core1/i2c_backend.h"
#include "torne_dib.h"

typedef void (*torne_dib_frontend_sleep_fn)(void *priv, unsigned int ms);
typedef uint64_t (*torne_dib_frontend_now_fn)(void *priv);
typedef void (*torne_dib_frontend_log_fn)(void *priv, int level,
					  const char *message);

/* The caller supplies a Core 1 I2C backend and explicit sleep/clock/log
 * boundaries. core1_and_gpio_ready states that Core 1 is running and the
 * MB86H47 GPIO/power precondition (S6) is already true; this frontend never
 * performs those operations. */
struct torne_dib_frontend_config {
	const struct torne_i2c_backend *backend;
	uint32_t i2c_timeout_ms;
	bool core1_and_gpio_ready;
	torne_dib_frontend_sleep_fn sleep;
	torne_dib_frontend_now_fn now;
	torne_dib_frontend_log_fn log;
	void *priv;
};

struct torne_dib_frontend {
	struct torne_dib dib;
	struct torne_bridge bridge;
	const struct torne_i2c_backend *backend;
	torne_dib_frontend_sleep_fn sleep;
	torne_dib_frontend_now_fn now;
	torne_dib_frontend_log_fn log;
	void *priv;
};

int torne_dib_frontend_init(struct torne_dib_frontend *frontend,
			    const struct torne_dib_frontend_config *config);
int torne_dib_frontend_set_output(struct torne_dib_frontend *frontend,
				  enum torne_dib_output mode);
int torne_dib_frontend_set_gpio(struct torne_dib_frontend *frontend,
				uint8_t pin, bool high);
int torne_dib_frontend_tune(struct torne_dib_frontend *frontend,
			    uint32_t frequency_hz);
/* Bounds the frontend/I2C tune sequence by one monotonic deadline. */
int torne_dib_frontend_tune_timeout(struct torne_dib_frontend *frontend,
				    uint32_t frequency_hz,
				    uint32_t timeout_ms);
int torne_dib_frontend_status(struct torne_dib_frontend *frontend,
			      struct torne_dib_status *status);
int torne_dib_frontend_sleep(struct torne_dib_frontend *frontend);

#ifdef __cplusplus
}
#endif

#endif /* TORNE_DIB_FRONTEND_H */
