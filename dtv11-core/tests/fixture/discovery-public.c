/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdint.h>
#include <string.h>

#include <dtv/dtv.h>

void dtv_public_test_batch(unsigned mode);
void dtv_public_test_fail_journal(unsigned fail);
void dtv_load_scanfixture(void);

static void init_error(dtv_error *error)
{
    memset(error, 0, sizeof(*error));
    error->struct_size = sizeof(*error);
}

int main(void)
{
    dtv_load_scanfixture();
    if (dtv_get_list_capacity() != DTV_MAX_ENCLOSURES) return 1;
    dtv_public_test_batch(0);
    dtv_error error;
    init_error(&error);
    dtv_enclosure *const *slots = dtv_list_enclosures(&error);
    if (slots == NULL || error.code != DTV_ERROR_OK || dtv_get_scan_complete() != 1) return 2;
    dtv_enclosure *first = slots[0];
    if (first == NULL || slots[1] != NULL || dtv_get_enclosure_id(first) == 0 ||
        dtv_get_product(first) == NULL || strcmp(dtv_get_product(first), "scanfixture") != 0 ||
        dtv_get_model(first) == NULL || strcmp(dtv_get_model(first), "Scan Fixture") != 0 ||
        dtv_get_status(first) != DTV_ENCLOSURE_READY ||
        dtv_get_enclosure_serial(first) != NULL || dtv_get_serial_unique(first) != DTV_UNKNOWN ||
        dtv_get_lnb_state(first) != DTV_LNB_UNKNOWN || dtv_get_card_state(first) != DTV_CARD_UNKNOWN ||
        dtv_get_card_generation(first) != 0 || dtv_get_card_reader_device(first) != NULL) return 3;
    dtv_device *const *devices = dtv_get_devices(first);
    const dtv_candidate *const *candidates = dtv_get_candidates(first);
    if (devices == NULL || devices[0] == NULL || devices[1] != NULL ||
        candidates == NULL || candidates[0] == NULL || candidates[1] != NULL) return 4;
    dtv_device *device = devices[0];
    dtv_receiver *const *receivers = dtv_get_receivers(device);
    if (receivers == NULL || receivers[0] == NULL || dtv_get_index(receivers[0]) != 0 ||
        dtv_get_local(receivers[0]) != 0 || dtv_get_systems(receivers[0]) !=
            (1u << DTV_SYSTEM_ISDB_T) || dtv_get_ts_scrambling(receivers[0]) != DTV_TS_DESCRAMBLED)
        return 17;
    dtv_receiver *held_receiver = receivers[0];
    uint32_t support = DTV_UNKNOWN;
    if (dtv_get_receiver_support(held_receiver, DTV_OP_TUNE_ISDB_T, &support, &error) !=
            DTV_ERROR_OK || support != DTV_YES ||
        dtv_get_receiver_support(held_receiver, DTV_OP_SELECT_TSID, &support, &error) !=
            DTV_ERROR_OK || support != DTV_YES ||
        dtv_get_receiver_support(held_receiver, DTV_OP_LNB_POWER, &support, &error) !=
            DTV_ERROR_OK || support != DTV_UNKNOWN) return 18;
    const uint32_t *bus = dtv_get_bus(device);
    const uint32_t *address = dtv_get_address(device);
    if (dtv_get_device_id(device) == 0 || dtv_get_vid(device) != 0x1234 ||
        dtv_get_pid(device) != 0x0101 || dtv_get_device_serial(device) != NULL ||
        bus == NULL || *bus != 1 || address == NULL || *address != 7 ||
        dtv_get_port(device) == NULL || strcmp(dtv_get_port(device), "1-1.7") != 0 ||
        dtv_get_mode(device) != DTV_DEVICE_UNKNOWN || dtv_get_receivers(device) != receivers ||
        candidates[0]->device != 0 || candidates[0]->bus == NULL || *candidates[0]->bus != 1 ||
        candidates[0]->address == NULL || *candidates[0]->address != 7) return 5;

    dtv_hold_enclosure(first);
    dtv_public_test_batch(0);
    init_error(&error);
    slots = dtv_list_enclosures(&error);
    if (slots == NULL || slots[0] != first || error.code != DTV_ERROR_OK ||
        dtv_get_receivers(device) != receivers || dtv_get_index(held_receiver) != 0) return 6;
    dtv_public_test_batch(1);
    init_error(&error);
    slots = dtv_list_enclosures(&error);
    if (slots == NULL || slots[0] != NULL || error.code != DTV_ERROR_OK ||
        dtv_get_status(first) != DTV_ENCLOSURE_DISCONNECTED || dtv_get_vid(device) != 0x1234 ||
        dtv_get_receivers(device) != receivers || dtv_get_index(held_receiver) != 0) return 7;
    dtv_public_test_batch(2);
    init_error(&error);
    slots = dtv_list_enclosures(&error);
    dtv_enclosure *second = slots == NULL ? NULL : slots[0];
    if (second == NULL || second == first || dtv_get_status(first) != DTV_ENCLOSURE_DISCONNECTED ||
        error.code != DTV_ERROR_OK) return 8;
    dtv_device *const *new_devices = dtv_get_devices(second);
    dtv_receiver *const *new_receivers = new_devices == NULL ? NULL : dtv_get_receivers(new_devices[0]);
    if (new_receivers == NULL || new_receivers[0] == NULL || new_receivers[0] == held_receiver ||
        dtv_get_index(new_receivers[0]) != 0) return 19;

    dtv_event event;
    dtv_call_options timeout = {sizeof(timeout), 1};
    if (dtv_wait_event(&event, &timeout, &error) != DTV_ERROR_OK ||
        event.kind != DTV_EVENT_USB_CONNECTED || event.enclosure != first) return 9;
    if (dtv_wait_event(&event, &timeout, &error) != DTV_ERROR_OK ||
        event.kind != DTV_EVENT_USB_DISCONNECTED || event.enclosure != first) return 10;
    if (dtv_wait_event(&event, &timeout, &error) != DTV_ERROR_OK ||
        event.kind != DTV_EVENT_USB_CONNECTED || event.enclosure != second) return 11;
    dtv_drop_enclosure(first);
    if (dtv_wait_event(&event, &timeout, &error) != DTV_ERROR_TIMEOUT ||
        error.code != DTV_ERROR_TIMEOUT) return 12;

    dtv_public_test_batch(1);
    init_error(&error);
    slots = dtv_list_enclosures(&error);
    if (slots == NULL || slots[0] != NULL || error.code != DTV_ERROR_OK) return 13;
    dtv_public_test_batch(3);
    dtv_public_test_fail_journal(1);
    init_error(&error);
    if (dtv_list_enclosures(&error) != NULL || error.code != DTV_ERROR_RESOURCE_LIMIT ||
        dtv_get_status(second) != DTV_ENCLOSURE_DISCONNECTED) return 14;
    if (dtv_wait_event(&event, &timeout, &error) != DTV_ERROR_OK ||
        event.kind != DTV_EVENT_USB_DISCONNECTED || event.enclosure != second) return 15;
    if (dtv_wait_event(&event, &timeout, &error) != DTV_ERROR_RESOURCE_LIMIT ||
        error.code != DTV_ERROR_RESOURCE_LIMIT) return 16;
    return 0;
}
