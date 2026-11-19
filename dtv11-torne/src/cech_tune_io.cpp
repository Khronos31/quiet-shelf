/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cech_tune_io.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace torne::cech_zd1j {
namespace {

constexpr std::uint32_t kWideRequest = 0xBC;
constexpr std::uint32_t kWideWriteType = 0x40;
constexpr std::uint32_t kWideReadType = 0xC0;
constexpr std::uint32_t kControlAddress = 0x017A3840;
constexpr std::uint32_t kProducerAddress = 0x017A3850;
constexpr std::uint32_t kProgressAddress = 0x017A389C;
constexpr std::uint32_t kRequestAddress = 0x017A3900;
constexpr std::uint32_t kCompletionAddress = 0x017A3B00;
constexpr std::size_t kRecordSize = 64;
constexpr std::size_t kSlotCount = 8;

dtv_internal_result result(std::int32_t code, std::uint32_t detail = 0)
{
    return dtv_internal_result_make(code, detail);
}

std::uint16_t packed_value(std::uint32_t address)
{
    return static_cast<std::uint16_t>((((address >> 16) & 0xFF) << 8) |
                                      ((address >> 24) & 0xFF));
}

std::uint32_t read_be32(const std::uint8_t *bytes)
{
    return (static_cast<std::uint32_t>(bytes[0]) << 24) |
           (static_cast<std::uint32_t>(bytes[1]) << 16) |
           (static_cast<std::uint32_t>(bytes[2]) << 8) | bytes[3];
}

void write_be32(std::uint8_t *bytes, std::uint32_t value)
{
    bytes[0] = static_cast<std::uint8_t>(value >> 24);
    bytes[1] = static_cast<std::uint8_t>(value >> 16);
    bytes[2] = static_cast<std::uint8_t>(value >> 8);
    bytes[3] = static_cast<std::uint8_t>(value);
}

void write_be16(std::uint8_t *bytes, std::uint16_t value)
{
    bytes[0] = static_cast<std::uint8_t>(value >> 8);
    bytes[1] = static_cast<std::uint8_t>(value);
}

std::uint16_t read_be16(const std::uint8_t *bytes)
{
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(bytes[0]) << 8) |
                                      bytes[1]);
}

dtv_internal_result control_read(const dtv_usb_io *io, dtv_timeout timeout,
                                 std::uint32_t address, std::span<std::uint8_t> bytes)
{
    if (io == nullptr || io->control == nullptr || bytes.empty() || (address & 3) != 0 ||
        address < 0x01000000 || address > 0x01FFFFFF ||
        bytes.size() > 0x02000000 - address)
        return result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT);
    dtv_buffer buffer{bytes.data(), bytes.size(), 0};
    const auto transferred = io->control(io->user, kWideReadType, kWideRequest,
        packed_value(address), static_cast<std::uint16_t>(address), &buffer, timeout);
    if (transferred.code != DTV_INTERNAL_RESULT_OK) return transferred;
    if (buffer.size != bytes.size())
        return result(DTV_INTERNAL_RESULT_USB_IO, static_cast<std::uint32_t>(buffer.size));
    return transferred;
}

dtv_internal_result control_write(const dtv_usb_io *io, dtv_timeout timeout,
                                  std::uint32_t address,
                                  std::span<const std::uint8_t> bytes)
{
    if (io == nullptr || io->control == nullptr || bytes.empty() || (address & 3) != 0 ||
        address < 0x01000000 || address > 0x01FFFFFF ||
        bytes.size() > 0x02000000 - address)
        return result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT);
    dtv_buffer buffer{const_cast<std::uint8_t *>(bytes.data()), bytes.size(), bytes.size()};
    return io->control(io->user, kWideWriteType, kWideRequest, packed_value(address),
        static_cast<std::uint16_t>(address), &buffer, timeout);
}

dtv_internal_result map_i2c_status(std::uint32_t status)
{
    const auto code = status & 0xFF;
    switch (code) {
    case 0: return result(DTV_INTERNAL_RESULT_OK);
    case 4:
    case 32: return result(DTV_INTERNAL_RESULT_TIMEOUT, code);
    case 5: return result(DTV_INTERNAL_RESULT_BUSY, code);
    case 16: return result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT, code);
    case 17: return result(DTV_INTERNAL_RESULT_NOT_READY, code);
    case 18:
    case 33: return result(DTV_INTERNAL_RESULT_DISCONNECTED, code);
    case 1:
    case 2:
    case 3:
    case 34: return result(DTV_INTERNAL_RESULT_USB_IO);
    default: return result(DTV_INTERNAL_RESULT_USB_IO);
    }
}

}

CechI2cIo::CechI2cIo(const dtv_usb_io *io, dtv_timeout timeout,
                     const std::atomic<bool> *cancelled, I2cCursor *cursor)
    : io_(io), timeout_(timeout), cancelled_(cancelled), cursor_(cursor)
{
}

dtv_internal_result CechI2cIo::read_demodulator(std::uint16_t reg, std::uint16_t *value)
{
    if (value == nullptr) return result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT);
    std::uint8_t address[2]{};
    std::uint8_t response[2]{};
    write_be16(address, reg);
    const auto transferred = transact(0x24, 3, address, response);
    if (transferred.code == DTV_INTERNAL_RESULT_OK) *value = read_be16(response);
    return transferred;
}

dtv_internal_result CechI2cIo::write_demodulator(std::uint16_t reg, std::uint16_t value)
{
    std::uint8_t bytes[4]{};
    write_be16(bytes, reg);
    write_be16(bytes + 2, value);
    return transact(0x24, 1, bytes, {});
}

dtv_internal_result CechI2cIo::read_tuner(std::uint8_t reg, std::uint16_t *value)
{
    if (value == nullptr) return result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT);
    const std::uint8_t address[1]{reg};
    std::uint8_t response[2]{};
    const auto transferred = transact(0xC0, 3, address, response);
    if (transferred.code == DTV_INTERNAL_RESULT_OK) *value = read_be16(response);
    return transferred;
}

dtv_internal_result CechI2cIo::write_tuner(std::uint8_t reg, std::uint16_t value)
{
    std::uint8_t bytes[3]{reg, 0, 0};
    write_be16(bytes + 1, value);
    return transact(0xC0, 1, bytes, {});
}

dtv_internal_result CechI2cIo::initialize_cursor()
{
    if (cursor_ == nullptr) return result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT);
    if (cursor_->pending) return result(DTV_INTERNAL_RESULT_NOT_READY);
    std::uint8_t bytes[4]{};
    auto transferred = control_read(io_, timeout_, kControlAddress, bytes);
    if (transferred.code != DTV_INTERNAL_RESULT_OK) return transferred;
    const auto session = read_be32(bytes) & 0xFFFF0000;
    if (session == 0) return result(DTV_INTERNAL_RESULT_NOT_READY);
    transferred = control_read(io_, timeout_, kProducerAddress, bytes);
    if (transferred.code != DTV_INTERNAL_RESULT_OK) return transferred;
    cursor_->session = session;
    cursor_->sequence = static_cast<std::uint16_t>(read_be32(bytes));
    return result(DTV_INTERNAL_RESULT_OK);
}

dtv_internal_result CechI2cIo::finish_pending(std::span<std::uint8_t> input)
{
    if (cursor_ == nullptr || !cursor_->pending)
        return result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT);
    if (io_ == nullptr || io_->clock == nullptr ||
        io_->clock->monotonic_now_ns == nullptr || io_->clock->delay_until == nullptr)
        return result(DTV_INTERNAL_RESULT_UNSUPPORTED);

    std::uint8_t session_bytes[4]{};
    auto transferred = control_read(io_, timeout_, kControlAddress, session_bytes);
    if (transferred.code != DTV_INTERNAL_RESULT_OK) return transferred;
    if ((read_be32(session_bytes) & 0xFFFF0000) != cursor_->session) {
        cursor_->pending = false;
        return result(DTV_INTERNAL_RESULT_DISCONNECTED);
    }

    const auto address = kCompletionAddress +
        static_cast<std::uint32_t>(cursor_->pending_slot * kRecordSize);
    for (;;) {
        if (cancelled_ != nullptr && cancelled_->load(std::memory_order_acquire))
            return result(DTV_INTERNAL_RESULT_CANCELLED);
        std::uint8_t progress_bytes[4]{};
        transferred = control_read(io_, timeout_, kProgressAddress, progress_bytes);
        if (transferred.code != DTV_INTERNAL_RESULT_OK) return transferred;
        const auto progress = read_be32(progress_bytes);
        if ((progress & 0x80000000) != 0)
            return result(DTV_INTERNAL_RESULT_NOT_READY, progress);
        const auto completed = static_cast<std::uint16_t>(progress);
        if (completed == cursor_->pending_completion_count) {
            const auto now = io_->clock->monotonic_now_ns(io_->clock->user);
            if (timeout_.operation_deadline_ns != 0 && now >= timeout_.operation_deadline_ns)
                return result(DTV_INTERNAL_RESULT_TIMEOUT);
            // Core-clock pacing only; this is not a device timing requirement.
            const auto maximum = std::numeric_limits<std::uint64_t>::max();
            const auto wake = now > maximum - 1000000 ? maximum : now + 1000000;
            std::uint32_t cancelled = 0;
            transferred = io_->clock->delay_until(io_->clock->user, wake,
                timeout_.operation_deadline_ns, nullptr, &cancelled);
            if (cancelled != 0) return result(DTV_INTERNAL_RESULT_CANCELLED);
            if (transferred.code != DTV_INTERNAL_RESULT_OK &&
                transferred.code != DTV_INTERNAL_RESULT_TIMEOUT)
                return transferred;
            continue;
        }
        std::uint8_t record[kRecordSize]{};
        transferred = control_read(io_, timeout_, address, record);
        if (transferred.code != DTV_INTERNAL_RESULT_OK) return transferred;
        const auto first_tag = read_be32(record);
        const auto last_tag = read_be32(record + 60);
        if (first_tag != cursor_->pending_tag || last_tag != cursor_->pending_tag) {
            const auto now = io_->clock->monotonic_now_ns(io_->clock->user);
            if (timeout_.operation_deadline_ns != 0 && now >= timeout_.operation_deadline_ns)
                return result(DTV_INTERNAL_RESULT_USB_IO);
            const auto maximum = std::numeric_limits<std::uint64_t>::max();
            const auto wake = now > maximum - 1000000 ? maximum : now + 1000000;
            std::uint32_t cancelled = 0;
            transferred = io_->clock->delay_until(io_->clock->user, wake,
                timeout_.operation_deadline_ns, nullptr, &cancelled);
            if (cancelled != 0) return result(DTV_INTERNAL_RESULT_CANCELLED);
            if (transferred.code != DTV_INTERNAL_RESULT_OK &&
                transferred.code != DTV_INTERNAL_RESULT_TIMEOUT)
                return transferred;
            continue;
        }
        const auto read_length = read_be32(record + 8);
        const auto status = read_be32(record + 4);
        const auto completion = map_i2c_status(status);
        cursor_->pending = false;
        cursor_->sequence = static_cast<std::uint16_t>(cursor_->sequence + 1);
        if (completion.code != DTV_INTERNAL_RESULT_OK) return completion;
        if (read_length != cursor_->pending_read_length ||
            (!input.empty() && read_length != input.size()))
            return result(DTV_INTERNAL_RESULT_USB_IO);
        if (!input.empty())
            std::copy_n(record + 16, input.size(), input.data());
        return result(DTV_INTERNAL_RESULT_OK);
    }
}

dtv_internal_result CechI2cIo::transact(std::uint8_t address, std::uint16_t operation,
                                        std::span<const std::uint8_t> output,
                                        std::span<std::uint8_t> input)
{
    if (io_ == nullptr || io_->control == nullptr || cursor_ == nullptr ||
        address == 0 || (address & 1) != 0 || output.size() > 32 || input.size() > 32 ||
        (operation == 1 && (output.empty() || !input.empty())) ||
        (operation == 2 && (!output.empty() || input.empty())) ||
        (operation == 3 && (output.empty() || input.empty())) ||
        (operation < 1 || operation > 3))
        return result(DTV_INTERNAL_RESULT_INVALID_ARGUMENT);
    if (cancelled_ != nullptr && cancelled_->load(std::memory_order_acquire))
        return result(DTV_INTERNAL_RESULT_CANCELLED);
    if (cursor_->pending) {
        const auto pending_result = finish_pending({});
        if (pending_result.code != DTV_INTERNAL_RESULT_OK) return pending_result;
    }
    auto transferred = initialize_cursor();
    if (transferred.code != DTV_INTERNAL_RESULT_OK) return transferred;

    std::uint8_t progress_bytes[4]{};
    transferred = control_read(io_, timeout_, kProgressAddress, progress_bytes);
    if (transferred.code != DTV_INTERNAL_RESULT_OK) return transferred;
    const auto progress = read_be32(progress_bytes);
    if ((progress & 0x80000000) != 0)
        return result(DTV_INTERNAL_RESULT_NOT_READY, progress);
    const auto outstanding = static_cast<std::uint16_t>(cursor_->sequence -
        static_cast<std::uint16_t>(progress));
    if (outstanding >= kSlotCount)
        return result(DTV_INTERNAL_RESULT_BUSY, outstanding);

    const auto slot = static_cast<std::size_t>(cursor_->sequence % kSlotCount);
    const auto tag = cursor_->session | cursor_->sequence;
    std::uint8_t request[kRecordSize]{};
    write_be32(request, tag);
    write_be32(request + 4,
        (static_cast<std::uint32_t>(address) << 16) | operation);
    write_be32(request + 8,
        (static_cast<std::uint32_t>(input.size()) << 24) |
        static_cast<std::uint32_t>(output.size()));
    std::copy(output.begin(), output.end(), request + 16);
    write_be32(request + 60, tag);
    transferred = control_write(io_, timeout_, kRequestAddress +
        static_cast<std::uint32_t>(slot * kRecordSize), request);
    if (transferred.code != DTV_INTERNAL_RESULT_OK) return transferred;

    cursor_->pending_tag = tag;
    cursor_->pending_slot = slot;
    cursor_->pending_read_length = input.size();
    cursor_->pending_completion_count = static_cast<std::uint16_t>(progress);
    cursor_->pending = true;
    std::uint8_t producer[4]{};
    write_be32(producer, static_cast<std::uint16_t>(cursor_->sequence + 1));
    transferred = control_write(io_, timeout_, kProducerAddress, producer);
    if (transferred.code != DTV_INTERNAL_RESULT_OK) return transferred;
    return finish_pending(input);
}

}
