// SPDX-License-Identifier: GPL-2.0-or-later
#include <dtv/internal/boot.hpp>

#include <algorithm>
#include <array>
#include <string_view>
#include <vector>

namespace {

constexpr dtv_function kFirst = 10;
constexpr dtv_function kSecond = 20;
constexpr dtv_function kOtherDevice = 30;
constexpr char kAbcHash[] = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
constexpr char kDefHash[] = "cb8379ac2098aa165029e3938a51da0bcecfc008fd6795f401178647f96c5b34";
constexpr char kWrongHash[] = "0000000000000000000000000000000000000000000000000000000000000000";

struct FunctionState {
    dtv_function role = 0;
};

const dtv_usb_io fake_io{};
std::vector<dtv_function> opened_roles;
std::vector<dtv_function> closed_roles;
std::vector<dtv_function> destroyed_roles;
std::vector<std::uint32_t> written_stages;
std::vector<dtv_function> written_roles;
std::vector<std::uint32_t> identify_roles;
std::uint32_t firmware_write_count = 0;
dtv_function fail_open_role = 0;
dtv_function fail_firmware_role = 0;
std::uint32_t fail_firmware_stage = 0;
struct FakeClockState {
    std::uint64_t now_ns = 0;
    bool cancelled = false;
};
FakeClockState *cancel_on_firmware = nullptr;
bool force_ambiguous = false;
bool force_no_match = false;
bool force_duplicate_selection = false;
bool force_duplicate_interface = false;
bool force_invalid_interface = false;
bool force_invalid_alternate = false;
bool force_complete = false;
bool visible_one = false;
bool include_other_device = false;
dtv_function force_unknown_role = 0;

std::uint64_t fake_now(void *user)
{
    auto &state = *static_cast<FakeClockState *>(user);
    state.now_ns += 1000000;
    return state.now_ns;
}

dtv_internal_result fake_is_cancelled(void *user, const dtv_cancel *, std::uint32_t *out)
{
    *out = static_cast<FakeClockState *>(user)->cancelled;
    return {DTV_INTERNAL_RESULT_OK, 0};
}

struct FakeProvider final : dtv::internal::BootUsbProvider {
    std::vector<std::uint64_t> opened_candidates;
    std::vector<std::vector<dtv::internal::UsbInterfaceClaim>> claim_sets;
    std::vector<std::uint64_t> released_candidates;
    std::uint64_t fail_candidate = 0;
    int fail_interface = -1;

    struct Owner final : dtv::internal::BootUsbOwner {
        Owner(FakeProvider &provider, std::uint64_t id) : provider(provider), candidate_id(id) {}
        ~Owner() override { provider.released_candidates.push_back(candidate_id); }
        FakeProvider &provider;
        std::uint64_t candidate_id;
        dtv_usb_io usb_io{};
        const dtv_usb_io *io() const override { return &usb_io; }
    };

    std::unique_ptr<dtv::internal::BootUsbOwner> open(
        const dtv::internal::UsbDeviceSnapshot &, int, const dtv_runtime_clock *,
        dtv_internal_result *result) override
    {
        *result = {DTV_INTERNAL_RESULT_UNSUPPORTED, 0};
        return nullptr;
    }

    std::unique_ptr<dtv::internal::BootUsbOwner> open_group(
        const dtv::internal::UsbDeviceSnapshot &snapshot,
        std::span<const dtv::internal::UsbInterfaceClaim> claims,
        const dtv_runtime_clock *, dtv_internal_result *result) override
    {
        opened_candidates.push_back(snapshot.candidate_id);
        claim_sets.emplace_back(claims.begin(), claims.end());
        if (snapshot.candidate_id == fail_candidate) {
            *result = {DTV_INTERNAL_RESULT_USB_IO, 23};
            return nullptr;
        }
        if (std::any_of(claims.begin(), claims.end(), [&](const auto &claim) {
                return claim.interface_number == fail_interface;
            })) {
            *result = {DTV_INTERNAL_RESULT_USB_IO, 23};
            return std::make_unique<Owner>(*this, snapshot.candidate_id);
        }
        *result = {DTV_INTERNAL_RESULT_OK, 0};
        return std::make_unique<Owner>(*this, snapshot.candidate_id);
    }
};

dtv_internal_result create_state(dtv_device_id, dtv_context **out)
{
    *out = new FunctionState;
    return {DTV_INTERNAL_RESULT_OK, 0};
}

void destroy_state(dtv_context *context)
{
    auto *state = static_cast<FunctionState *>(context);
    destroyed_roles.push_back(state->role);
    delete state;
}

dtv_internal_result open_function(dtv_context *context, const dtv_usb_io *io,
                                  const dtv_open_args *args)
{
    auto *state = static_cast<FunctionState *>(context);
    state->role = args->function;
    opened_roles.push_back(state->role);
    if (io == nullptr || args->usb_observation == nullptr ||
        args->usb_observation->vendor_id != 0x1234 ||
        args->usb_observation->product_id != 0x0001 ||
        args->usb_observation->port_path_length != 2 ||
        args->interface_number > 1 || args->alternate_setting != 0 ||
        args->usb_observation->interface_count != 2) {
        return {DTV_INTERNAL_RESULT_INVALID_ARGUMENT, 0};
    }
    if (state->role == fail_open_role) return {DTV_INTERNAL_RESULT_BUSY, 41};
    return {DTV_INTERNAL_RESULT_OK, 0};
}

dtv_internal_result close_function(dtv_context *context, dtv_timeout)
{
    closed_roles.push_back(static_cast<FunctionState *>(context)->role);
    return {DTV_INTERNAL_RESULT_OK, 0};
}

dtv_internal_result identify_function(dtv_context *context, dtv_identify_info *info, dtv_timeout)
{
    auto *state = static_cast<FunctionState *>(context);
    identify_roles.push_back(state->role);
    if (state->role == force_unknown_role) {
        info->state = DTV_IDENTIFY_UNKNOWN;
        info->firmware_key = 0;
        return {DTV_INTERNAL_RESULT_OK, 0};
    }
    const bool written = std::find(written_roles.begin(), written_roles.end(), state->role) !=
                         written_roles.end();
    info->state = written ? DTV_IDENTIFY_RUNTIME : DTV_IDENTIFY_LOADER;
    info->firmware_key = state->role + 100;
    return {DTV_INTERNAL_RESULT_OK, 0};
}

dtv_internal_result firmware_function(dtv_context *context, std::uint32_t stage, dtv_bytes bytes,
                                      dtv_firmware_result *result, dtv_timeout timeout)
{
    auto *state = static_cast<FunctionState *>(context);
    if (timeout.operation_deadline_ns == 0 || bytes.data == nullptr || bytes.size != 3) {
        return {DTV_INTERNAL_RESULT_INVALID_ARGUMENT, 0};
    }
    const std::string_view image(reinterpret_cast<const char *>(bytes.data), bytes.size);
    if ((state->role == kFirst && (stage != 41 || image != "abc")) ||
        (state->role == kSecond && (stage != 42 || image != "def")) ||
        state->role == kOtherDevice) {
        return {DTV_INTERNAL_RESULT_INVALID_ARGUMENT, stage};
    }
    ++firmware_write_count;
    written_stages.push_back(stage);
    written_roles.push_back(state->role);
    if (state->role == fail_firmware_role && stage == fail_firmware_stage) {
        return {DTV_INTERNAL_RESULT_USB_IO, 91};
    }
    if (cancel_on_firmware != nullptr) cancel_on_firmware->cancelled = true;
    result->transition = DTV_FIRMWARE_SAME_HANDLE;
    return {DTV_INTERNAL_RESULT_OK, 0};
}

const dtv::spi::DeviceOps bootstrap_ops{
    .open = open_function,
    .identify = identify_function,
    .firmware = firmware_function,
    .close = close_function,
};
const dtv::spi::DeviceOps first_runtime{};
const dtv::spi::DeviceOps second_runtime{};
const dtv::spi::DeviceOps other_runtime{};
const dtv::spi::FirmwareEntry first_runtime_entries[]{{kFirst + 100, &first_runtime}};
const dtv::spi::FirmwareEntry second_runtime_entries[]{{kSecond + 100, &second_runtime}};
const dtv::spi::FirmwareEntry other_runtime_entries[]{{kOtherDevice + 100, &other_runtime}};
dtv::spi::FirmwareManifestImage first_images[]{{0, 3, kAbcHash}};
dtv::spi::FirmwareManifestImage second_images[]{{3, 3, kWrongHash}};
dtv::spi::FirmwareManifestImage other_images[]{{100, 3, kAbcHash}};
const dtv::spi::FunctionFirmwareStage first_stages[]{{41, 0, DTV_IDENTIFY_LOADER,
                                                       DTV_IDENTIFY_RUNTIME}};
dtv::spi::FunctionFirmwareStage second_stages[]{{42, 0, DTV_IDENTIFY_LOADER,
                                                  DTV_IDENTIFY_RUNTIME}};
const dtv::spi::FunctionFirmwareStage other_stages[]{{43, 0, DTV_IDENTIFY_LOADER,
                                                       DTV_IDENTIFY_RUNTIME}};
const dtv::spi::FunctionEntry function_entries[]{
    {kFirst, &bootstrap_ops, first_runtime_entries, 1, first_images, 1, first_stages, 1,
     create_state, destroy_state},
    {kSecond, &bootstrap_ops, second_runtime_entries, 1, second_images, 1, second_stages, 1,
     create_state, destroy_state},
    {kOtherDevice, &bootstrap_ops, other_runtime_entries, 1, other_images, 1, other_stages, 1,
     create_state, destroy_state},
};
const dtv_usb_identity cold_identities[]{{0x1234, 0x0001}};
const dtv_function required_first[]{kFirst};
const dtv_function required_first_and_second[]{kFirst, kSecond};
const dtv_function required_all[]{kFirst, kSecond, kOtherDevice};
const dtv::spi::OperationRequirement operation_requirements[]{
    {7, DTV_SPI_OPERATION_LNB_POWER, required_first, 1},
    {7, DTV_SPI_OPERATION_CARD, required_first_and_second, 2},
    {7, DTV_SPI_OPERATION_OUTPUT_PID_FILTER, required_all, 3},
};

dtv_internal_result select_group(const dtv_usb_candidate_info *, std::uint32_t count,
                                 std::uint32_t anchor, dtv_usb_function_selection *out,
                                 std::uint32_t capacity, std::uint32_t *out_count,
                                 dtv_usb_group_match *match)
{
    if (force_no_match) {
        *out_count = 0;
        *match = DTV_USB_GROUP_NO_MATCH;
        return {DTV_INTERNAL_RESULT_OK, 0};
    }
    if (force_ambiguous) {
        *out_count = 0;
        *match = DTV_USB_GROUP_AMBIGUOUS;
        return {DTV_INTERNAL_RESULT_OK, 0};
    }
    if (force_duplicate_selection) {
        *match = DTV_USB_GROUP_INCOMPLETE;
        *out_count = 2;
        out[0] = {anchor, kFirst, 0, 0};
        out[1] = {anchor, kFirst, force_duplicate_interface ? std::uint8_t{0} : std::uint8_t{1}, 0};
        return {DTV_INTERNAL_RESULT_OK, 0};
    }
    *match = force_complete ? DTV_USB_GROUP_COMPLETE : DTV_USB_GROUP_INCOMPLETE;
    *out_count = visible_one ? 1 : include_other_device && count > 1 ? 3 : 2;
    if (*out_count > capacity) return {DTV_INTERNAL_RESULT_INVALID_ARGUMENT, 0};
    out[0] = {0, kFirst, static_cast<std::uint8_t>(force_invalid_interface ? 9 : 0),
              static_cast<std::uint8_t>(force_invalid_alternate ? 7 : 0)};
    if (*out_count > 1) out[1] = {0, kSecond,
        static_cast<std::uint8_t>(force_duplicate_interface ? 0 : 1), 0};
    if (*out_count == 3) out[2] = {1, kOtherDevice, 0, 0};
    return {DTV_INTERNAL_RESULT_OK, 0};
}

const dtv::spi::ModelEntry model{
    .model = {"fixture", 0x9999, 0x0001},
    .cold_identities = cold_identities,
    .cold_identity_count = std::size(cold_identities),
    .select_usb_group = select_group,
    .functions = function_entries,
    .function_count = std::size(function_entries),
    .operation_requirements = operation_requirements,
    .operation_requirement_count = std::size(operation_requirements),
};
const dtv::spi::ModelEntry *models[]{&model};
const dtv::spi::ProductEntry product{"fixture-partial", models, 1};
const dtv::spi::ProductEntry *products[]{&product};

dtv::internal::UsbDeviceSnapshot make_snapshot(std::uint64_t id)
{
    dtv::internal::UsbDeviceSnapshot snapshot{};
    snapshot.candidate_id = id;
    snapshot.vendor_id = 0x1234;
    snapshot.product_id = 0x0001;
    snapshot.device_version = 0x0102;
    snapshot.bus_number = 2;
    snapshot.device_address = static_cast<std::uint8_t>(id);
    snapshot.port_path = {3, static_cast<std::uint8_t>(id)};
    for (std::uint8_t interface_number = 0; interface_number < 2; ++interface_number) {
        dtv::internal::UsbInterfaceSnapshot interface{};
        interface.info.interface_number = interface_number;
        interface.info.alternate_setting = 0;
        interface.info.class_code = 0xff;
        interface.endpoints.push_back({static_cast<std::uint8_t>(0x81 + interface_number), 2, 64, 0});
        snapshot.interfaces.push_back(std::move(interface));
    }
    return snapshot;
}

dtv::internal::BootGroupCandidate discover(bool two_devices = false)
{
    auto first = make_snapshot(1);
    std::vector<dtv::internal::UsbDeviceSnapshot> snapshots{first};
    if (two_devices) snapshots.push_back(make_snapshot(2));
    auto groups = dtv::internal::discover_boot_candidates(snapshots, products);
    return groups.empty() ? dtv::internal::BootGroupCandidate{} : std::move(groups.front());
}

void reset_observations()
{
    opened_roles.clear();
    closed_roles.clear();
    destroyed_roles.clear();
    written_stages.clear();
    written_roles.clear();
    identify_roles.clear();
    firmware_write_count = 0;
    fail_open_role = 0;
    fail_firmware_role = 0;
    fail_firmware_stage = 0;
    force_unknown_role = 0;
}

bool partial_and_ambiguous()
{
    force_no_match = force_ambiguous = force_duplicate_selection = include_other_device = false;
    force_complete = visible_one = false;
    auto group = discover();
    if (group.match != DTV_USB_GROUP_INCOMPLETE || group.functions.size() != 2 ||
        group.snapshots.size() != 1 ||
        dtv::internal::check_operation_requirements(group, 7, DTV_SPI_OPERATION_LNB_POWER).code != DTV_INTERNAL_RESULT_OK ||
        dtv::internal::check_operation_requirements(group, 7, DTV_SPI_OPERATION_CARD).code != DTV_INTERNAL_RESULT_OK ||
        dtv::internal::check_operation_requirements(group, 7, DTV_SPI_OPERATION_OUTPUT_PID_FILTER).code != DTV_INTERNAL_RESULT_NOT_READY) {
        return false;
    }
    visible_one = true;
    auto one_visible = discover();
    visible_one = false;
    if (one_visible.match != DTV_USB_GROUP_INCOMPLETE || one_visible.functions.size() != 1 ||
        dtv::internal::check_operation_requirements(one_visible, 7, DTV_SPI_OPERATION_LNB_POWER).code != DTV_INTERNAL_RESULT_OK ||
        dtv::internal::check_operation_requirements(one_visible, 7, DTV_SPI_OPERATION_CARD).code != DTV_INTERNAL_RESULT_NOT_READY) {
        return false;
    }
    force_complete = true;
    auto complete = discover();
    force_complete = false;
    if (complete.match != DTV_USB_GROUP_COMPLETE || complete.functions.size() != 2) return false;
    auto wrong_cold = complete;
    wrong_cold.snapshots[0].product_id = 0x0002;
    FakeProvider cold_provider;
    dtv::internal::UsbContext usb;
    dtv_internal_result cold_result{};
    auto cold_rejected = dtv::internal::open_boot_group(usb, wrong_cold, 1, {1000, 0}, nullptr,
                                                       nullptr, &cold_provider, &cold_result);
    if (cold_rejected || cold_result.code != DTV_INTERNAL_RESULT_UNSUPPORTED ||
        !cold_provider.opened_candidates.empty()) return false;
    force_ambiguous = true;
    auto ambiguous = discover();
    FakeProvider provider;
    dtv_internal_result result{};
    auto rejected = dtv::internal::open_boot_group(usb, ambiguous, 1, {1000, 0}, nullptr,
                                                   nullptr, &provider, &result);
    force_ambiguous = false;
    if (ambiguous.match != DTV_USB_GROUP_AMBIGUOUS || rejected || !provider.opened_candidates.empty()) {
        return false;
    }
    force_no_match = true;
    const auto no_match = discover();
    force_no_match = false;
    if (no_match.model != nullptr) return false;
    auto runtime_only_snapshot = make_snapshot(1);
    runtime_only_snapshot.vendor_id = model.model.vendor_id;
    runtime_only_snapshot.product_id = model.model.product_id;
    const auto runtime_only = dtv::internal::discover_boot_candidates(
        std::array{runtime_only_snapshot}, products);
    if (!runtime_only.empty()) return false;
    force_duplicate_selection = true;
    dtv_internal_result discovery_result{};
    const auto invalid_group = dtv::internal::discover_boot_candidates(
        std::array{make_snapshot(1)}, products, &discovery_result);
    force_duplicate_selection = false;
    if (!invalid_group.empty() || discovery_result.code != DTV_INTERNAL_RESULT_INVALID_ARGUMENT) return false;
    force_duplicate_interface = true;
    const auto duplicate_interface_group = dtv::internal::discover_boot_candidates(
        std::array{make_snapshot(1)}, products, &discovery_result);
    force_duplicate_interface = false;
    if (!duplicate_interface_group.empty() ||
        discovery_result.code != DTV_INTERNAL_RESULT_INVALID_ARGUMENT) return false;
    for (const auto invalid_selection : {0, 1}) {
        force_invalid_interface = invalid_selection == 0;
        force_invalid_alternate = invalid_selection == 1;
        const auto invalid_view = dtv::internal::discover_boot_candidates(
            std::array{make_snapshot(1)}, products, &discovery_result);
        if (!invalid_view.empty() || discovery_result.code != DTV_INTERNAL_RESULT_INVALID_ARGUMENT) {
            force_invalid_interface = force_invalid_alternate = false;
            return false;
        }
    }
    force_invalid_interface = force_invalid_alternate = false;
    return true;
}

bool same_handle_package()
{
    reset_observations();
    force_no_match = force_ambiguous = force_duplicate_selection = false;
    include_other_device = true;
    auto group = discover(true);
    include_other_device = false;
    if (group.match != DTV_USB_GROUP_INCOMPLETE || group.functions.size() != 3 ||
        group.snapshots.size() != 2 ||
        dtv::internal::check_operation_requirements(group, 7, DTV_SPI_OPERATION_LNB_POWER).code != DTV_INTERNAL_RESULT_OK) {
        return false;
    }
    FakeProvider provider;
    dtv::internal::UsbContext usb;
    dtv_internal_result result{};
    auto session = dtv::internal::open_boot_group(usb, group, 99, {1000, 0}, nullptr,
                                                  nullptr, &provider, &result);
    if (!session || result.code != DTV_INTERNAL_RESULT_OK || provider.opened_candidates.size() != 2 ||
        provider.claim_sets[0].size() != 2 || provider.claim_sets[1].size() != 1 ||
        session->io(kFirst) != session->io(kSecond) || session->io(kFirst) == session->io(kOtherDevice) ||
        opened_roles.size() != 3 || !std::all_of(opened_roles.begin(), opened_roles.end(),
            [](dtv_function role) { return role == kFirst || role == kSecond || role == kOtherDevice; })) {
        return false;
    }

    std::array<std::uint8_t, 6> package{'a', 'b', 'c', 'd', 'e', 'f'};
    const auto bad_hash = session->send_firmware(0, {package.data(), package.size()}, {1000, 0});
    if (bad_hash.code == DTV_INTERNAL_RESULT_OK || firmware_write_count != 0 ||
        !identify_roles.empty()) return false;
    second_images[0].sha256 = kDefHash;
    second_stages[0].image_index = 9;
    const auto bad_stage = session->send_firmware(0, {package.data(), package.size()}, {1000, 0});
    second_stages[0].image_index = 0;
    if (bad_stage.code != DTV_INTERNAL_RESULT_INVALID_ARGUMENT || firmware_write_count != 0 ||
        !identify_roles.empty()) return false;

    const auto completed = session->send_firmware(0, {package.data(), package.size()}, {1000, 0});
    const bool package_ok = completed.code == DTV_INTERNAL_RESULT_OK && firmware_write_count == 2 &&
        written_stages == std::vector<std::uint32_t>{41, 42} &&
        session->ops(kFirst) == &first_runtime && session->ops(kSecond) == &second_runtime &&
        session->ops(kOtherDevice) == nullptr;
    session.reset();
    const bool reverse_release = provider.released_candidates == std::vector<std::uint64_t>{2, 1};
    return package_ok && reverse_release;
}

bool failed_claim_and_open_cleanup()
{
    auto group = discover(true);
    include_other_device = true;
    group.functions.push_back({1, kOtherDevice, 1, 0});
    include_other_device = false;
    dtv::internal::UsbContext usb;
    dtv_internal_result result{};
    FakeProvider claim_failure;
    claim_failure.fail_candidate = 2;
    reset_observations();
    auto failed = dtv::internal::open_boot_group(usb, group, 5, {1000, 0}, nullptr,
                                                 nullptr, &claim_failure, &result);
    if (!failed || result.code != DTV_INTERNAL_RESULT_USB_IO ||
        failed->open_result().code != DTV_INTERNAL_RESULT_USB_IO || firmware_write_count != 0) {
        return false;
    }
    failed->close();
    failed.reset();
    if (claim_failure.released_candidates != std::vector<std::uint64_t>{1}) return false;

    group.snapshots.resize(1);
    group.functions = {{0, kFirst, 0, 0}, {0, kSecond, 1, 0}};
    FakeProvider interface_failure;
    interface_failure.fail_interface = 1;
    auto failed_multiclaim = dtv::internal::open_boot_group(usb, group, 5, {1000, 0}, nullptr,
                                                            nullptr, &interface_failure, &result);
    if (!failed_multiclaim || result.code != DTV_INTERNAL_RESULT_USB_IO ||
        failed_multiclaim->open_result().code != DTV_INTERNAL_RESULT_USB_IO) return false;
    failed_multiclaim.reset();
    if (interface_failure.released_candidates != std::vector<std::uint64_t>{1}) return false;

    FakeProvider open_failure;
    reset_observations();
    fail_open_role = kSecond;
    auto failed_open = dtv::internal::open_boot_group(usb, group, 5, {1000, 0}, nullptr,
                                                      nullptr, &open_failure, &result);
    fail_open_role = 0;
    const bool open_failed = failed_open && result.code == DTV_INTERNAL_RESULT_BUSY &&
        failed_open->open_result().code == DTV_INTERNAL_RESULT_BUSY;
    failed_open.reset();
    return open_failed &&
        closed_roles == std::vector<dtv_function>{kSecond, kFirst} &&
        destroyed_roles == std::vector<dtv_function>{kSecond, kFirst} &&
        open_failure.released_candidates == std::vector<std::uint64_t>{1};
}

bool failed_stage_blocks_retry_and_closes()
{
    reset_observations();
    force_no_match = force_ambiguous = force_duplicate_selection = false;
    include_other_device = false;
    second_images[0].sha256 = kDefHash;
    auto group = discover();
    dtv::internal::UsbContext usb;
    FakeProvider provider;
    dtv_internal_result result{};
    auto session = dtv::internal::open_boot_group(usb, group, 99, {1000, 0}, nullptr,
                                                  nullptr, &provider, &result);
    if (!session || result.code != DTV_INTERNAL_RESULT_OK) return false;

    std::array<std::uint8_t, 6> package{'a', 'b', 'c', 'd', 'e', 'f'};
    fail_firmware_role = kFirst;
    fail_firmware_stage = 41;
    const auto failed = session->send_firmware(0, {package.data(), package.size()}, {1000, 0});
    fail_firmware_role = 0;
    fail_firmware_stage = 0;
    const auto identify_count = identify_roles.size();
    if (failed.code != DTV_INTERNAL_RESULT_USB_IO || failed.detail != 91 ||
        firmware_write_count != 1 || written_stages != std::vector<std::uint32_t>{41} ||
        session->context(kFirst) == nullptr || session->context(kSecond) == nullptr) {
        return false;
    }

    const auto retry = session->send_firmware(0, {package.data(), package.size()}, {1000, 0});
    if (retry.code != failed.code || retry.detail != failed.detail || firmware_write_count != 1 ||
        written_stages != std::vector<std::uint32_t>{41} || identify_roles.size() != identify_count) {
        return false;
    }
    session->close();
    const bool closed = closed_roles == std::vector<dtv_function>{kSecond, kFirst} &&
        destroyed_roles == std::vector<dtv_function>{kSecond, kFirst} &&
        provider.released_candidates == std::vector<std::uint64_t>{1};
    session.reset();
    return closed;
}

bool deadline_and_unknown_boundaries()
{
    second_images[0].sha256 = kDefHash;
    auto group = discover();
    dtv::internal::UsbContext usb;
    dtv_internal_result result{};
    FakeClockState clock_state;
    dtv_runtime_clock clock{&clock_state, fake_now, nullptr, fake_is_cancelled};
    const auto *cancel = reinterpret_cast<const dtv_cancel *>(&clock_state);
    FakeProvider provider;
    reset_observations();
    auto session = dtv::internal::open_boot_group(usb, group, 99, {1000, 0}, &clock,
                                                  cancel, &provider, &result);
    if (!session || result.code != DTV_INTERNAL_RESULT_OK) return false;
    std::array<std::uint8_t, 6> package{'a', 'b', 'c', 'd', 'e', 'f'};
    cancel_on_firmware = &clock_state;
    const auto cancelled = session->send_firmware(0, {package.data(), package.size()}, {1000, 0},
                                                   cancel);
    cancel_on_firmware = nullptr;
    if (cancelled.code != DTV_INTERNAL_RESULT_TIMEOUT || firmware_write_count != 1 ||
        written_stages != std::vector<std::uint32_t>{41} ||
        session->ops(kFirst) != nullptr || session->ops(kSecond) != nullptr) {
        return false;
    }
    session.reset();

    reset_observations();
    provider = FakeProvider{};
    auto unknown_session = dtv::internal::open_boot_group(usb, group, 99, {1000, 0}, nullptr,
                                                          nullptr, &provider, &result);
    if (!unknown_session || result.code != DTV_INTERNAL_RESULT_OK) return false;
    force_unknown_role = kFirst;
    const auto unknown = unknown_session->send_firmware(0,
        {package.data(), package.size()}, {1000, 0});
    force_unknown_role = 0;
    const bool unknown_rejected = unknown.code == DTV_INTERNAL_RESULT_NOT_READY &&
        firmware_write_count == 0 && written_stages.empty() &&
        !unknown_session->ops(kFirst) && !unknown_session->ops(kSecond);
    unknown_session.reset();
    return unknown_rejected;
}

}

int main()
{
    return partial_and_ambiguous() && same_handle_package() && failed_claim_and_open_cleanup() &&
        failed_stage_blocks_retry_and_closes() && deadline_and_unknown_boundaries() ? 0 : 1;
}
