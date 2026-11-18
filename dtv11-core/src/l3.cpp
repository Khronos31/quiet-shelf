// SPDX-License-Identifier: GPL-2.0-or-later
#include <dtv/dtv.h>
#include <dtv/spi/feature.hpp>
#include <dtv/internal/discovery.hpp>
#include <dtv/internal/l3.hpp>
#include <dtv/internal/result.hpp>
#include <dtv/spi/device_ops.hpp>

#include <algorithm>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace {

std::mutex products_mutex;
struct LoadedProduct {
    std::string name;
    const dtv::spi::l3_model_record *const *models;
    std::size_t model_count;
    const dtv::spi::ProductEntry *discovery_product;
    int32_t (*send_to_all)(const dtv_call_options *, dtv_error *);
};
std::vector<LoadedProduct> loaded_products;
std::deque<dtv_model> model_storage;
std::vector<const dtv_model *> model_list;

void set_error(dtv_error *error, int32_t code)
{
    if (error != nullptr && error->struct_size >= offsetof(dtv_error, code) + sizeof(error->code)) {
        error->code = code;
    }
}

bool valid_operation(uint32_t operation)
{
    return operation >= DTV_OP_LNB_POWER && operation <= DTV_OP_LNA;
}

const dtv::spi::l3_model_record *find_model_record(
    const char *product_name, const char *model_name, int32_t &result, dtv_error *error)
{
    if (model_name == nullptr) {
        result = DTV_ERROR_NOT_READY;
        set_error(error, result);
        return nullptr;
    }
    if (product_name == nullptr) {
        result = DTV_ERROR_INVALID_ARGUMENT;
        set_error(error, result);
        return nullptr;
    }
    std::lock_guard lock(products_mutex);
    const auto product = std::find_if(loaded_products.begin(), loaded_products.end(),
                                      [product_name](const auto &loaded) {
                                          return loaded.name == product_name;
                                      });
    if (product == loaded_products.end()) {
        result = DTV_ERROR_NOT_FOUND;
        set_error(error, result);
        return nullptr;
    }
    for (std::size_t index = 0; index < product->model_count; ++index) {
        const auto *model = product->models[index];
        if (model != nullptr && model->model != nullptr && model->model->name != nullptr &&
            std::string(model->model->name) == model_name) {
            result = DTV_ERROR_OK;
            return model;
        }
    }
    result = DTV_ERROR_NOT_FOUND;
    set_error(error, result);
    return nullptr;
}

bool model_supports(const dtv::spi::l3_model_record &model, uint32_t operation)
{
    dtv::spi::Feature feature;
    switch (operation) {
    case DTV_OP_LNB_POWER: feature = dtv::spi::Feature::LnbPower; break;
    case DTV_OP_CARD: feature = dtv::spi::Feature::Card; break;
    case DTV_OP_OUTPUT_PID_FILTER: feature = dtv::spi::Feature::OutputPidFilter; break;
    case DTV_OP_TUNE_ISDB_T: feature = dtv::spi::Feature::TuneIsdbT; break;
    case DTV_OP_TUNE_ISDB_S: feature = dtv::spi::Feature::TuneIsdbS; break;
    case DTV_OP_SELECT_SLOT: feature = dtv::spi::Feature::SelectSlot; break;
    case DTV_OP_SELECT_TSID: feature = dtv::spi::Feature::SelectTsid; break;
    case DTV_OP_SIGNAL: feature = dtv::spi::Feature::Signal; break;
    case DTV_OP_LNA: feature = dtv::spi::Feature::Lna; break;
    default: return false;
    }
    return (model.features & (std::uint32_t{1} << static_cast<std::uint32_t>(feature))) != 0;
}

}

extern "C" uint32_t dtv_get_api_version(void)
{
    return UINT32_C(0x00020000);
}

extern "C" const dtv_model *const *dtv_list_models(void)
{
    std::lock_guard lock(products_mutex);
    if (model_list.empty()) model_list.push_back(nullptr);
    return model_list.data();
}

extern "C" void dtv_l3_register_product(
    const char *product_name, const dtv::spi::l3_model_record *const *models,
    std::size_t model_count, const dtv::spi::ProductEntry *discovery_product,
    int32_t (*send_to_all)(const dtv_call_options *, dtv_error *))
{
    if (product_name == nullptr || models == nullptr || discovery_product == nullptr ||
        send_to_all == nullptr || discovery_product->product_id == nullptr ||
        std::strcmp(product_name, discovery_product->product_id) != 0 ||
        discovery_product->model_count != model_count ||
        (model_count != 0 && discovery_product->models == nullptr)) return;
    for (std::size_t index = 0; index < model_count; ++index) {
        if (models[index] == nullptr || models[index]->discovery == nullptr ||
            discovery_product->models[index] != models[index]->discovery) return;
    }
    std::lock_guard lock(products_mutex);
    const auto found = std::find_if(loaded_products.begin(), loaded_products.end(),
                                    [product_name](const auto &loaded) {
                                        return loaded.name == product_name;
                                    });
    if (found == loaded_products.end()) {
        loaded_products.push_back({product_name, models, model_count, discovery_product, send_to_all});
        for (std::size_t index = 0; index < model_count; ++index) {
            if (models[index] != nullptr && models[index]->model != nullptr) {
                model_storage.push_back(*models[index]->model);
            }
        }
        model_list.clear();
        for (const auto &model : model_storage) model_list.push_back(&model);
        model_list.push_back(nullptr);
    }
}

extern "C" int32_t dtv_get_product_support(
    const char *product_name, uint32_t operation, uint32_t *out_tribool, dtv_error *error)
{
    if (product_name == nullptr || out_tribool == nullptr || !valid_operation(operation)) {
        set_error(error, DTV_ERROR_INVALID_ARGUMENT);
        return DTV_ERROR_INVALID_ARGUMENT;
    }
    std::lock_guard lock(products_mutex);
    const auto product = std::find_if(loaded_products.begin(), loaded_products.end(),
                                      [product_name](const auto &loaded) {
                                          return loaded.name == product_name;
                                      });
    if (product == loaded_products.end()) {
        set_error(error, DTV_ERROR_NOT_FOUND);
        return DTV_ERROR_NOT_FOUND;
    }
    bool any_yes = false;
    bool any_no = false;
    for (std::size_t index = 0; index < product->model_count; ++index) {
        if (product->models[index] != nullptr && model_supports(*product->models[index], operation)) {
            any_yes = true;
        } else {
            any_no = true;
        }
    }
    *out_tribool = any_yes && any_no ? DTV_UNKNOWN : any_yes ? DTV_YES : DTV_NO;
    return DTV_ERROR_OK;
}

extern "C" int32_t dtv_get_model_support(
    const char *model_name, uint32_t operation, uint32_t *out_tribool, dtv_error *error)
{
    if (model_name == nullptr || out_tribool == nullptr || !valid_operation(operation)) {
        set_error(error, DTV_ERROR_INVALID_ARGUMENT);
        return DTV_ERROR_INVALID_ARGUMENT;
    }
    std::lock_guard lock(products_mutex);
    for (const auto &product : loaded_products) {
        for (std::size_t index = 0; index < product.model_count; ++index) {
            const auto &model = *product.models[index];
            if (model.model != nullptr && model.model->name != nullptr &&
                std::string(model.model->name) == model_name) {
                *out_tribool = model_supports(model, operation) ? DTV_YES : DTV_NO;
                return DTV_ERROR_OK;
            }
        }
    }
    set_error(error, DTV_ERROR_NOT_FOUND);
    return DTV_ERROR_NOT_FOUND;
}

extern "C" int32_t dtv_send_default_firmware_to_all(
    const dtv_call_options *options, dtv_error *error)
{
    std::vector<int32_t (*)(const dtv_call_options *, dtv_error *)> send_functions;
    {
        std::lock_guard lock(products_mutex);
        for (const auto &product : loaded_products) send_functions.push_back(product.send_to_all);
    }
    for (const auto send_to_all : send_functions) {
        const auto result = send_to_all(options, error);
        if (result != DTV_ERROR_OK) return result;
    }
    return DTV_ERROR_OK;
}

namespace dtv::internal {
namespace {

const dtv::spi::model_operation_table *operation_table(
    const char *product, const char *model, int32_t &result, dtv_error *error)
{
    const auto *record = find_model_record(product, model, result, error);
    if (record == nullptr) return nullptr;
    if (record->operations == nullptr) {
        result = DTV_ERROR_UNSUPPORTED;
        set_error(error, result);
        return nullptr;
    }
    return record->operations;
}

}

int32_t dispatch_lnb_on(const char *product, const char *model, dtv_enclosure *enclosure,
                        const dtv_call_options *options, dtv_error *error)
{
    int32_t result = DTV_ERROR_OK;
    const auto *operations = operation_table(product, model, result, error);
    return operations == nullptr ? result : operations->lnb_on(enclosure, options, error);
}

int32_t dispatch_lnb_off(const char *product, const char *model, dtv_enclosure *enclosure,
                         const dtv_call_options *options, dtv_error *error)
{
    int32_t result = DTV_ERROR_OK;
    const auto *operations = operation_table(product, model, result, error);
    return operations == nullptr ? result : operations->lnb_off(enclosure, options, error);
}

int32_t dispatch_tune_by_frequency(const char *product, const char *model, dtv_receiver *receiver,
                                  uint32_t system, uint64_t frequency_hz,
                                  const dtv_tune_options *tune, const dtv_call_options *options,
                                  dtv_error *error)
{
    int32_t result = DTV_ERROR_OK;
    const auto *operations = operation_table(product, model, result, error);
    return operations == nullptr ? result : operations->tune_by_frequency(
        receiver, system, frequency_hz, tune, options, error);
}

int32_t dispatch_get_signal(const char *product, const char *model, dtv_receiver *receiver,
                            const dtv_call_options *options, dtv_signal *out, dtv_error *error)
{
    int32_t result = DTV_ERROR_OK;
    const auto *operations = operation_table(product, model, result, error);
    return operations == nullptr ? result : operations->get_signal(receiver, options, out, error);
}

int32_t dispatch_set_pid_filter(const char *product, const char *model, dtv_stream *stream,
                                const uint16_t *pids, uint32_t count, dtv_error *error)
{
    int32_t result = DTV_ERROR_OK;
    const auto *operations = operation_table(product, model, result, error);
    return operations == nullptr ? result : operations->set_pid_filter(stream, pids, count, error);
}

int32_t dispatch_send_firmware(const char *product, const char *model, dtv_device *device,
                               const uint8_t *firmware, uint64_t size,
                               const dtv_call_options *options, dtv_error *error)
{
    int32_t result = DTV_ERROR_OK;
    const auto *operations = operation_table(product, model, result, error);
    return operations == nullptr ? result : operations->send_firmware(
        device, firmware, size, options, error);
}

int32_t dispatch_send_default_firmware(const char *product, const char *model, dtv_device *device,
                                       const dtv_call_options *options, dtv_error *error)
{
    int32_t result = DTV_ERROR_OK;
    const auto *operations = operation_table(product, model, result, error);
    return operations == nullptr ? result : operations->send_default_firmware(device, options, error);
}

int32_t dispatch_reset_card(const char *product, const char *model, dtv_enclosure *enclosure,
                            const dtv_call_options *options, std::vector<std::uint8_t> *out,
                            dtv_error *error)
{
    int32_t result = DTV_ERROR_OK;
    const auto *operations = operation_table(product, model, result, error);
    return operations == nullptr ? result : operations->reset_card(enclosure, options, out, error);
}

}

namespace dtv::internal {

dtv_internal_result observe_loaded_product_groups(
    std::vector<UsbDeviceSnapshot> snapshots, CompleteGroupBatch *out)
{
    if (out == nullptr) {
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_INVALID_ARGUMENT, 0);
    }
    out->snapshots.clear();
    out->groups.clear();
    out->ungrouped.clear();
    try {
        std::vector<LoadedProductDiscoveryView> products;
        {
            std::lock_guard lock(products_mutex);
            products.reserve(loaded_products.size());
            for (const auto &loaded : loaded_products) {
                products.push_back({loaded.discovery_product, loaded.models, loaded.model_count});
            }
        }
        return collect_complete_groups(products, std::move(snapshots), out);
    } catch (...) {
        out->snapshots.clear();
        out->groups.clear();
        out->ungrouped.clear();
        return exception_result();
    }
}

dtv_internal_result scan_loaded_product_groups(CompleteGroupBatch *out)
{
    if (out == nullptr) {
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_INVALID_ARGUMENT, 0);
    }
    out->snapshots.clear();
    out->groups.clear();
    out->ungrouped.clear();
    try {
        auto context = UsbContext::scan_only();
        return scan_loaded_product_groups(context, out);
    } catch (...) {
        out->snapshots.clear();
        out->groups.clear();
        out->ungrouped.clear();
        return exception_result();
    }
}

dtv_internal_result scan_loaded_product_groups(const UsbContext &context,
                                                CompleteGroupBatch *out)
{
    return scan_loaded_product_groups(context, out, nullptr, nullptr);
}

dtv_internal_result scan_loaded_product_groups(const UsbContext &context,
                                                CompleteGroupBatch *out,
                                                dtv_token_of_callback token_callback,
                                                void *token_user)
{
    if (out == nullptr) {
        return dtv_internal_result_make(DTV_INTERNAL_RESULT_INVALID_ARGUMENT, 0);
    }
    out->snapshots.clear();
    out->groups.clear();
    out->ungrouped.clear();
    try {
        auto result = context.result();
        if (result.code != DTV_INTERNAL_RESULT_OK) {
            return result;
        }
        auto snapshots = context.enumerate(&result, token_callback, token_user);
        if (result.code != DTV_INTERNAL_RESULT_OK) {
            return result;
        }
        return observe_loaded_product_groups(std::move(snapshots), out);
    } catch (...) {
        out->snapshots.clear();
        out->groups.clear();
        out->ungrouped.clear();
        return exception_result();
    }
}

}
