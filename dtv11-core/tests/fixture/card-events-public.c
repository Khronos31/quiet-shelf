/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <dtv/dtv.h>
#include <stddef.h>

int dtv_public_test_receiver_transition(dtv_enclosure *);
void dtv_public_test_set_tune_context(dtv_enclosure *, dtv_receiver *, dtv_receiver *);
void dtv_public_test_set_tune_mode(uint32_t);

typedef int (*card_event_driver)(void *, uint32_t);

static int wait_event(dtv_event *event, dtv_enclosure *root, uint32_t kind,
                      uint32_t timeout_ms)
{
    dtv_call_options call = {sizeof(dtv_call_options), timeout_ms};
    dtv_error error = {0};
    error.struct_size = sizeof(error);
    if (dtv_wait_event(event, &call, &error) != DTV_ERROR_OK ||
        error.code != DTV_ERROR_OK || event->kind != kind ||
        event->enclosure != root || event->receiver != NULL) {
        return 0;
    }
    return 1;
}

static int wait_receiver_event(dtv_event *event, dtv_enclosure *root,
                               dtv_receiver *receiver, uint32_t kind)
{
    dtv_call_options call = {sizeof(dtv_call_options), 1000};
    dtv_error error = {0};
    error.struct_size = sizeof(error);
    return dtv_wait_event(event, &call, &error) == DTV_ERROR_OK &&
        error.code == DTV_ERROR_OK && event->kind == kind &&
        event->enclosure == root && event->receiver == receiver;
}

static int wait_connected_event(dtv_enclosure *root)
{
    uint32_t attempt;
    for (attempt = 0; attempt < 128; ++attempt) {
        dtv_event event = {0};
        dtv_call_options call = {sizeof(dtv_call_options), 50};
        dtv_error error = {0};
        error.struct_size = sizeof(error);
        if (dtv_wait_event(&event, &call, &error) != DTV_ERROR_OK ||
            error.code != DTV_ERROR_OK) return 0;
        if (event.kind == DTV_EVENT_USB_CONNECTED && event.enclosure == root &&
            event.receiver == NULL) return 1;
    }
    return 0;
}

static int expect_timeout(void)
{
    dtv_event event = {0};
    dtv_call_options call = {sizeof(dtv_call_options), 25};
    dtv_error error = {0};
    error.struct_size = sizeof(error);
    return dtv_wait_event(&event, &call, &error) == DTV_ERROR_TIMEOUT &&
        error.code == DTV_ERROR_TIMEOUT;
}

static int expect_signal(dtv_receiver *receiver, uint32_t state, uint32_t reason, int32_t value)
{
    dtv_signal signal = {0};
    dtv_error error = {0};
    error.struct_size = sizeof(error);
    return dtv_get_signal(receiver, NULL, &signal, &error) == DTV_ERROR_OK &&
        error.code == DTV_ERROR_OK && signal.struct_size == sizeof(signal) &&
        signal.lock.state == state && signal.lock.reason == reason && signal.lock.value == value &&
        signal.cnr_mdb.state == DTV_MEASUREMENT_UNSUPPORTED &&
        signal.cnr_mdb.reason == DTV_MEASUREMENT_REASON_NONE &&
        signal.signal_mdbm.state == DTV_MEASUREMENT_UNSUPPORTED &&
        signal.signal_mdbm.reason == DTV_MEASUREMENT_REASON_NONE;
}

static int drive(card_event_driver driver, void *user, uint32_t step)
{
    return driver != NULL && driver(user, step) == 0;
}

int card_events_public_consumer(card_event_driver driver, void *user)
{
    dtv_error error = {0};
    error.struct_size = sizeof(error);
    dtv_enclosure *const *enclosures = dtv_list_enclosures(&error);
    if (enclosures == NULL || enclosures[0] == NULL) return 1;
    dtv_enclosure *root = enclosures[0];
    dtv_hold_enclosure(root);
    dtv_device *const *devices = dtv_get_devices(root);
    if (devices == NULL || devices[0] == NULL ||
        dtv_get_card_state(root) != DTV_CARD_UNKNOWN ||
        dtv_get_card_generation(root) != 0 ||
        dtv_get_card_reader_device(root) != NULL) return 2;
    dtv_receiver *const *receivers = dtv_get_receivers(devices[0]);
    dtv_receiver *const *resolved_receivers = NULL;
    if (receivers == NULL || receivers[0] == NULL || receivers[1] == NULL ||
        receivers[2] == NULL || receivers[3] != NULL ||
        dtv_get_index(receivers[0]) != 0 || dtv_get_index(receivers[1]) != 1 ||
        dtv_get_index(receivers[2]) != 2 || dtv_get_local(receivers[1]) != 1 ||
        dtv_get_systems(receivers[0]) != (1u << DTV_SYSTEM_ISDB_T) ||
        dtv_get_systems(receivers[1]) != (1u << DTV_SYSTEM_ISDB_S)) return 15;
    {
        uint32_t support = DTV_UNKNOWN;
        dtv_signal signal = {0};
        dtv_device *const *next_device = devices + 1;
        if (next_device[0] == NULL || dtv_get_receivers(next_device[0]) != NULL ||
            dtv_get_receiver_support(receivers[1], DTV_OP_TUNE_ISDB_S, &support, &error) !=
                DTV_ERROR_OK || support != DTV_YES ||
            dtv_get_receiver_support(receivers[1], DTV_OP_SIGNAL, &support, &error) !=
                DTV_ERROR_OK || support != DTV_YES) return 16;
        if (dtv_get_signal(receivers[0], NULL, &signal, &error) != DTV_ERROR_NOT_READY ||
            error.code != DTV_ERROR_NOT_READY ||
            dtv_tune_by_frequency(receivers[2], DTV_SYSTEM_ISDB_T, 5931428571, NULL,
                NULL, &error) != DTV_ERROR_UNSUPPORTED || error.code != DTV_ERROR_UNSUPPORTED ||
            dtv_get_signal(receivers[2], NULL, &signal, &error) != DTV_ERROR_UNSUPPORTED ||
            error.code != DTV_ERROR_UNSUPPORTED) return 21;
        if (dtv_public_test_receiver_transition(root) != 0) return 17;
        resolved_receivers = dtv_get_receivers(next_device[0]);
        if (resolved_receivers == NULL || resolved_receivers[0] != NULL ||
            dtv_get_receivers(devices[0]) != receivers) return 18;
    }

    dtv_event event = {0};
    if (!wait_connected_event(root)) return 3;
    if (dtv_open_enclosure(root, NULL, &error) != DTV_ERROR_OK ||
        error.code != DTV_ERROR_OK) return 4;
    if (!expect_signal(receivers[0], DTV_MEASUREMENT_UNKNOWN,
                       DTV_MEASUREMENT_REASON_NOT_SAMPLED, 0) ||
        !expect_signal(receivers[1], DTV_MEASUREMENT_UNKNOWN,
                       DTV_MEASUREMENT_REASON_NOT_SAMPLED, 0)) return 22;
    dtv_device *reader = dtv_get_card_reader_device(root);
    if (dtv_get_card_state(root) != DTV_CARD_ABSENT ||
        dtv_get_card_generation(root) != 0 || reader != devices[0]) return 5;

    if (!drive(driver, user, 9) || !drive(driver, user, 10) || !expect_timeout() ||
        !expect_signal(receivers[0], DTV_MEASUREMENT_VALID,
                       DTV_MEASUREMENT_REASON_NONE, 0) ||
        !expect_signal(receivers[1], DTV_MEASUREMENT_VALID,
                       DTV_MEASUREMENT_REASON_NONE, 1)) return 19;
    if (!drive(driver, user, 1) || !expect_timeout() ||
        dtv_get_card_state(root) != DTV_CARD_ABSENT ||
        dtv_get_card_generation(root) != 0) return 6;
    if (!drive(driver, user, 2) ||
        !wait_event(&event, root, DTV_EVENT_CARD_INSERTED, 1000) ||
        dtv_get_card_state(root) != DTV_CARD_PRESENT ||
        dtv_get_card_generation(root) != 1 ||
        dtv_get_card_reader_device(root) != reader) return 7;

    enclosures = dtv_list_enclosures(&error);
    if (enclosures == NULL || enclosures[0] != root ||
        dtv_get_card_reader_device(root) != reader || dtv_get_receivers(devices[0]) != receivers ||
        dtv_get_receivers(devices[1]) != resolved_receivers ||
        dtv_get_receivers(devices[1])[0] != NULL)
        return 8;
    if (!drive(driver, user, 3) || !expect_timeout() ||
        dtv_get_card_state(root) != DTV_CARD_PRESENT ||
        dtv_get_card_generation(root) != 1) return 9;
    if (!drive(driver, user, 11) || !drive(driver, user, 4) ||
        !wait_receiver_event(&event, root, receivers[0], DTV_EVENT_LOCK_ACQUIRED) ||
        !expect_signal(receivers[0], DTV_MEASUREMENT_VALID,
                       DTV_MEASUREMENT_REASON_NONE, 1) ||
        !expect_signal(receivers[1], DTV_MEASUREMENT_VALID,
                       DTV_MEASUREMENT_REASON_NONE, 1) ||
        !wait_event(&event, root, DTV_EVENT_CARD_REMOVED, 1000) ||
        dtv_get_card_state(root) != DTV_CARD_ABSENT ||
        dtv_get_card_generation(root) != 2) return 10;

    if (!drive(driver, user, 12) ||
        !wait_receiver_event(&event, root, receivers[1], DTV_EVENT_LOCK_LOST) ||
        !expect_signal(receivers[0], DTV_MEASUREMENT_VALID,
                       DTV_MEASUREMENT_REASON_NONE, 1) ||
        !expect_signal(receivers[1], DTV_MEASUREMENT_VALID,
                       DTV_MEASUREMENT_REASON_NONE, 0) ||
        !drive(driver, user, 22) ||
        !wait_receiver_event(&event, root, receivers[1], DTV_EVENT_LOCK_ACQUIRED) ||
        !expect_signal(receivers[1], DTV_MEASUREMENT_VALID,
                       DTV_MEASUREMENT_REASON_NONE, 1)) return 20;
    dtv_public_test_set_tune_context(root, receivers[0], receivers[1]);
    {
        dtv_tune_options tune = {sizeof(dtv_tune_options), DTV_TUNE_SELECTOR_TSID,
                                 0x1234, DTV_LNA_ON};
        dtv_call_options call = {sizeof(dtv_call_options), 1000};
        if (dtv_tune_by_frequency(receivers[0], DTV_SYSTEM_ISDB_T, UINT64_C(5931428571),
                &tune, &call, &error) != DTV_ERROR_OK || error.code != DTV_ERROR_OK ||
            !expect_signal(receivers[0], DTV_MEASUREMENT_UNKNOWN,
                           DTV_MEASUREMENT_REASON_TUNE_CHANGED, 0) ||
            !expect_signal(receivers[1], DTV_MEASUREMENT_VALID,
                           DTV_MEASUREMENT_REASON_NONE, 1) || !expect_timeout()) return 23;
        if (!drive(driver, user, 19) || !expect_timeout() ||
            !expect_signal(receivers[0], DTV_MEASUREMENT_UNKNOWN,
                           DTV_MEASUREMENT_REASON_TUNE_CHANGED, 0)) return 24;
        if (!drive(driver, user, 20) ||
            !wait_receiver_event(&event, root, receivers[0], DTV_EVENT_LOCK_LOST) ||
            !expect_signal(receivers[0], DTV_MEASUREMENT_VALID,
                           DTV_MEASUREMENT_REASON_NONE, 0)) return 25;

        tune.selector = DTV_TUNE_SELECTOR_SLOT;
        tune.value = 1;
        tune.lna = DTV_LNA_OFF;
        if (dtv_tune_by_frequency(receivers[1], DTV_SYSTEM_ISDB_S, UINT64_C(11700000000),
                &tune, &call, &error) != DTV_ERROR_OK || error.code != DTV_ERROR_OK ||
            !expect_signal(receivers[1], DTV_MEASUREMENT_UNKNOWN,
                           DTV_MEASUREMENT_REASON_TUNE_CHANGED, 0) ||
            !drive(driver, user, 21) || !expect_timeout() ||
            !expect_signal(receivers[1], DTV_MEASUREMENT_VALID,
                           DTV_MEASUREMENT_REASON_NONE, 1)) return 26;
        tune.value = 2;
        if (dtv_tune_by_frequency(receivers[1], DTV_SYSTEM_ISDB_S, UINT64_C(11700000000),
                &tune, &call, &error) != DTV_ERROR_UNSUPPORTED ||
            error.code != DTV_ERROR_UNSUPPORTED ||
            !expect_signal(receivers[1], DTV_MEASUREMENT_VALID,
                           DTV_MEASUREMENT_REASON_NONE, 1) || !expect_timeout()) return 27;
        tune.value = 12;
        if (dtv_tune_by_frequency(receivers[1], DTV_SYSTEM_ISDB_S, UINT64_C(11700000000),
                &tune, &call, &error) != DTV_ERROR_INVALID_ARGUMENT ||
            error.code != DTV_ERROR_INVALID_ARGUMENT ||
            !expect_signal(receivers[1], DTV_MEASUREMENT_VALID,
                           DTV_MEASUREMENT_REASON_NONE, 1) || !expect_timeout()) return 33;

        tune.selector = DTV_TUNE_SELECTOR_TSID;
        tune.value = 0x1234;
        tune.lna = DTV_LNA_ON;
        dtv_public_test_set_tune_mode(1);
        if (dtv_tune_by_frequency(receivers[0], DTV_SYSTEM_ISDB_T, UINT64_C(5931428571),
                &tune, &call, &error) != DTV_ERROR_UNSUPPORTED ||
            error.code != DTV_ERROR_UNSUPPORTED ||
            !expect_signal(receivers[0], DTV_MEASUREMENT_VALID,
                           DTV_MEASUREMENT_REASON_NONE, 0) || !expect_timeout()) return 28;
        dtv_public_test_set_tune_mode(2);
        if (dtv_tune_by_frequency(receivers[0], DTV_SYSTEM_ISDB_T, UINT64_C(5931428571),
                &tune, &call, &error) != DTV_ERROR_USB_IO || error.code != DTV_ERROR_USB_IO ||
            !expect_signal(receivers[0], DTV_MEASUREMENT_UNKNOWN,
                           DTV_MEASUREMENT_REASON_TUNE_CHANGED, 0)) return 29;
        dtv_public_test_set_tune_mode(3);
        if (dtv_tune_by_frequency(receivers[0], DTV_SYSTEM_ISDB_T, UINT64_C(5931428571),
                &tune, &call, &error) != DTV_ERROR_CANCELLED || error.code != DTV_ERROR_CANCELLED ||
            !expect_signal(receivers[0], DTV_MEASUREMENT_UNKNOWN,
                           DTV_MEASUREMENT_REASON_TUNE_CHANGED, 0)) return 30;
        dtv_public_test_set_tune_mode(4);
        call.timeout_ms = 1;
        if (dtv_tune_by_frequency(receivers[0], DTV_SYSTEM_ISDB_T, UINT64_C(5931428571),
                &tune, &call, &error) != DTV_ERROR_TIMEOUT || error.code != DTV_ERROR_TIMEOUT ||
            !expect_signal(receivers[0], DTV_MEASUREMENT_UNKNOWN,
                           DTV_MEASUREMENT_REASON_TUNE_CHANGED, 0)) return 31;
        dtv_public_test_set_tune_mode(0);
        if (!drive(driver, user, 23) ||
            !wait_receiver_event(&event, root, receivers[0], DTV_EVENT_LOCK_ACQUIRED) ||
            !expect_signal(receivers[0], DTV_MEASUREMENT_VALID,
                           DTV_MEASUREMENT_REASON_NONE, 1)) return 32;
        call.timeout_ms = 1000;

        if (dtv_tune_by_channel(receivers[0], "T12", NULL, &call, &error) !=
                DTV_ERROR_INVALID_ARGUMENT || error.code != DTV_ERROR_INVALID_ARGUMENT ||
            dtv_tune_by_channel(receivers[0], "BS02", NULL, &call, &error) !=
                DTV_ERROR_INVALID_ARGUMENT || error.code != DTV_ERROR_INVALID_ARGUMENT ||
            dtv_tune_by_channel(receivers[0], "CS03", NULL, &call, &error) !=
                DTV_ERROR_INVALID_ARGUMENT || error.code != DTV_ERROR_INVALID_ARGUMENT ||
            dtv_tune_by_channel(receivers[0], "BS01", NULL, &call, &error) !=
                DTV_ERROR_INVALID_ARGUMENT || error.code != DTV_ERROR_INVALID_ARGUMENT ||
            !expect_signal(receivers[0], DTV_MEASUREMENT_VALID,
                           DTV_MEASUREMENT_REASON_NONE, 1) || !expect_timeout()) return 34;

        tune.selector = DTV_TUNE_SELECTOR_SLOT;
        tune.value = 0;
        tune.lna = DTV_LNA_DEFAULT;
        if (dtv_tune_by_channel(receivers[0], "BS01_0", &tune, &call, &error) !=
                DTV_ERROR_INVALID_ARGUMENT || error.code != DTV_ERROR_INVALID_ARGUMENT ||
            dtv_tune_by_channel(receivers[0], "13", &tune, &call, &error) !=
                DTV_ERROR_INVALID_ARGUMENT || error.code != DTV_ERROR_INVALID_ARGUMENT ||
            !expect_signal(receivers[0], DTV_MEASUREMENT_VALID,
                           DTV_MEASUREMENT_REASON_NONE, 1) || !expect_timeout()) return 35;
        tune.value = 12;
        if (dtv_tune_by_channel(receivers[1], "CS2", &tune, &call, &error) !=
                DTV_ERROR_INVALID_ARGUMENT || error.code != DTV_ERROR_INVALID_ARGUMENT ||
            !expect_signal(receivers[1], DTV_MEASUREMENT_VALID,
                           DTV_MEASUREMENT_REASON_NONE, 1) || !expect_timeout()) return 36;
        tune.selector = DTV_TUNE_SELECTOR_NONE;
        tune.value = 0;
        if (dtv_tune_by_channel(receivers[1], "BS23_11", NULL, &call, &error) !=
                DTV_ERROR_UNSUPPORTED || error.code != DTV_ERROR_UNSUPPORTED ||
            !expect_signal(receivers[1], DTV_MEASUREMENT_VALID,
                           DTV_MEASUREMENT_REASON_NONE, 1) || !expect_timeout()) return 37;

        tune.lna = DTV_LNA_ON;
        if (dtv_tune_by_channel(receivers[0], "T13", &tune, &call, &error) != DTV_ERROR_OK ||
            error.code != DTV_ERROR_OK ||
            !expect_signal(receivers[0], DTV_MEASUREMENT_UNKNOWN,
                           DTV_MEASUREMENT_REASON_TUNE_CHANGED, 0) ||
            !drive(driver, user, 19) || !expect_timeout() ||
            !expect_signal(receivers[0], DTV_MEASUREMENT_UNKNOWN,
                           DTV_MEASUREMENT_REASON_TUNE_CHANGED, 0)) return 38;
        tune.lna = DTV_LNA_OFF;
        if (dtv_tune_by_channel(receivers[0], "13", NULL, &call, &error) != DTV_ERROR_OK ||
            error.code != DTV_ERROR_OK ||
            !expect_signal(receivers[0], DTV_MEASUREMENT_UNKNOWN,
                           DTV_MEASUREMENT_REASON_TUNE_CHANGED, 0) ||
            dtv_tune_by_channel(receivers[0], "T62", &tune, &call, &error) != DTV_ERROR_OK ||
            error.code != DTV_ERROR_OK ||
            !expect_signal(receivers[0], DTV_MEASUREMENT_UNKNOWN,
                           DTV_MEASUREMENT_REASON_TUNE_CHANGED, 0)) return 39;

        tune.lna = DTV_LNA_DEFAULT;
        if (dtv_tune_by_channel(receivers[1], "BS01_0", NULL, &call, &error) != DTV_ERROR_OK ||
            error.code != DTV_ERROR_OK ||
            !expect_signal(receivers[1], DTV_MEASUREMENT_UNKNOWN,
                           DTV_MEASUREMENT_REASON_TUNE_CHANGED, 0)) return 40;
        tune.selector = DTV_TUNE_SELECTOR_TSID;
        tune.value = 0x1234;
        tune.lna = DTV_LNA_ON;
        if (dtv_tune_by_channel(receivers[1], "BS01", &tune, &call, &error) != DTV_ERROR_OK ||
            error.code != DTV_ERROR_OK ||
            !expect_signal(receivers[1], DTV_MEASUREMENT_UNKNOWN,
                           DTV_MEASUREMENT_REASON_TUNE_CHANGED, 0)) return 41;
        if (dtv_tune_by_channel(receivers[1], "BS23", &tune, &call, &error) != DTV_ERROR_OK ||
            error.code != DTV_ERROR_OK ||
            !expect_signal(receivers[1], DTV_MEASUREMENT_UNKNOWN,
                           DTV_MEASUREMENT_REASON_TUNE_CHANGED, 0)) return 45;
        if (dtv_tune_by_channel(receivers[1], "CS2", NULL, &call, &error) != DTV_ERROR_OK ||
            error.code != DTV_ERROR_OK ||
            !expect_signal(receivers[1], DTV_MEASUREMENT_UNKNOWN,
                           DTV_MEASUREMENT_REASON_TUNE_CHANGED, 0)) return 42;
        tune.selector = DTV_TUNE_SELECTOR_SLOT;
        tune.value = 0;
        tune.lna = DTV_LNA_OFF;
        if (dtv_tune_by_channel(receivers[1], "CS08", &tune, &call, &error) != DTV_ERROR_OK ||
            error.code != DTV_ERROR_OK ||
            !expect_signal(receivers[1], DTV_MEASUREMENT_UNKNOWN,
                           DTV_MEASUREMENT_REASON_TUNE_CHANGED, 0) ||
            !drive(driver, user, 23) || !expect_timeout() ||
            !expect_signal(receivers[0], DTV_MEASUREMENT_VALID,
                           DTV_MEASUREMENT_REASON_NONE, 1) ||
            !drive(driver, user, 21) || !expect_timeout() ||
            !expect_signal(receivers[1], DTV_MEASUREMENT_VALID,
                           DTV_MEASUREMENT_REASON_NONE, 1)) return 43;

        if (dtv_tune_by_channel(receivers[1], "CS24", NULL, &call, &error) != DTV_ERROR_OK ||
            error.code != DTV_ERROR_OK ||
            !expect_signal(receivers[1], DTV_MEASUREMENT_UNKNOWN,
                           DTV_MEASUREMENT_REASON_TUNE_CHANGED, 0) ||
            !drive(driver, user, 12) ||
            !wait_receiver_event(&event, root, receivers[1], DTV_EVENT_LOCK_LOST) ||
            !expect_signal(receivers[1], DTV_MEASUREMENT_VALID,
                           DTV_MEASUREMENT_REASON_NONE, 0) || !expect_timeout()) return 44;
    }
    if (!drive(driver, user, 13) || !expect_timeout() ||
        !drive(driver, user, 14) || !expect_timeout() ||
        !drive(driver, user, 15) || !expect_timeout() ||
        !drive(driver, user, 17) || !expect_timeout() ||
        !drive(driver, user, 18) || !expect_timeout()) return 20;

    if (!drive(driver, user, 5) || !drive(driver, user, 16) || !drive(driver, user, 6) ||
        !wait_event(&event, root, DTV_EVENT_CARD_INSERTED, 1000) ||
        dtv_get_card_state(root) != DTV_CARD_PRESENT ||
        dtv_get_card_generation(root) != 3 ||
        !wait_receiver_event(&event, root, receivers[0], DTV_EVENT_LOCK_LOST)) return 11;
    if (dtv_wait_event(&event, NULL, &error) != DTV_ERROR_RESOURCE_LIMIT ||
        error.code != DTV_ERROR_RESOURCE_LIMIT ||
        dtv_wait_event(&event, NULL, &error) != DTV_ERROR_RESOURCE_LIMIT ||
        error.code != DTV_ERROR_RESOURCE_LIMIT ||
        !expect_signal(receivers[0], DTV_MEASUREMENT_VALID,
                       DTV_MEASUREMENT_REASON_NONE, 1)) return 12;

    dtv_call_options short_close = {sizeof(dtv_call_options), 1};
    if (!drive(driver, user, 7) ||
        dtv_close_enclosure(root, &short_close, &error) != DTV_CLOSE_PENDING ||
        error.code != DTV_ERROR_OK ||
        dtv_get_signal(receivers[0], NULL, &(dtv_signal){0}, &error) != DTV_ERROR_NOT_READY ||
        error.code != DTV_ERROR_NOT_READY ||
        dtv_get_enclosure_close_state(root, &error) != DTV_CLOSE_PENDING ||
        error.code != DTV_ERROR_OK || !drive(driver, user, 8)) return 13;
    {
        uint32_t attempt;
        for (attempt = 0; attempt < 2000; ++attempt) {
            if (dtv_get_enclosure_close_state(root, &error) == DTV_CLOSE_CLOSED) break;
        }
        if (attempt == 2000 || error.code != DTV_ERROR_OK) return 14;
    }
    if (dtv_close_enclosure(root, NULL, &error) != DTV_CLOSE_CLOSED ||
        error.code != DTV_ERROR_OK) return 13;
    dtv_public_test_set_tune_context(NULL, NULL, NULL);
    dtv_release_enclosure(root);
    dtv_drop_enclosure(root);
    return 0;
}
