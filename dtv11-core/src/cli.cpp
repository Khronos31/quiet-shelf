// SPDX-License-Identifier: GPL-2.0-or-later
#include <dtv/cli.h>

#include <cstdio>
#include <cstring>

namespace {

int print_help(const dtv_cli_config *config)
{
    if (std::printf("Usage: %s [--help] [--version] [--models]\n",
                    config->command_name) < 0 ||
        std::printf("Show product metadata. Other operations are not implemented yet.\n") < 0 ||
        std::fflush(stdout) != 0) {
        return 70;
    }
    return 0;
}

int print_models(dtv_model_provider model_at, size_t model_count)
{
    for (size_t i = 0; i < model_count; ++i) {
        const dtv_model_info *model = nullptr;
        try {
            model = model_at(i);
        } catch (...) {
            std::fprintf(stderr, "model provider failed\n");
            return 70;
        }
        if (model == nullptr || model->name == nullptr) {
            std::fprintf(stderr, "invalid model descriptor\n");
            return 70;
        }
        if (std::printf("%s %04x:%04x\n",
                        model->name,
                        static_cast<unsigned int>(model->vendor_id),
                        static_cast<unsigned int>(model->product_id)) < 0) {
            return 70;
        }
    }
    return std::fflush(stdout) == 0 ? 0 : 70;
}

bool has_argument(int argc, const char *const argv[], const char *argument)
{
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], argument) == 0) {
            return true;
        }
    }
    return false;
}

}

extern "C" uint32_t dtv_api_version(void)
{
    return 1;
}

extern "C" int dtv_cli_main(int argc,
                            const char *const argv[],
                            const dtv_cli_config *config,
                            dtv_model_provider model_at,
                            size_t model_count)
{
    if (argc < 1 || argv == nullptr || config == nullptr ||
        config->product_name == nullptr || config->command_name == nullptr ||
        config->version == nullptr || config->default_model == nullptr ||
        (model_count != 0 && model_at == nullptr)) {
        std::fprintf(stderr, "invalid CLI configuration\n");
        return 70;
    }

    const bool help = has_argument(argc, argv, "--help") ||
                      has_argument(argc, argv, "-h");
    const bool version = has_argument(argc, argv, "--version");
    const bool model_list = has_argument(argc, argv, "--models");

    if (help) {
        return print_help(config);
    }
    if (version) {
        if (std::printf("%s %s\n", config->product_name, config->version) < 0 ||
            std::fflush(stdout) != 0) {
            return 70;
        }
        return 0;
    }
    if (model_list) {
        if (argc != 2) {
            std::fprintf(stderr, "%s: --models cannot be combined with other arguments\n",
                         config->command_name);
            return 2;
        }
        return print_models(model_at, model_count);
    }

    std::fprintf(stderr, "%s: unknown argument\n", config->command_name);
    return 2;
}
