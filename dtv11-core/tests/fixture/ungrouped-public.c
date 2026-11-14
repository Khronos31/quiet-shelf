/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdint.h>
#include <string.h>

#include <dtv/dtv.h>
#include <fixture_api.h>
#include <scanfixture_api.h>

void dtv_ungrouped_test_batch(unsigned mode);
void dtv_ungrouped_test_scan_error(unsigned fail);
void dtv_ungrouped_test_fail_allocation(unsigned fail);

static uint32_t count_devices(const dtv_ungrouped_device *const *devices, uint32_t limit)
{
    for (uint32_t count = 0; count < limit; ++count) {
        if (devices[count] == NULL) return count;
    }
    return UINT32_MAX;
}

static int empty_success(void)
{
    const dtv_ungrouped_device *const *devices = dtv_list_ungrouped_devices();
    return devices != NULL && devices[0] == NULL && dtv_get_scan_complete() == 1;
}

static int scan_fixture_entry(const dtv_ungrouped_device *entry, uint8_t bus,
                              uint8_t address, const char *port)
{
    return entry != NULL && entry->struct_size == sizeof(*entry) && entry->serial == NULL &&
           entry->model == NULL &&
           entry->port != NULL && strcmp(entry->port, port) == 0 &&
           entry->vid == 0x1234 && entry->pid == 0x0201 && entry->bus != NULL &&
           *entry->bus == bus && entry->address != NULL && *entry->address == address &&
           entry->status == DTV_UNGROUPED_UNKNOWN;
}

static int unique_scan_fixture_entry(const dtv_ungrouped_device *entry)
{
    return entry != NULL && entry->model != NULL &&
           strcmp(entry->model, "Scan Fixture") == 0 && entry->pid == 0x0202;
}

int main(void)
{
    dtv_load_scanfixture();
    dtv_event event;
    dtv_call_options timeout = {sizeof(timeout), 1};

    dtv_ungrouped_test_batch(5);
    if (!empty_success()) return 1;
    dtv_ungrouped_test_batch(6);
    if (!empty_success()) return 2;
    dtv_load_fixture();
    dtv_ungrouped_test_batch(6);
    const dtv_ungrouped_device *const *devices = dtv_list_ungrouped_devices();
    if (devices == NULL || count_devices(devices, 3) != 1 || devices[0]->model == NULL ||
        strcmp(devices[0]->model, "Fixture One") != 0) return 3;

    dtv_ungrouped_test_batch(7);
    devices = dtv_list_ungrouped_devices();
    if (devices == NULL || count_devices(devices, 3) != 1 || devices[0]->vid != 0x1234 ||
        devices[0]->pid != 0x0001) return 4;

    dtv_ungrouped_test_batch(8);
    devices = dtv_list_ungrouped_devices();
    if (devices == NULL || count_devices(devices, dtv_get_list_capacity() + 2) !=
                               dtv_get_list_capacity() + 1) return 5;

    dtv_ungrouped_test_batch(0);
    devices = dtv_list_ungrouped_devices();
    if (devices == NULL || count_devices(devices, 3) != 1 ||
        !scan_fixture_entry(devices[0], 1, 7, "1-1.7")) return 6;

    dtv_ungrouped_test_batch(10);
    devices = dtv_list_ungrouped_devices();
    if (devices == NULL || count_devices(devices, 3) != 1 ||
        !unique_scan_fixture_entry(devices[0])) return 24;

    dtv_ungrouped_test_batch(0);
    devices = dtv_list_ungrouped_devices();
    if (devices == NULL || count_devices(devices, 3) != 1 ||
        !scan_fixture_entry(devices[0], 1, 7, "1-1.7")) return 25;
    const dtv_ungrouped_device *old = devices[0];
    dtv_error error = {0};
    error.struct_size = sizeof(error);
    dtv_enclosure *const *common = dtv_list_enclosures(&error);
    dtv_enclosure *const *product = dtv_list_enclosures_scanfixture(&error);
    if (common == NULL || product == NULL || error.code != DTV_ERROR_OK ||
        common[0] != NULL || product[0] != NULL) return 26;
    devices = dtv_list_ungrouped_devices();
    if (devices == NULL || devices[0] != old) return 27;
    dtv_ungrouped_test_batch(1);
    devices = dtv_list_ungrouped_devices();
    if (devices == NULL || devices[0] != old) return 7;
    if (dtv_wait_event(&event, &timeout, NULL) != DTV_ERROR_TIMEOUT) return 23;
    devices = dtv_list_ungrouped_devices();
    if (devices == NULL || devices[0] != old) return 28;

    dtv_ungrouped_test_batch(2);
    dtv_ungrouped_test_fail_allocation(1);
    if (dtv_list_ungrouped_devices() != NULL || dtv_get_scan_complete() != 0 ||
        !scan_fixture_entry(old, 1, 7, "1-1.7")) return 8;
    dtv_ungrouped_test_fail_allocation(0);
    devices = dtv_list_ungrouped_devices();
    if (devices == NULL || count_devices(devices, 3) != 1 || devices[0] == old ||
        !scan_fixture_entry(devices[0], 2, 8, "2-2.8") ||
        !scan_fixture_entry(old, 1, 7, "1-1.7")) return 9;

    const dtv_ungrouped_device *changed = devices[0];
    dtv_ungrouped_test_scan_error(1);
    if (dtv_list_ungrouped_devices() != NULL || dtv_get_scan_complete() != 0 ||
        !scan_fixture_entry(changed, 2, 8, "2-2.8")) return 10;
    dtv_ungrouped_test_scan_error(0);
    devices = dtv_list_ungrouped_devices();
    if (devices == NULL || devices[0] != changed) return 11;

    dtv_ungrouped_test_batch(4);
    devices = dtv_list_ungrouped_devices();
    if (devices == NULL || devices[0] != NULL ||
        !scan_fixture_entry(changed, 2, 8, "2-2.8")) return 12;
    if (dtv_wait_event(&event, &timeout, NULL) != DTV_ERROR_TIMEOUT) return 13;

    dtv_ungrouped_test_batch(9);
    devices = dtv_list_ungrouped_devices();
    if (devices == NULL || count_devices(devices, 3) != 1 ||
        !scan_fixture_entry(devices[0], 1, 7, "1-1.7")) return 14;
    const dtv_ungrouped_device *reinserted = devices[0];

    dtv_ungrouped_test_batch(3);
    devices = dtv_list_ungrouped_devices();
    if (devices == NULL || devices[0] != NULL ||
        !scan_fixture_entry(reinserted, 1, 7, "1-1.7")) return 15;

    common = dtv_list_enclosures(&error);
    product = dtv_list_enclosures_scanfixture(&error);
    dtv_enclosure *root = NULL;
    for (uint32_t index = 0; common != NULL && index < dtv_get_list_capacity(); ++index) {
        if (common[index] != NULL) root = common[index];
    }
    if (root == NULL || product == NULL || error.code != DTV_ERROR_OK) return 16;
    uint32_t index = 0;
    while (index < dtv_get_list_capacity() && product[index] == NULL) ++index;
    if (index == dtv_get_list_capacity() || product[index] != root) return 17;

    if (dtv_wait_event(&event, &timeout, &error) != DTV_ERROR_OK ||
        event.kind != DTV_EVENT_USB_CONNECTED || event.enclosure != root) return 18;
    if (dtv_wait_event(&event, &timeout, &error) != DTV_ERROR_TIMEOUT) return 19;
    dtv_hold_enclosure(root);
    dtv_ungrouped_test_batch(4);
    devices = dtv_list_ungrouped_devices();
    if (devices == NULL || devices[0] != NULL ||
        dtv_get_status(root) != DTV_ENCLOSURE_DISCONNECTED) return 20;
    if (dtv_wait_event(&event, &timeout, &error) != DTV_ERROR_OK ||
        event.kind != DTV_EVENT_USB_DISCONNECTED || event.enclosure != root) return 21;
    if (dtv_wait_event(&event, &timeout, &error) != DTV_ERROR_TIMEOUT) return 22;
    dtv_drop_enclosure(root);
    return 0;
}
