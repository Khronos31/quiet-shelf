// SPDX-License-Identifier: GPL-2.0-only
#include <host_ctl.h>

#if defined(_WIN32)
#include <px4_windows_args.h>

#include <string>
#include <vector>
#endif

namespace {

using dtv::userland::host_ctl::Profile;
using dtv::userland::identity::Product;

// Torne has no serial descriptor (i50 = 0) and this product is ISDB-T only.
// Card and LNB commands stay unsupported until a backend exists.
const Profile kProfile{
    "tornectl",
    "torne-userland",
    TORNE_USERLAND_PRODUCT_VERSION,
    Product::torne,
    false,
    false,
    nullptr,
    nullptr,
    false,
    false,
};

}  // namespace

int main(int argc, char** argv)
{
#if defined(_WIN32)
    const std::vector<std::string> owned_argv =
        px4::userland::cli::windows_argv_utf8(argc, argv);
    std::vector<const char*> argv_views;
    argv_views.reserve(owned_argv.size());
    for (const std::string& value : owned_argv) argv_views.push_back(value.c_str());
    return dtv::userland::host_ctl::run_control_cli(
        static_cast<int>(argv_views.size()), argv_views.data(), kProfile);
#else
    return dtv::userland::host_ctl::run_control_cli(
        argc, const_cast<const char* const*>(argv), kProfile);
#endif
}
