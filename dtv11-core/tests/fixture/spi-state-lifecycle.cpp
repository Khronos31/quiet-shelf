// SPDX-License-Identifier: GPL-2.0-or-later
#include <dtv/spi/device_ops.hpp>

namespace dtv::spi {

extern const ModelEntry fixture_one_model;
uint32_t fixture_one_destroy_count_for_test();

}

int main()
{
    using namespace dtv::spi;

    const auto destroy_count = fixture_one_destroy_count_for_test();
    {
        ProductInstance first{fixture_one_model, 11};
        ProductInstance second{fixture_one_model, 12};
        if (first.result().code != 0 || second.result().code != 0 ||
            !first.context() || !second.context() || first.context() == second.context()) {
            return 1;
        }

        dtv_identify_info info{};
        const auto result = fixture_one_model.firmware[0].ops->identify(
            first.context(), &info, {});
        if (result.code != 0 || info.firmware_key != 11) {
            return 2;
        }
    }

    return fixture_one_destroy_count_for_test() == destroy_count + 2 ? 0 : 3;
}
