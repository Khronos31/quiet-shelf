// SPDX-License-Identifier: GPL-2.0-only
/*
 * Synchronous C11 bridge from the DiB frontend to the injected Core 1 I2C
 * backend. It owns no USB state and does not retry: submit/wait are the
 * caller-visible contract, and flush is issued before dependent reads.
 */
#include "torne_core.h"

#include <errno.h>
#include <string.h>

static int map_code(uint8_t code)
{
	switch (code) {
	case TORNE_I2C_OK:
		return 0;
	case TORNE_I2C_TIMEOUT:
	case TORNE_I2C_NO_COMPLETION:
	case TORNE_I2C_CORE_STALL:
		return -ETIMEDOUT;
	case TORNE_I2C_DEVICE_GONE:
		return -ENODEV;
	case TORNE_I2C_ADDR_NACK:
	case TORNE_I2C_DATA_NACK:
	case TORNE_I2C_BUS_ERROR:
	case TORNE_I2C_BUS_BUSY:
	case TORNE_I2C_INVALID:
	case TORNE_I2C_SKIPPED:
	case TORNE_I2C_TORN:
	default:
		return -EIO;
	}
}

static int valid_address(uint8_t address)
{
	return address != 0U && (address & 1U) == 0U;
}

static int valid_length(unsigned int length)
{
	return length >= 1U && length <= 32U;
}

static int deadline_expired(struct torne_bridge *b)
{
	if (!b || !b->deadline_ms || !b->ops || !b->ops->now_ms)
		return 0;
	return b->ops->now_ms(b->priv) >= b->deadline_ms;
}

static uint32_t bounded_timeout(struct torne_bridge *b, uint32_t configured)
{
	uint64_t remaining;

	if (!b || !b->deadline_ms || !b->ops || !b->ops->now_ms)
		return configured;
	const u64 now = b->ops->now_ms(b->priv);
	if (now >= b->deadline_ms) return 1U;
	remaining = b->deadline_ms - now;
	if (remaining < configured) return (uint32_t)remaining;
	return configured;
}

static int call_backend(struct torne_bridge *b, const struct torne_i2c_req *req,
			struct torne_i2c_result *res)
{
	uint32_t ticket = 0U;
	int rc;

	if (!b || !b->i2c || !b->i2c->submit || !b->i2c->wait)
		return -EINVAL;
	if (deadline_expired(b))
		return -ETIMEDOUT;
	rc = b->i2c->submit(b->i2c->ctx, req, &ticket);
	if (rc)
		return rc;
	rc = b->i2c->wait(b->i2c->ctx, ticket, res,
			  bounded_timeout(b, b->i2c_timeout_ms ? b->i2c_timeout_ms : 1000U));
	if (rc)
		return rc;
	return map_code(res->code);
}

int torne_i2c_write(struct torne_bridge *b, u8 address, const u8 *data,
		    unsigned int len)
{
	struct torne_i2c_req req;
	struct torne_i2c_result res;

	if (!valid_address(address) || !valid_length(len) || data == NULL)
		return -EINVAL;
	memset(&req, 0, sizeof(req));
	req.op = TORNE_I2C_WRITE;
	req.address = address;
	req.wlen = (uint8_t)len;
	memcpy(req.wdata, data, len);
	return call_backend(b, &req, &res);
}

int torne_i2c_read(struct torne_bridge *b, u8 address, u8 *data,
		   unsigned int len)
{
	struct torne_i2c_req req;
	struct torne_i2c_result res;
	int rc;

	if (!valid_address(address) || !valid_length(len) || data == NULL)
		return -EINVAL;
	memset(&req, 0, sizeof(req));
	req.op = TORNE_I2C_READ;
	req.address = address;
	req.rlen = (uint8_t)len;
	rc = call_backend(b, &req, &res);
	if (rc)
		return rc;
	if (res.rlen < len)
		return -EIO;
	memcpy(data, res.rdata, len);
	return 0;
}

int torne_i2c_write_read(struct torne_bridge *b, u8 address, const u8 *wdata,
			 unsigned int wlen, u8 *rdata, unsigned int rlen)
{
	struct torne_i2c_req req;
	struct torne_i2c_result res;
	struct torne_i2c_result first_failure;
	int rc;

	if (!valid_address(address) || !valid_length(wlen) ||
	    !valid_length(rlen) || wdata == NULL || rdata == NULL)
		return -EINVAL;

	/* A dependent read follows all earlier writes on the same bus. */
	if (b && b->i2c && b->i2c->flush) {
		if (deadline_expired(b))
			return -ETIMEDOUT;
		memset(&first_failure, 0, sizeof(first_failure));
		rc = b->i2c->flush(b->i2c->ctx,
				   bounded_timeout(b, b->i2c_timeout_ms ? b->i2c_timeout_ms : 1000U),
				   &first_failure);
		if (rc)
			return rc;
		if (first_failure.code != TORNE_I2C_OK)
			return map_code(first_failure.code);
	}

	memset(&req, 0, sizeof(req));
	req.op = TORNE_I2C_WRITE_READ;
	req.address = address;
	req.wlen = (uint8_t)wlen;
	req.rlen = (uint8_t)rlen;
	memcpy(req.wdata, wdata, wlen);
	rc = call_backend(b, &req, &res);
	if (rc)
		return rc;
	if (res.rlen < rlen)
		return -EIO;
	memcpy(rdata, res.rdata, rlen);
	return 0;
}
