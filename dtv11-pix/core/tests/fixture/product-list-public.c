/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdint.h>
#include <string.h>

#include <dtv/dtv.h>
#include <i0_asicen_fixture_api.h>
#include <i0_torne_fixture_api.h>

void dtv_product_list_test_batch(unsigned mode);
void dtv_product_list_test_fail_journal(unsigned fail);
void dtv_product_list_test_scan_error(unsigned fail);
dtv_enclosure *const *dtv_product_list_test_call_other_thread(unsigned product,
                                                               int32_t *out_error);

static void init_error(dtv_error *error)
{
    memset(error, 0, sizeof(*error));
    error->struct_size = sizeof(*error);
}

static int projection_matches(dtv_enclosure *const *common,
                              dtv_enclosure *const *product, const char *name,
                              uint32_t capacity)
{
    for (uint32_t index = 0; index < capacity; ++index) {
        dtv_enclosure *expected = common[index] != NULL &&
                                          strcmp(dtv_get_product(common[index]), name) == 0
                                      ? common[index]
                                      : NULL;
        if (product[index] != expected) return 0;
    }
    return 1;
}

static dtv_enclosure *find_product(dtv_enclosure *const *slots, uint32_t capacity,
                                   const char *name)
{
    for (uint32_t index = 0; index < capacity; ++index) {
        if (slots[index] != NULL && strcmp(dtv_get_product(slots[index]), name) == 0) {
            return slots[index];
        }
    }
    return NULL;
}

int main(void)
{
    const char *asicen_name = "i0_asicen_fixture";
    const char *torne_name = "i0_torne_fixture";
    dtv_load_i0_asicen_fixture();
    dtv_load_i0_torne_fixture();
    dtv_product_list_test_batch(0);

    dtv_error error;
    init_error(&error);
    dtv_enclosure *const *common = dtv_list_enclosures(&error);
    const uint32_t capacity = dtv_get_list_capacity();
    if (common == NULL || error.code != DTV_ERROR_OK || dtv_get_scan_complete() != 1) return 1;
    init_error(&error);
    dtv_enclosure *const *asicen = dtv_list_enclosures_i0_asicen_fixture(&error);
    if (asicen == NULL || error.code != DTV_ERROR_OK ||
        !projection_matches(common, asicen, asicen_name, capacity)) return 2;
    init_error(&error);
    dtv_enclosure *const *torne = dtv_list_enclosures_i0_torne_fixture(&error);
    if (torne == NULL || error.code != DTV_ERROR_OK ||
        !projection_matches(common, torne, torne_name, capacity)) return 3;
    if (dtv_list_enclosures_i0_asicen_fixture(&error) != asicen ||
        dtv_list_enclosures_i0_torne_fixture(&error) != torne) return 4;
    int32_t other_error = DTV_ERROR_INTERNAL;
    if (dtv_product_list_test_call_other_thread(0, &other_error) != asicen ||
        other_error != DTV_ERROR_OK) return 18;

    dtv_enclosure *old_asicen = find_product(common, capacity, asicen_name);
    dtv_enclosure *old_torne = find_product(common, capacity, torne_name);
    if (old_asicen == NULL || old_torne == NULL || old_asicen == old_torne) return 5;
    dtv_hold_enclosure(old_torne);

    dtv_event event;
    dtv_call_options timeout = {sizeof(timeout), 1};
    unsigned asicen_events = 0;
    unsigned torne_events = 0;
    for (unsigned index = 0; index < 2; ++index) {
        if (dtv_wait_event(&event, &timeout, &error) != DTV_ERROR_OK ||
            event.kind != DTV_EVENT_USB_CONNECTED) return 6;
        const char *product = dtv_get_product(event.enclosure);
        if (product == NULL) return 7;
        if (strcmp(product, asicen_name) == 0 && event.enclosure == old_asicen) ++asicen_events;
        else if (strcmp(product, torne_name) == 0 && event.enclosure == old_torne) ++torne_events;
        else return 8;
    }
    if (asicen_events != 1 || torne_events != 1 ||
        dtv_wait_event(&event, &timeout, &error) != DTV_ERROR_TIMEOUT) return 9;

    dtv_product_list_test_scan_error(1);
    init_error(&error);
    if (dtv_list_enclosures_i0_asicen_fixture(&error) != NULL ||
        error.code != DTV_ERROR_USB_IO || dtv_get_scan_complete() != 0 ||
        find_product(asicen, capacity, asicen_name) != old_asicen ||
        find_product(torne, capacity, torne_name) != old_torne) return 10;
    dtv_product_list_test_scan_error(0);

    dtv_product_list_test_batch(2);
    init_error(&error);
    asicen = dtv_list_enclosures_i0_asicen_fixture(&error);
    if (asicen == NULL || error.code != DTV_ERROR_OK ||
        asicen != dtv_list_enclosures_i0_asicen_fixture(&error)) return 11;
    init_error(&error);
    torne = dtv_list_enclosures_i0_torne_fixture(&error);
    if (torne == NULL || error.code != DTV_ERROR_OK ||
        !projection_matches(common, torne, torne_name, capacity)) return 12;
    if (dtv_get_status(old_torne) != DTV_ENCLOSURE_DISCONNECTED) return 13;
    if (dtv_wait_event(&event, &timeout, &error) != DTV_ERROR_OK ||
        event.kind != DTV_EVENT_USB_DISCONNECTED || event.enclosure != old_torne) return 14;
    if (dtv_wait_event(&event, &timeout, &error) != DTV_ERROR_TIMEOUT) return 15;
    dtv_drop_enclosure(old_torne);

    dtv_product_list_test_fail_journal(1);
    dtv_product_list_test_batch(1);
    init_error(&error);
    if (dtv_list_enclosures_i0_asicen_fixture(&error) != NULL ||
        error.code != DTV_ERROR_RESOURCE_LIMIT ||
        find_product(asicen, capacity, asicen_name) != old_asicen) return 16;
    if (dtv_wait_event(&event, &timeout, &error) != DTV_ERROR_RESOURCE_LIMIT ||
        error.code != DTV_ERROR_RESOURCE_LIMIT) return 17;
    return 0;
}
