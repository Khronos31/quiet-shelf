/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#include <dtv/dtv.h>
#include <scanfixture_api.h>

void fd_test_install_backend(void);
void fd_test_reset(void);
void fd_test_fail_wrap(unsigned);
void fd_test_duplicate_second(unsigned);
void fd_test_reinserted(unsigned);
void fd_test_fail_config(unsigned);
void fd_test_group_error(unsigned);
int fd_test_last_descriptor(void);
unsigned fd_test_closed(void);
void dtv_load_scanfixture(void);

static int valid_fd(int fd)
{
    return fcntl(fd, F_GETFD) >= 0;
}

int main(void)
{
    int first[2], second[2];
    if (pipe(first) != 0 || pipe(second) != 0) return 1;
    fd_test_install_backend();
    dtv_load_scanfixture();
    dtv_error error = {.struct_size = sizeof(error)};
    dtv_enclosure *const *normal = dtv_list_enclosures(&error);
    if (!normal || !normal[0] || normal[1] || error.code != DTV_ERROR_OK) return 2;
    dtv_enclosure *root = normal[0];

    fd_test_reset();
    int32_t fds[] = {first[0], second[0]};
    dtv_enclosure *const *selected = dtv_list_enclosures_by_fds(fds, 2, &error);
    if (!selected || selected[0] != root || selected[1] || error.code != DTV_ERROR_OK) return 3;
    if (!valid_fd(first[0]) || !valid_fd(second[0]) ||
        fd_test_last_descriptor() == second[0] ||
        (fcntl(fd_test_last_descriptor(), F_GETFD) & FD_CLOEXEC) == 0 ||
        fd_test_closed() != 0) return 4;
    selected = dtv_list_enclosures_by_fds_scanfixture(fds, 2, &error);
    if (!selected || selected[0] != root || selected[1] || error.code != DTV_ERROR_OK ||
        fd_test_closed() != 2) return 9;
    if (dtv_list_enclosures_by_fds(fds, 1, &error) == NULL ||
        error.code != DTV_ERROR_OK || dtv_get_status(root) != DTV_ENCLOSURE_READY) return 5;
    selected = dtv_list_enclosures_by_fds(NULL, 0, &error);
    if (!selected || selected[0] || error.code != DTV_ERROR_OK ||
        dtv_get_status(root) != DTV_ENCLOSURE_READY) return 11;
    int32_t repeated[] = {first[0], first[0]};
    fd_test_reset();
    if (dtv_list_enclosures_by_fds(repeated, 2, &error) != NULL ||
        error.code != DTV_ERROR_INVALID_ARGUMENT || fd_test_closed() != 0) return 12;

    fd_test_reset();
    fd_test_duplicate_second(1);
    if (dtv_list_enclosures_by_fds(fds, 2, &error) != NULL ||
        error.code != DTV_ERROR_INVALID_ARGUMENT || fd_test_closed() != 2 ||
        dtv_get_status(root) != DTV_ENCLOSURE_READY) return 6;

    fd_test_reset();
    fd_test_fail_wrap(2);
    if (dtv_list_enclosures_by_fds(fds, 2, &error) != NULL ||
        error.code != DTV_ERROR_PERMISSION_DENIED || fd_test_closed() != 1 ||
        !valid_fd(first[0]) || !valid_fd(second[0]) ||
        dtv_get_status(root) != DTV_ENCLOSURE_READY) return 7;
    fd_test_reset();
    fd_test_fail_config(1);
    if (dtv_list_enclosures_by_fds(fds, 2, &error) != NULL ||
        error.code != DTV_ERROR_USB_IO || fd_test_closed() != 1 ||
        !valid_fd(first[0]) || !valid_fd(second[0]) ||
        dtv_get_status(root) != DTV_ENCLOSURE_READY) return 13;
    fd_test_reset();
    fd_test_group_error(1);
    if (dtv_list_enclosures_by_fds(fds, 2, &error) != NULL ||
        error.code != DTV_ERROR_UNSUPPORTED || fd_test_closed() != 2 ||
        dtv_get_status(root) != DTV_ENCLOSURE_READY) return 14;
    fd_test_group_error(0);
    int stale[2];
    if (pipe(stale) != 0) return 15;
    close(stale[0]);
    fd_test_reset();
    int32_t stale_fd = stale[0];
    if (dtv_list_enclosures_by_fds(&stale_fd, 1, &error) != NULL ||
        error.code != DTV_ERROR_INVALID_ARGUMENT || fd_test_closed() != 0) return 16;
    close(stale[1]);
    fd_test_reset();
    fd_test_reinserted(1);
    normal = dtv_list_enclosures(&error);
    if (!normal || !normal[0] || normal[0] == root || normal[1] ||
        error.code != DTV_ERROR_OK || dtv_get_status(root) != DTV_ENCLOSURE_DISCONNECTED) return 17;
    root = normal[0];
    fd_test_reset();
    fd_test_reinserted(1);
    selected = dtv_list_enclosures_by_fds(fds, 2, &error);
    if (!selected || selected[0] != root || selected[1] ||
        error.code != DTV_ERROR_OK || dtv_get_status(root) != DTV_ENCLOSURE_READY) return 10;
    fd_test_reset();
    fd_test_reinserted(1);
    fd_test_duplicate_second(1);
    if (dtv_list_enclosures_by_fds(fds, 2, &error) != NULL ||
        error.code != DTV_ERROR_INVALID_ARGUMENT || dtv_get_status(root) != DTV_ENCLOSURE_READY) return 18;
    if (dtv_list_enclosures_by_fds(NULL, 1, &error) != NULL ||
        error.code != DTV_ERROR_INVALID_ARGUMENT) return 8;
    close(first[0]); close(first[1]); close(second[0]); close(second[1]);
    return 0;
}
