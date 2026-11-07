/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DTV_CLI_H
#define DTV_CLI_H

#include <stdint.h>

#include <dtv/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dtv_cli_config {
    uint32_t struct_size;
    /* 文字列はdtv_cli_mainの呼出し中だけ借用する。 */
    const char *product_name;
    const char *command_name;
    const char *version;
    const char *default_model;
} dtv_cli_config;

typedef const dtv_model_info *(*dtv_model_at_fn)(uint32_t index);

/* モデル名は製品moduleの寿命中有効な借用文字列。 */
dtv_result dtv_model_count(uint32_t *out_count);
dtv_result dtv_model_get(uint32_t index, dtv_model_info *out_model);

uint32_t dtv_api_version(void);

/*
 * model_atが返すdescriptorは呼出し中だけ借用する。
 */
int32_t dtv_cli_main(int32_t argc,
                 const char *const argv[],
                 const dtv_cli_config *config,
                 dtv_model_at_fn model_at,
                 uint32_t model_count);

#ifdef __cplusplus
}
#endif

#endif
