#pragma once

// Emulated Bluetooth controller behind the Wii's internal USB module
// (/dev/usb/oh1/57e/305). The game's own Broadcom stack (BTE), WUD, WPAD and KPAD
// talk HCI to it; each of the four virtual Wii Remotes it hosts is backed by a
// physical remote on the PC (see RemoteHost), whose HID reports are passed
// through unchanged. Pure host code: no guest memory, so it is unit-testable.

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <vector>

namespace Bluetooth {

// Bluetooth device address in HCI (least significant byte first) order.
using BdAddr = std::array<uint8_t, 6>;

inline constexpr size_t kRemoteSlots = 4;

enum class LinkState : uint8_t {
    Down,       // no baseband connection to the game
    Connecting, // baseband up, HID channels being opened
    Ready,      // HID control and interrupt channels open
};

// The physical side of the virtual remotes. Called from the thread that drives
// the Controller; implementations synchronize with their own I/O threads.
class RemoteHost {
public:
    virtual ~RemoteHost() = default;
    // A physical remote is attached to `slot` and can carry a connection.
    virtual bool IsAttached(size_t slot) const = 0;
    // True once each time the remote on `slot` asks to connect (it was just
    // attached, or a button was pressed while it had no connection).
    virtual bool ConsumeWake(size_t slot) = 0;
    // Next input report (report id first, no HID header), false when none.
    virtual bool PopInputReport(size_t slot, std::vector<uint8_t>& report) = 0;
    // Output report from the game (report id first, no HID header).
    virtual void SendOutputReport(size_t slot, const uint8_t* report, size_t size) = 0;
    virtual void OnLinkStateChanged(size_t slot, LinkState state) = 0;
    // The game ended a working connection; a real remote powers off here.
    virtual void OnGuestDisconnect(size_t slot) = 0;
    // The game is searching for remotes (its 1+2 / reconnect screens).
    virtual void OnGuestInquiry(bool active) = 0;
    // "Nintendo RVL-CNT-01" or "Nintendo RVL-CNT-01-TR".
    virtual const char* DeviceName(size_t slot) const = 0;
};

class RemoteLink;

class Controller {
public:
    explicit Controller(RemoteHost& host);
    ~Controller();
    Controller(const Controller&) = delete;
    Controller& operator=(const Controller&) = delete;

    // The guest opened / closed the USB device.
    void Open(uint64_t nowUs);
    void Close();
    bool IsOpen() const { return m_open; }

    // Raw HCI command packet (opcode first, no H4 byte).
    void ExecuteCommand(const uint8_t* packet, size_t size, uint64_t nowUs);
    // Raw ACL packet from the guest (handle first).
    void ReceiveAcl(const uint8_t* packet, size_t size, uint64_t nowUs);
    // Advances timers, connection attempts, input forwarding and flow control.
    void Tick(uint64_t nowUs);

    bool HasEvent() const { return !m_events.empty(); }
    bool PopEvent(std::vector<uint8_t>& event);
    // ACL data is only handed out once every queued event has been delivered,
    // so the guest always sees a connection before data on it.
    bool PopAcl(std::vector<uint8_t>& packet);
    size_t AclQueued() const { return m_acl.size(); }

    LinkState SlotLinkState(size_t slot) const;
    bool PageScanEnabled() const { return (m_scanEnable & 0x02) != 0; }
    bool InquiryActive() const { return m_inquiry.active; }

    static BdAddr LocalAddress();
    static BdAddr RemoteAddress(size_t slot);
    static uint16_t ConnectionHandle(size_t slot);

    // Used by RemoteLink.
    void QueueEvent(std::vector<uint8_t> event);
    void QueueAcl(uint16_t handle, const std::vector<uint8_t>& l2capFrame);
    RemoteHost& Host() { return m_host; }
    uint32_t PageTimeoutUs() const { return static_cast<uint32_t>(m_pageTimeoutSlots) * 625u; }

private:
    struct StoredKey {
        BdAddr addr{};
        std::array<uint8_t, 16> key{};
    };
    struct Inquiry {
        bool active = false;
        uint64_t endUs = 0;
        uint8_t maxResponses = 0;
        uint8_t reported = 0;
        std::array<bool, kRemoteSlots> slotReported{};
    };
    struct Reassembly {
        std::vector<uint8_t> frame;
        size_t expected = 0;
    };
    // A reply the controller sends after a delay (page / name request timeouts).
    struct PendingEvent {
        uint64_t dueUs = 0;
        std::vector<uint8_t> event;
    };

    void ResetState(bool keepStoredKeys);
    void CommandComplete(uint16_t opcode, const std::vector<uint8_t>& params);
    void CommandStatus(uint16_t opcode, uint8_t status);
    void TickInquiry(uint64_t nowUs);
    void FinishInquiry();
    void SendNumberOfCompletedPackets();
    int SlotForAddress(const BdAddr& addr) const;
    int SlotForHandle(uint16_t handle) const;
    const StoredKey* FindStoredKey(const BdAddr& addr) const;

    void HandleLinkControl(uint16_t opcode, const uint8_t* p, size_t n, uint64_t nowUs);
    void HandleLinkPolicy(uint16_t opcode, const uint8_t* p, size_t n);
    void HandleControllerBaseband(uint16_t opcode, const uint8_t* p, size_t n, uint64_t nowUs);
    void HandleInformational(uint16_t opcode, const uint8_t* p, size_t n);
    void HandleStatusParameters(uint16_t opcode, const uint8_t* p, size_t n);
    void HandleVendor(uint16_t opcode, const uint8_t* p, size_t n);
    void UnknownCommand(uint16_t opcode);

    RemoteHost& m_host;
    bool m_open = false;
    uint8_t m_scanEnable = 0;
    uint16_t m_pageTimeoutSlots = 0x2000;
    uint8_t m_inquiryMode = 0;
    std::array<uint8_t, 3> m_classOfDevice{};
    std::array<char, 248> m_localName{};
    Inquiry m_inquiry;
    bool m_hostInquiryNotified = false;
    std::vector<StoredKey> m_storedKeys;
    std::array<std::unique_ptr<RemoteLink>, kRemoteSlots> m_links;
    std::array<uint16_t, kRemoteSlots> m_completedPackets{};
    std::array<Reassembly, kRemoteSlots> m_reassembly;
    std::array<bool, kRemoteSlots> m_authPending{};
    std::vector<PendingEvent> m_pending;
    std::deque<std::vector<uint8_t>> m_events;
    std::deque<std::vector<uint8_t>> m_acl;
};

} // namespace Bluetooth
