// SPDX-License-Identifier: GPL-2.0-only
#include <dtv/host_ts/px4_ts_core.h>
#include <dtv/userland/host_facade/px4d_signals.h>
#include <dtv/userland/identity.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <thread>

#if defined(_WIN32)
#include <dtv/host_ts/px4_ts_windows_output.h>
#include <px4_windows_args.h>

#include <string>
#include <vector>
#else
#include <dtv/host_ts/px4_ts_posix.h>

#include <csignal>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace {

using namespace px4::userland;
using namespace px4::userland::cli;

// Serial selection is unsupported because the device has no serial descriptor.
// -f still requires --system; the product rejects ISDB-S after parsing.
const TsClientProfile kProfile{
    "torne-ts",
    "torne-userland",
    dtv::userland::identity::Product::torne,
    false,
    true,
    nullptr,
    false,
};

class SteadyClock final : public Px4TsClock {
public:
    std::uint64_t monotonic_ms() noexcept override
    {
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    void sleep_ms(std::uint32_t milliseconds) noexcept override
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
    }
};

class StopSignal final : public Px4TsSignal {
public:
    bool stop_requested() const noexcept override
    {
        return px4::userland::px4d::stop_requested();
    }
};

bool install_signals() noexcept
{
#if !defined(_WIN32)
    struct sigaction ignore_pipe {};
    ignore_pipe.sa_handler = SIG_IGN;
    if ((::sigemptyset)(&ignore_pipe.sa_mask) != 0 ||
        (::sigaction)(SIGPIPE, &ignore_pipe, nullptr) != 0)
        return false;
#endif
    return px4::userland::px4d::install_signal_handlers();
}

}  // namespace

int main(int argc, char** argv)
{
#if defined(_WIN32)
    const std::vector<std::string> owned_argv =
        px4::userland::cli::windows_argv_utf8(argc, argv);
    std::vector<const char*> argv_views;
    argv_views.reserve(owned_argv.size());
    for (const std::string& value : owned_argv) argv_views.push_back(value.c_str());
    Px4TsArguments arguments = parse_px4_ts_product_arguments(
        static_cast<int>(argv_views.size()), argv_views.data(), kProfile);
#else
    Px4TsArguments arguments = parse_px4_ts_product_arguments(
        argc, const_cast<const char* const*>(argv), kProfile);
#endif
    if (!arguments.valid) {
        std::fprintf(stderr, "torne-ts: %s\n", arguments.error.c_str());
        print_px4_ts_usage(kProfile, stderr);
        return 2;
    }
    if (arguments.help) {
        print_px4_ts_usage(kProfile, stdout);
        return 0;
    }
    if (arguments.version) {
        std::printf("torne-ts %s\n", TORNE_USERLAND_PRODUCT_VERSION);
        return 0;
    }
    if (!arguments.instance.empty()) {
        const auto instance =
            dtv::userland::identity::canonicalize_instance(arguments.instance);
        if (!instance) {
            std::fprintf(stderr, "torne-ts: invalid --instance token\n");
            print_px4_ts_usage(kProfile, stderr);
            return 2;
        }
        arguments.instance.assign(instance.value().bytes.data(),
                                  instance.value().length);
    }
    if (arguments.system != ipc::System::ISDB_T) {
        std::fprintf(stderr,
            "torne-ts: only ISDB-T is supported by this product\n");
        return px4_ts_exit_status(dtv::userland::Error::UNSUPPORTED);
    }

    if (!install_signals()) {
        std::fprintf(stderr, "torne-ts: signal setup failed\n");
        return 70;
    }

    const bool stdout_output = arguments.output == "-";
    std::unique_ptr<Px4TsOutput> output;

#if defined(_WIN32)
    HANDLE handle = ::GetStdHandle(STD_OUTPUT_HANDLE);
    bool close_handle = false;
    if (!stdout_output) {
        const std::vector<wchar_t> path =
            px4::userland::cli::utf8_to_wide_path(arguments.output);
        if (path.empty()) {
            std::fprintf(stderr, "torne-ts: invalid output path\n");
            return 70;
        }
        handle = ::CreateFileW(path.data(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            std::fprintf(stderr, "torne-ts: could not open output file\n");
            return 70;
        }
        close_handle = true;
    }
    auto windows_output = Px4TsWindowsOutput::create(handle, close_handle);
    if (!windows_output) {
        std::fprintf(stderr, "torne-ts: output setup failed\n");
        return 70;
    }
    Px4TsWindowsOutput* const windows_sink = windows_output.value().get();
    output = std::move(windows_output.value());
#else
    int fd = STDOUT_FILENO;
    if (!stdout_output) {
        fd = (::open)(arguments.output.c_str(),
                      O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
        if (fd < 0) {
            std::fprintf(stderr, "torne-ts: could not open output file\n");
            return 70;
        }
    }
    auto fd_output = std::make_unique<Px4TsFdOutput>(fd, !stdout_output);
    if (!fd_output->ready()) {
        std::fprintf(stderr, "torne-ts: output setup failed\n");
        return 70;
    }
    output = std::move(fd_output);
#endif

    SteadyClock clock;
    StopSignal signal;
    Px4TsFailureKind failure_kind = Px4TsFailureKind::ipc;
    Px4TsRunDiagnostics diagnostics;
    const auto result = Px4TsRunner::run_wire(
        arguments, kProfile, *output, clock, signal, &failure_kind, &diagnostics);

#if defined(_WIN32)
    const auto flushed = windows_sink->finish();
    if (result && !flushed) {
        std::fprintf(stderr, "torne-ts: output flush failed\n");
        return 70;
    }
#endif

    if (!result) {
        print_px4_ts_run_diagnostic(stderr, kProfile.tool_name, result.error(),
                                    diagnostics);
        std::fprintf(stderr, "torne-ts: %s\n",
                     dtv::userland::error_string(result.error()));
        return px4_ts_exit_status(result.error(), failure_kind);
    }
    return 0;
}
