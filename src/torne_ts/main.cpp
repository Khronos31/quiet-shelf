// SPDX-License-Identifier: GPL-2.0-only
#include <dtv/host_ts/px4_ts_core.h>
#include <dtv/userland/host_facade/px4d_signals.h>
#include "product/device.h"
#include "product/runner.h"
#include "torne_ts/profile.h"

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

class ProductClock final : public torne::product::Clock {
public:
    explicit ProductClock(Px4TsClock& clock) noexcept : clock_(clock) {}
    std::uint64_t monotonic_ms() noexcept override
    {
        return clock_.monotonic_ms();
    }
    void sleep_ms(std::uint32_t milliseconds) noexcept override
    {
        clock_.sleep_ms(milliseconds);
    }

private:
    Px4TsClock& clock_;
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
        static_cast<int>(argv_views.size()), argv_views.data(),
        torne::ts::kProfile);
#else
    Px4TsArguments arguments = parse_px4_ts_product_arguments(
        argc, const_cast<const char* const*>(argv), torne::ts::kProfile);
#endif
    if (!arguments.valid) {
        std::fprintf(stderr, "torne-ts: %s\n", arguments.error.c_str());
        print_px4_ts_usage(torne::ts::kProfile, stderr);
        return 2;
    }
    if (arguments.help) {
        print_px4_ts_usage(torne::ts::kProfile, stdout);
        std::fputs("  Torne direct hardware: --device usb:BUS-PORT owns the "
                   "device directly; --instance remains the Core IPC client.\n",
                   stdout);
        return 0;
    }
    if (arguments.version) {
        std::printf("torne-ts %s\n", TORNE_USERLAND_PRODUCT_VERSION);
        return 0;
    }
    if (!torne::ts::normalize_instance(arguments)) {
        std::fprintf(stderr, "torne-ts: invalid --instance token\n");
        print_px4_ts_usage(torne::ts::kProfile, stderr);
        return 2;
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

    if (arguments.device.rfind("usb:", 0U) == 0U) {
        if (arguments.receiver != 0U) {
            std::fprintf(stderr,
                "torne-ts: direct mode has one receiver (0)\n");
            return 3;
        }
        ProductClock product_clock(clock);
        auto device = torne::product::open_hardware(arguments.device, product_clock);
        if (!device) {
            std::fprintf(stderr, "torne-ts: direct hardware open: %s\n",
                         dtv::userland::error_string(device.error()));
            return px4_ts_exit_status(device.error());
        }
        torne::product::RunOptions options;
        options.frequency_hz = arguments.frequency_hz;
        options.tune_timeout_ms = arguments.tune_timeout_ms;
        options.duration_seconds = arguments.duration_seconds;
        options.packet_count = arguments.packet_count;
        options.duration_set = arguments.duration_set;
        options.packet_count_set = arguments.packet_count_set;
        const auto direct = torne::product::run(
            *device.value(), options, *output, clock, signal);

#if defined(_WIN32)
        const auto flushed_direct = windows_sink->finish();
        if (direct && !flushed_direct) {
            std::fprintf(stderr, "torne-ts: output flush failed\n");
            return 70;
        }
#endif
        if (!direct) {
            std::fprintf(stderr, "torne-ts: direct run: %s\n",
                         dtv::userland::error_string(direct.error()));
            return px4_ts_exit_status(direct.error());
        }
        return 0;
    }

    Px4TsFailureKind failure_kind = Px4TsFailureKind::ipc;
    Px4TsRunDiagnostics diagnostics;
    const auto result = Px4TsRunner::run_wire(
        arguments, torne::ts::kProfile, *output, clock, signal,
        &failure_kind, &diagnostics);

#if defined(_WIN32)
    const auto flushed = windows_sink->finish();
    if (result && !flushed) {
        std::fprintf(stderr, "torne-ts: output flush failed\n");
        return 70;
    }
#endif

    if (!result) {
        print_px4_ts_run_diagnostic(stderr, torne::ts::kProfile.tool_name,
                                    result.error(),
                                    diagnostics);
        std::fprintf(stderr, "torne-ts: %s\n",
                     dtv::userland::error_string(result.error()));
        return px4_ts_exit_status(result.error(), failure_kind);
    }
    return 0;
}
