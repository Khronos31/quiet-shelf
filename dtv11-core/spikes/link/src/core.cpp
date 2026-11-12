// SPDX-License-Identifier: GPL-2.0-or-later
#include "link_spike.hpp"

#include <cstdio>
#include <cstring>

namespace dtv::link_spike {
namespace {

const ProductEntry *loaded_product;

}

Result unsupported()
{
    return Result::unsupported;
}

void register_product(const ProductEntry *product)
{
    loaded_product = product;
}

int cli_main(int argc, char **argv, const CliConfig *config)
{
    if (loaded_product == nullptr || config == nullptr) {
        return 70;
    }
    if (argc == 2 && std::strcmp(argv[1], "--models") == 0) {
        std::printf("product=%s command=%s version=%s model=%s firmware=%s\n",
                    config->product, config->command, config->version,
                    loaded_product->model->model, loaded_product->model->firmware);
        return 0;
    }
    if (argc == 2 && std::strcmp(argv[1], "--link-spike") == 0) {
        const Result firmware_result = loaded_product->model->send_default_firmware();
        const Result signal_result = loaded_product->model->get_signal();
        std::printf("send_default_firmware=%s get_signal=%s\n",
                    firmware_result == Result::ok ? "OK" : "ERROR",
                    signal_result == Result::unsupported ? "UNSUPPORTED" : "IMPLEMENTED");
        return firmware_result == Result::ok && signal_result == Result::unsupported ? 0 : 1;
    }
    std::printf("usage: %s --models | --link-spike\n", config->command);
    return 0;
}

}
