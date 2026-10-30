// SPDX-License-Identifier: GPL-2.0-only
#include "usb/transport.h"

#include <cstring>
#include <string_view>

namespace torne::usb {
namespace {

constexpr std::uint16_t kSonyVendorId = 0x054cU;
constexpr std::uint16_t kCechZd1ProductId = 0x04b2U;

bool parse_component(std::string_view text, std::size_t& offset,
                     std::uint8_t& value) noexcept
{
    if (offset >= text.size() || text[offset] < '0' || text[offset] > '9')
        return false;
    if (text[offset] == '0' && offset + 1U < text.size() &&
        text[offset + 1U] >= '0' && text[offset + 1U] <= '9')
        return false;
    unsigned parsed = 0U;
    while (offset < text.size() && text[offset] >= '0' && text[offset] <= '9') {
        parsed = parsed * 10U + static_cast<unsigned>(text[offset] - '0');
        if (parsed > 255U) return false;
        ++offset;
    }
    value = static_cast<std::uint8_t>(parsed);
    return parsed != 0U;
}

bool expected_endpoint(const DeviceSummary& summary, std::uint8_t address,
                       std::uint8_t attributes, std::uint16_t max_packet,
                       std::uint8_t interval) noexcept
{
    for (std::size_t index = 0U;
         index < summary.interface_zero.endpoint_count; ++index) {
        const EndpointInfo& endpoint = summary.interface_zero.endpoints[index];
        if (endpoint.address == address)
            return endpoint.attributes == attributes &&
                   endpoint.max_packet == max_packet &&
                   endpoint.interval == interval;
    }
    return false;
}

bool expected_summary(const DeviceSummary& summary) noexcept
{
    if (summary.vid != kSonyVendorId || summary.pid != kCechZd1ProductId)
        return false;
    if (summary.num_configurations != 1U || summary.configuration_value != 1U ||
        summary.num_interfaces != 1U)
        return false;
    const InterfaceInfo& interface = summary.interface_zero;
    if (interface.number != 0U || interface.alternate != 0U ||
        interface.interface_class != 0xffU || interface.interface_subclass != 0U ||
        interface.interface_protocol != 0U || interface.endpoint_count != 4U)
        return false;
    return expected_endpoint(summary, 0x81U, 0x02U, 512U, 0U) &&
           expected_endpoint(summary, 0x02U, 0x02U, 512U, 0U) &&
           expected_endpoint(summary, 0x83U, 0x03U, 64U, 1U) &&
           expected_endpoint(summary, 0x84U, 0x02U, 512U, 0U);
}

bool kind_direction_matches(ControlKind kind, bool is_in) noexcept
{
    switch (kind) {
    case ControlKind::pure_read:
    case ControlKind::consuming_read:
        return is_in;
    case ControlKind::abs_write:
    case ControlKind::fifo_write:
    case ControlKind::trigger:
        return !is_in;
    case ControlKind::unclassified:
        return false;
    }
    return false;
}

}  // namespace

bool location_from_selector(const dtv::userland::identity::Selector& selector,
                            DeviceLocation& location) noexcept
{
    if (selector.kind != dtv::userland::identity::SelectorKind::usb_topology ||
        selector.topology.count != 1U ||
        selector.topology.paths[0].length == 0U ||
        selector.topology.paths[0].length > selector.topology.paths[0].bytes.size())
        return false;
    const std::string_view path(
        selector.topology.paths[0].bytes.data(),
        selector.topology.paths[0].length);
    DeviceLocation parsed;
    std::size_t offset = 0U;
    if (!parse_component(path, offset, parsed.bus)) return false;
    if (offset >= path.size() || path[offset] != '-') return false;
    ++offset;
    while (offset < path.size()) {
        if (parsed.port_count >= parsed.ports.size()) return false;
        if (!parse_component(path, offset, parsed.ports[parsed.port_count]))
            return false;
        ++parsed.port_count;
        if (offset == path.size()) break;
        if (path[offset] != '.') return false;
        ++offset;
    }
    if (parsed.port_count == 0U) return false;
    location = parsed;
    return true;
}

OpenResult Transport::open(std::unique_ptr<UsbIo> io,
                           const DeviceLocation& location) noexcept
{
    if (!io) return OpenResult{OpenOutcome{OpenState::io, TransferCause::io}, nullptr};
    DeviceSummary summary;
    const OpenOutcome opened = io->open(location, summary);
    if (opened.state != OpenState::ok) {
        io->close();
        return OpenResult{opened, nullptr};
    }
    if (!expected_summary(summary)) {
        io->close();
        return OpenResult{OpenOutcome{OpenState::unsupported,
                                      TransferCause::unsupported}, nullptr};
    }
    const OpenOutcome claimed = io->claim_interface(0U);
    if (claimed.state != OpenState::ok) {
        io->close();
        return OpenResult{claimed, nullptr};
    }
    std::unique_ptr<Transport> transport(
        new Transport(std::move(io), summary));
    transport->claimed_ = true;
    return OpenResult{OpenOutcome{OpenState::ok, TransferCause::none},
                      std::move(transport)};
}

Transport::Transport(std::unique_ptr<UsbIo> io, DeviceSummary summary) noexcept
    : io_(std::move(io)), summary_(summary)
{
}

Transport::~Transport() noexcept
{
    close();
}

void Transport::close() noexcept
{
    if (claimed_ && io_) io_->release_interface(0U);
    claimed_ = false;
    if (io_) {
        io_->close();
        io_.reset();
    }
}

bool Transport::endpoint_supported(std::uint8_t endpoint,
                                   std::uint8_t attributes) const noexcept
{
    for (std::size_t index = 0U;
         index < summary_.interface_zero.endpoint_count; ++index) {
        const EndpointInfo& candidate = summary_.interface_zero.endpoints[index];
        if (candidate.address == endpoint)
            return candidate.attributes == attributes;
    }
    return false;
}

TransferOutcome Transport::control(std::uint8_t request_type,
                                   std::uint8_t request,
                                   std::uint16_t value,
                                   std::uint16_t index,
                                   std::uint16_t length,
                                   std::uint8_t* in_data,
                                   const std::uint8_t* out_data,
                                   std::uint32_t timeout_ms,
                                   ControlKind kind) noexcept
{
    const bool is_in = (request_type & 0x80U) != 0U;
    const bool missing_payload =
        length != 0U && (is_in ? in_data == nullptr : out_data == nullptr);
    if (!claimed_ || !io_ || kind == ControlKind::unclassified ||
        missing_payload || !kind_direction_matches(kind, is_in))
        return TransferOutcome{TransferState::failed, TransferCause::invalid, 0U, false};

    const auto attempt = [&]() noexcept {
        if (is_in)
            return io_->control_in(request_type, request, value, index, length,
                                   in_data, timeout_ms);
        return io_->control_out(request_type, request, value, index, length,
                                out_data, timeout_ms);
    };
    TransferOutcome result = attempt();
    if (result.state == TransferState::complete) return result;

    const bool retryable =
        kind == ControlKind::pure_read || kind == ControlKind::abs_write;
    const bool definite = result.cause == TransferCause::no_device ||
                          result.cause == TransferCause::unsupported ||
                          result.cause == TransferCause::access ||
                          result.cause == TransferCause::busy ||
                          result.cause == TransferCause::invalid;
    if (retryable && !definite) {
        result = attempt();
        result.retried = true;
        return result;
    }
    if (!retryable) {
        if (result.cause == TransferCause::timeout ||
            result.cause == TransferCause::pipe ||
            result.cause == TransferCause::io ||
            result.cause == TransferCause::overflow ||
            result.cause == TransferCause::other ||
            result.state == TransferState::short_transfer)
            result.state = TransferState::outcome_unknown;
    }
    return result;
}

TransferOutcome Transport::control_in(std::uint8_t request_type,
                                      std::uint8_t request,
                                      std::uint16_t value,
                                      std::uint16_t index,
                                      std::uint16_t length,
                                      std::uint8_t* data,
                                      std::uint32_t timeout_ms,
                                      ControlKind kind) noexcept
{
    if ((request_type & 0x80U) == 0U)
        return TransferOutcome{TransferState::failed, TransferCause::invalid,
                               0U, false};
    return control(request_type, request, value, index, length, data, nullptr,
                   timeout_ms, kind);
}

TransferOutcome Transport::control_out(std::uint8_t request_type,
                                       std::uint8_t request,
                                       std::uint16_t value,
                                       std::uint16_t index,
                                       std::uint16_t length,
                                       const std::uint8_t* data,
                                       std::uint32_t timeout_ms,
                                       ControlKind kind) noexcept
{
    if ((request_type & 0x80U) != 0U)
        return TransferOutcome{TransferState::failed, TransferCause::invalid,
                               0U, false};
    return control(request_type, request, value, index, length, nullptr, data,
                   timeout_ms, kind);
}

TransferOutcome Transport::bulk_in(std::uint8_t endpoint,
                                   std::uint8_t* data,
                                   std::size_t length,
                                   std::uint32_t timeout_ms) noexcept
{
    if (!claimed_ || !io_ || (endpoint & 0x80U) == 0U ||
        !endpoint_supported(endpoint, 0x02U) ||
        (length != 0U && data == nullptr))
        return TransferOutcome{TransferState::failed, TransferCause::invalid, 0U, false};
    return io_->bulk_in(endpoint, data, length, timeout_ms);
}

TransferOutcome Transport::bulk_out(std::uint8_t endpoint,
                                    const std::uint8_t* data,
                                    std::size_t length,
                                    std::uint32_t timeout_ms) noexcept
{
    if (!claimed_ || !io_ || (endpoint & 0x80U) != 0U ||
        !endpoint_supported(endpoint, 0x02U) ||
        (length != 0U && data == nullptr))
        return TransferOutcome{TransferState::failed, TransferCause::invalid, 0U, false};
    return io_->bulk_out(endpoint, data, length, timeout_ms);
}

}  // namespace torne::usb
