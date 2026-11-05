/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <dtv/cli.h>

int main(void)
{
    const char *argv[] = {"consumer", "--version"};
    const dtv_cli_config config = {"consumer", "consumer", "1.0.0", ""};
    return dtv_api_version() == 1
               ? dtv_cli_main(2, argv, &config, 0, 0)
               : 1;
}
