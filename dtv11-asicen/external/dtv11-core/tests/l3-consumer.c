/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <dtv/dtv.h>
#include <dtv/types.h>

int main(void)
{
    const dtv_model *const *models = dtv_list_models();
    return dtv_get_api_version() == UINT32_C(0x00020000) && models != 0 && models[0] == 0
               ? 0
               : 1;
}
