// SPDX-License-Identifier: GPL-2.0-or-later
#include <dtv/spi/device_ops.hpp>

#include <dtv/internal/result.hpp>

namespace dtv::spi {

ProductInstance::ProductInstance(const ModelEntry &model, dtv_device_id device) : model_(&model)
{
    if ((model_->create_state == nullptr) != (model_->destroy_state == nullptr)) {
        result_ = dtv_internal_result_make(DTV_INTERNAL_RESULT_INVALID_ARGUMENT, 0);
        return;
    }
    if (model_->create_state) {
        dtv_context *temporary = nullptr;
        result_ = dtv::internal::call_callback([&] {
            return model_->create_state(device, &temporary);
        });
        if (result_.code == DTV_INTERNAL_RESULT_OK) {
            context_ = temporary;
        } else if (temporary != nullptr) {
            destroy(temporary);
        }
    }
}

ProductInstance::~ProductInstance() { reset(); }

void ProductInstance::reset() noexcept
{
    auto *context = context_;
    context_ = nullptr;
    if (context && model_->destroy_state) {
        destroy(context);
    }
}

void ProductInstance::destroy(dtv_context *context) noexcept
{
    if (model_->destroy_state) {
        try {
            model_->destroy_state(context);
        } catch (...) {
        }
    }
}

}
