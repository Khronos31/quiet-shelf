/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <dtv/internal/l3.hpp>

extern "C" void dtv_load_assetfixture(void);
int main()
{
    dtv_load_assetfixture();
    return dtv::internal::dispatch_send_default_firmware(
               "assetfixture", "Firmware Fixture", nullptr, nullptr, nullptr) == DTV_ERROR_OK
               ? 0
               : 1;
}
