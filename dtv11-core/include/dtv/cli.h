/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DTV_CLI_H
#define DTV_CLI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dtv_model_info {
    /* Borrowed product-owned model name. */
    const char *name;
    uint16_t vendor_id;
    uint16_t product_id;
} dtv_model_info;

typedef struct dtv_cli_config {
    /* All strings are borrowed for the duration of dtv_cli_main. */
    const char *product_name;
    const char *command_name;
    const char *version;
    const char *default_model;
} dtv_cli_config;

typedef const dtv_model_info *(*dtv_model_provider)(size_t index);

/* Returns the source API version. */
uint32_t dtv_api_version(void);

/*
 * Reads but does not modify argv, config, or model data. It retains no caller memory.
 * model_at returns a borrowed descriptor whose name remains valid during this call.
 * Output is written to stdout/stderr. No C++ exception crosses this C ABI.
 */
int dtv_cli_main(int argc,
                 const char *const argv[],
                 const dtv_cli_config *config,
                 dtv_model_provider model_at,
                 size_t model_count);

#ifdef __cplusplus
}
#endif

#endif
