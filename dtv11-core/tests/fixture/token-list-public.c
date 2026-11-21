/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <dtv/dtv.h>
#include <scanfixture_api.h>

void token_test_install_backend(void);
void token_test_set_devices(unsigned mode);
void token_test_set_duplicate_tokens(unsigned value);
void token_test_callback_seen(struct libusb_device *device);
unsigned token_test_callback_count(void);
int token_test_callback_is_current_thread(void);
dtv_usb_token token_test_token_for(struct libusb_device *device);
void token_test_fail_allocation(unsigned fail);
void token_test_fail_journal(unsigned fail);
void dtv_load_scanfixture(void);

static int callback_user;

static dtv_usb_token token_of(void *user, struct libusb_device *device)
{
    if (user != &callback_user) return 0;
    token_test_callback_seen(device);
    return token_test_token_for(device);
}

static void init_error(dtv_error *error, char *message, uint64_t capacity)
{
    memset(error, 0, sizeof(*error));
    error->struct_size = sizeof(*error);
    error->message = message;
    error->message_capacity = capacity;
    if (capacity != 0) message[0] = '\0';
}

int main(int argc, char **argv)
{
    token_test_install_backend();
    dtv_load_scanfixture();
    dtv_usb_token both[] = {11, 22};
    dtv_error error;
    char message[48];
    init_error(&error, message, sizeof(message));

    if (argc > 1 && strcmp(argv[1], "unconfigured") == 0) {
        if (dtv_list_enclosures_by_tokens(both, 2, &error) != NULL ||
            error.code != DTV_ERROR_INVALID_ARGUMENT || token_test_callback_count() != 0) return 1;
        return 0;
    }

    dtv_set_token_of(token_of, &callback_user);
    if (dtv_list_enclosures_by_tokens(NULL, 1, &error) != NULL ||
        error.code != DTV_ERROR_INVALID_ARGUMENT || token_test_callback_count() != 0) return 2;
    dtv_usb_token zero[] = {0};
    if (dtv_list_enclosures_by_tokens(zero, 1, &error) != NULL ||
        error.code != DTV_ERROR_INVALID_ARGUMENT || token_test_callback_count() != 0) return 3;
    dtv_usb_token repeated[] = {11, 11};
    if (dtv_list_enclosures_by_tokens(repeated, 2, &error) != NULL ||
        error.code != DTV_ERROR_INVALID_ARGUMENT || token_test_callback_count() != 0) return 4;

    dtv_enclosure *const *normal = dtv_list_enclosures(&error);
    if (normal == NULL || normal[0] == NULL || normal[1] != NULL ||
        error.code != DTV_ERROR_OK || token_test_callback_count() != 0) return 5;
    if (dtv_list_ungrouped_devices() == NULL || token_test_callback_count() != 0) return 23;
    dtv_enclosure *root = normal[0];
    dtv_hold_enclosure(root);

    dtv_enclosure *const *selected = dtv_list_enclosures_by_tokens(both, 2, &error);
    if (selected == NULL || selected[0] != root || selected[1] != NULL ||
        error.code != DTV_ERROR_OK || token_test_callback_count() != 2 ||
        !token_test_callback_is_current_thread()) return 6;
    const unsigned callbacks = token_test_callback_count();
    dtv_usb_token reverse[] = {22, 11};
    selected = dtv_list_enclosures_by_tokens(reverse, 2, &error);
    if (selected == NULL || selected[0] != root || token_test_callback_count() != callbacks + 2) return 7;
    selected = dtv_list_enclosures_by_tokens(NULL, 0, &error);
    if (selected == NULL || selected[0] != NULL || selected[1] != NULL ||
        token_test_callback_count() != callbacks + 4) return 19;
    selected = dtv_list_enclosures_by_tokens(both, 1, &error);
    if (selected == NULL || selected[0] != NULL || selected[1] != NULL ||
        dtv_get_status(root) != DTV_ENCLOSURE_READY) return 8;
    selected = dtv_list_enclosures_by_tokens_scanfixture(both, 2, &error);
    if (selected == NULL || selected[0] != root || selected[1] != NULL) return 9;
    normal = dtv_list_enclosures(&error);
    if (normal == NULL || normal[0] != root || token_test_callback_count() != callbacks + 8) return 10;

    dtv_usb_token missing[] = {99};
    dtv_enclosure *const *stable_token_view = dtv_list_enclosures_by_tokens(both, 2, &error);
    if (stable_token_view == NULL || stable_token_view[0] != root) return 22;
    if (dtv_list_enclosures_by_tokens(missing, 1, &error) != NULL ||
        error.code != DTV_ERROR_NOT_FOUND || dtv_get_status(root) != DTV_ENCLOSURE_READY ||
        stable_token_view[0] != root) return 11;
    token_test_set_duplicate_tokens(1);
    struct {
        char message[6];
        char guard;
    } short_message;
    memset(&short_message, 0xa5, sizeof(short_message));
    error.message = short_message.message;
    error.message_capacity = sizeof(short_message.message);
    if (dtv_list_enclosures_by_tokens(both, 2, &error) != NULL ||
        error.code != DTV_ERROR_INVALID_ARGUMENT || short_message.guard != (char)0xa5 ||
        short_message.message[sizeof(short_message.message) - 1] != '\0' ||
        dtv_get_status(root) != DTV_ENCLOSURE_READY || stable_token_view[0] != root) return 12;
    token_test_set_duplicate_tokens(0);

    token_test_fail_allocation(1);
    if (dtv_list_enclosures_by_tokens(both, 2, &error) != NULL ||
        error.code != DTV_ERROR_RESOURCE_LIMIT || dtv_get_status(root) != DTV_ENCLOSURE_READY ||
        stable_token_view[0] != root) return 13;
    token_test_fail_allocation(0);
    selected = dtv_list_enclosures_by_tokens(both, 2, &error);
    if (selected == NULL || selected != stable_token_view || selected[0] != root ||
        error.code != DTV_ERROR_OK) return 14;
    stable_token_view = selected;

    token_test_set_devices(4);
    if (dtv_list_enclosures_by_tokens(both, 2, &error) != NULL ||
        error.code != DTV_ERROR_USB_IO || dtv_get_status(root) != DTV_ENCLOSURE_READY ||
        stable_token_view[0] != root || dtv_get_scan_complete() != 0) return 24;
    token_test_set_devices(0);
    selected = dtv_list_enclosures_by_tokens(both, 2, &error);
    if (selected == NULL || selected != stable_token_view || selected[0] != root ||
        error.code != DTV_ERROR_OK) return 25;
    stable_token_view = selected;

    dtv_event event;
    dtv_call_options timeout = {sizeof(timeout), 1};
    const unsigned before_wait = token_test_callback_count();
    if (dtv_wait_event(&event, &timeout, &error) != DTV_ERROR_OK ||
        event.kind != DTV_EVENT_USB_CONNECTED || event.enclosure != root ||
        token_test_callback_count() != before_wait) return 15;

    token_test_set_devices(1);
    selected = dtv_list_enclosures_by_tokens(both, 2, &error);
    if (selected != NULL || error.code != DTV_ERROR_NOT_FOUND ||
        dtv_get_status(root) != DTV_ENCLOSURE_READY || stable_token_view[0] != root) return 16;
    normal = dtv_list_enclosures(&error);
    if (normal == NULL || normal[0] != NULL || dtv_get_status(root) != DTV_ENCLOSURE_DISCONNECTED) return 17;
    if (dtv_wait_event(&event, &timeout, &error) != DTV_ERROR_OK ||
        event.kind != DTV_EVENT_USB_DISCONNECTED || event.enclosure != root) return 20;
    token_test_set_devices(2);
    selected = dtv_list_enclosures_by_tokens(both, 2, &error);
    if (selected == NULL || selected[0] == NULL || selected[0] == root ||
        dtv_get_status(root) != DTV_ENCLOSURE_DISCONNECTED) return 18;
    dtv_enclosure *new_root = selected[0];
    token_test_set_devices(1);
    token_test_fail_journal(1);
    if (dtv_list_enclosures_by_tokens(both, 1, &error) != NULL ||
        error.code != DTV_ERROR_RESOURCE_LIMIT ||
        dtv_get_status(new_root) != DTV_ENCLOSURE_READY || stable_token_view[0] != new_root) return 26;
    if (dtv_wait_event(&event, &timeout, &error) != DTV_ERROR_OK ||
        event.kind != DTV_EVENT_USB_CONNECTED || event.enclosure != new_root) return 21;
    dtv_drop_enclosure(root);
    return 0;
}
