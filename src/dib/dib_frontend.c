// SPDX-License-Identifier: GPL-2.0-only
/*
 * C11 frontend boundary for the DiB8096. The DiB8000/DiB0090 core is the
 * GPL-2.0-only adaptation of torne_drv's torne_dib.c. This file only supplies
 * the injected Core 1 I2C backend, explicit sleep/clock boundaries and a
 * small frontend API.
 */
#include "dib_frontend.h"

#include <errno.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>

static void sleep_cb(void *priv, unsigned int ms)
{
	struct torne_dib_frontend *f = priv;

	f->sleep(f->priv, ms);
}

static u64 now_cb(void *priv)
{
	struct torne_dib_frontend *f = priv;

	return f->now(f->priv);
}

static void log_cb(void *priv, int level, const char *fmt, ...)
{
	struct torne_dib_frontend *f = priv;
	char message[256];
	va_list ap;

	if (!f->log)
		return;
	va_start(ap, fmt);
	(void)vsnprintf(message, sizeof(message), fmt, ap);
	va_end(ap);
	f->log(f->priv, level, message);
}

static const struct torne_ops frontend_ops = {
	NULL,
	NULL,
	sleep_cb,
	now_cb,
	log_cb,
};

int torne_dib_frontend_init(struct torne_dib_frontend *frontend,
			    const struct torne_dib_frontend_config *config)
{
	if (!frontend || !config || !config->backend ||
	    !config->backend->submit || !config->backend->wait ||
	    !config->core1_and_gpio_ready || !config->sleep || !config->now)
		return -EINVAL;

	frontend->backend = config->backend;
	frontend->sleep = config->sleep;
	frontend->now = config->now;
	frontend->log = config->log;
	frontend->priv = config->priv;

	frontend->bridge.ops = &frontend_ops;
	frontend->bridge.priv = frontend;
	frontend->bridge.i2c = config->backend;
	frontend->bridge.i2c_timeout_ms = config->i2c_timeout_ms;
	frontend->bridge.deadline_ms = 0U;
	frontend->bridge.err = 0;
	frontend->bridge.log_level = TORNE_LOG_ERR;

	torne_dib_setup(&frontend->dib, &frontend->bridge);
	return torne_dib_init(&frontend->dib);
}

int torne_dib_frontend_set_output(struct torne_dib_frontend *frontend,
				  enum torne_dib_output mode)
{
	if (!frontend) return -EINVAL;
	return torne_dib_set_output(&frontend->dib, mode);
}

int torne_dib_frontend_set_gpio(struct torne_dib_frontend *frontend,
				uint8_t pin, bool high)
{
	if (!frontend) return -EINVAL;
	return torne_dib_set_gpio(&frontend->dib, pin, high);
}

int torne_dib_frontend_tune(struct torne_dib_frontend *frontend,
			    uint32_t frequency_hz)
{
	if (!frontend) return -EINVAL;
	return torne_dib_tune(&frontend->dib, frequency_hz);
}

int torne_dib_frontend_tune_timeout(struct torne_dib_frontend *frontend,
				    uint32_t frequency_hz,
				    uint32_t timeout_ms)
{
	if (!frontend || !frontend->now) return -EINVAL;
	frontend->bridge.deadline_ms =
		frontend->now(frontend->priv) + (u64)timeout_ms;
	const int rc = torne_dib_frontend_tune(frontend, frequency_hz);
	frontend->bridge.deadline_ms = 0U;
	return rc;
}

int torne_dib_frontend_status(struct torne_dib_frontend *frontend,
			      struct torne_dib_status *status)
{
	if (!frontend || !status) return -EINVAL;
	return torne_dib_status(&frontend->dib, status);
}

int torne_dib_frontend_sleep(struct torne_dib_frontend *frontend)
{
	if (!frontend) return -EINVAL;
	return torne_dib_sleep(&frontend->dib);
}
