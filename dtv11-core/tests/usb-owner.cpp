// SPDX-License-Identifier: GPL-2.0-or-later
#include <dtv/internal/usb.hpp>

#if defined(__linux__)
#include <fcntl.h>
#include <unistd.h>
#endif

int main()
{
    dtv::internal::UsbContext context;
    if (context.result().code != 0) {
        return 1;
    }
    dtv::internal::UsbDeviceSnapshot invalid_snapshot{};
    auto invalid_device = context.open(invalid_snapshot, 0);
    if (invalid_device->result().code != 2 || invalid_device->io() != nullptr) {
        return 2;
    }

    dtv_result result{};
    if (dtv::internal::open_usb_fd(-1, 0, &result) || result.code != 2) {
        return 3;
    }

#if defined(__linux__)
    int descriptors[2];
    if (pipe(descriptors) != 0) {
        return 4;
    }
    if (fcntl(descriptors[0], F_GETFD) < 0) {
        return 5;
    }
    auto wrapped = dtv::internal::open_usb_fd(descriptors[0], 0, &result);
    if (result.code == 0 || fcntl(descriptors[0], F_GETFD) < 0 ||
        (wrapped != nullptr && wrapped->io() != nullptr)) {
        return 6;
    }
    wrapped.reset();
    if (fcntl(descriptors[0], F_GETFD) < 0) {
        return 7;
    }
    close(descriptors[0]);
    close(descriptors[1]);
#endif
}
