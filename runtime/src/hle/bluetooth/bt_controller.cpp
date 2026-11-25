#include "bluetooth/bt_controller.h"

#include "bluetooth/bt_bytes.h"
#include "bluetooth/bt_remote_link.h"

#include <algorithm>
#include <cstring>
#include <iostream>

namespace Bluetooth {
namespace HciDetail {


constexpr uint16_t kOgfLinkControl = 0x01;
constexpr uint16_t kOgfLinkPolicy = 0x02;
constexpr uint16_t kOgfControllerBaseband = 0x03;
constexpr uint16_t kOgfInformational = 0x04;
constexpr uint16_t kOgfStatus = 0x05;
constexpr uint16_t kOgfVendor = 0x3F;

constexpr uint8_t kEventInquiryComplete = 0x01;
constexpr uint8_t kEventInquiryResult = 0x02;
constexpr uint8_t kEventConnectionComplete = 0x03;
constexpr uint8_t kEventDisconnectionComplete = 0x05;
constexpr uint8_t kEventAuthenticationComplete = 0x06;
constexpr uint8_t kEventRemoteNameComplete = 0x07;
constexpr uint8_t kEventEncryptionChange = 0x08;
constexpr uint8_t kEventRemoteFeatures = 0x0B;
constexpr uint8_t kEventRemoteVersion = 0x0C;
constexpr uint8_t kEventCommandComplete = 0x0E;
constexpr uint8_t kEventCommandStatus = 0x0F;
constexpr uint8_t kEventRoleChange = 0x12;
constexpr uint8_t kEventNumberOfCompletedPackets = 0x13;
constexpr uint8_t kEventModeChange = 0x14;
constexpr uint8_t kEventReturnLinkKeys = 0x15;
constexpr uint8_t kEventPinCodeRequest = 0x16;
constexpr uint8_t kEventLinkKeyRequest = 0x17;
constexpr uint8_t kEventLinkKeyNotification = 0x18;
constexpr uint8_t kEventClockOffset = 0x1C;
constexpr uint8_t kEventPacketTypeChanged = 0x1D;
constexpr uint8_t kEventInquiryResultWithRssi = 0x22;
constexpr uint8_t kEventExtendedInquiryResult = 0x2F;

constexpr uint8_t kStatusSuccess = 0x00;
constexpr uint8_t kStatusUnknownCommand = 0x01;
constexpr uint8_t kStatusUnknownConnection = 0x02;
constexpr uint8_t kStatusPageTimeout = 0x04;
constexpr uint8_t kStatusAuthenticationFailure = 0x05;
constexpr uint8_t kStatusKeyMissing = 0x06;
constexpr uint8_t kStatusUnsupportedFeature = 0x11;
constexpr uint8_t kReasonLocalHostTerminated = 0x16;

// What a Wii's BCM2045 reports (the version makes WUD apply its usual patch).
constexpr BdAddr kLocalAddress = {0x11, 0x02, 0x19, 0x79, 0x00, 0xFF};
constexpr std::array<uint8_t, 8> kLocalFeatures = {0xFF, 0xFF, 0x8D, 0xFE, 0x9B, 0xF9, 0x00, 0x80};
// Never advertise more than 10 ACL buffers: the stack keeps the count in a u8.
constexpr uint16_t kAclBufferSize = 339;
constexpr uint16_t kAclBufferCount = 10;
constexpr uint8_t kScoBufferSize = 64;

// A real RVL-CNT-01.
constexpr std::array<uint8_t, 8> kRemoteFeatures = {0xBC, 0x02, 0x04, 0x38, 0x08, 0x00, 0x00, 0x00};
constexpr uint8_t kRemoteLmpVersion = 0x02;
constexpr uint16_t kRemoteManufacturer = 0x000F;
constexpr uint16_t kRemoteLmpSubversion = 0x0229;
constexpr uint16_t kRemoteClockOffset = 0x3818;
constexpr uint8_t kInquiryRssi = 0xD8; // -40 dBm
constexpr uint64_t kInquiryUnitUs = 1'280'000;
constexpr uint16_t kMaxStoredKeys = 0x00FF;
constexpr uint8_t kKeysPerEvent = 11;

void PutAddr(std::vector<uint8_t>& out, const BdAddr& addr) {
    out.insert(out.end(), addr.begin(), addr.end());
}

BdAddr ReadAddr(const uint8_t* p) {
    BdAddr addr{};
    std::memcpy(addr.data(), p, addr.size());
    return addr;
}

std::vector<uint8_t> Event(uint8_t code, const std::vector<uint8_t>& params) {
    std::vector<uint8_t> event;
    event.reserve(2 + params.size());
    event.push_back(code);
    event.push_back(static_cast<uint8_t>(params.size()));
    event.insert(event.end(), params.begin(), params.end());
    return event;
}

std::array<uint8_t, 16> SlotLinkKey(size_t slot) {
    std::array<uint8_t, 16> key{};
    for (size_t i = 0; i < key.size(); ++i) {
        key[i] = static_cast<uint8_t>(0xA0 + slot + i * 0x11);
    }
    return key;
}

} // namespace HciDetail

using namespace HciDetail;

Controller::Controller(RemoteHost& host) : m_host(host) {
    for (size_t slot = 0; slot < kRemoteSlots; ++slot) {
        m_links[slot] = std::make_unique<RemoteLink>(*this, slot);
        m_storedKeys.push_back({RemoteAddress(slot), SlotLinkKey(slot)});
    }
    ResetState(true);
}

Controller::~Controller() = default;

BdAddr Controller::LocalAddress() {
    return kLocalAddress;
}

BdAddr Controller::RemoteAddress(size_t slot) {
    return {0x11, 0x02, 0x19, 0x79, 0x00, static_cast<uint8_t>(slot)};
}

uint16_t Controller::ConnectionHandle(size_t slot) {
    return static_cast<uint16_t>(0x0100 + slot);
}

void Controller::Open(uint64_t nowUs) {
    (void)nowUs;
    ResetState(true);
    m_open = true;
}

void Controller::Close() {
    ResetState(true);
    m_open = false;
}

void Controller::ResetState(bool keepStoredKeys) {
    for (size_t slot = 0; slot < kRemoteSlots; ++slot) {
        RemoteLink& link = *m_links[slot];
        // A reset drops the radio links: the remotes lose their connection.
        if (link.State() == LinkState::Ready) {
            m_host.OnGuestDisconnect(slot);
        }
        link.Reset();
    }
    if (!keepStoredKeys) {
        m_storedKeys.clear();
    }
    if (m_hostInquiryNotified) {
        m_host.OnGuestInquiry(false);
        m_hostInquiryNotified = false;
    }
    m_inquiry = {};
    m_scanEnable = 0;
    m_pageTimeoutSlots = 0x2000;
    m_inquiryMode = 0;
    m_classOfDevice = {};
    m_localName = {};
    m_completedPackets = {};
    for (Reassembly& r : m_reassembly) r = {};
    m_events.clear();
    m_acl.clear();
    m_pending.clear();
    m_authPending = {};
}

LinkState Controller::SlotLinkState(size_t slot) const {
    return slot < kRemoteSlots ? m_links[slot]->State() : LinkState::Down;
}

int Controller::SlotForAddress(const BdAddr& addr) const {
    for (size_t slot = 0; slot < kRemoteSlots; ++slot) {
        if (RemoteAddress(slot) == addr) return static_cast<int>(slot);
    }
    return -1;
}

int Controller::SlotForHandle(uint16_t handle) const {
    for (size_t slot = 0; slot < kRemoteSlots; ++slot) {
        if (ConnectionHandle(slot) == (handle & 0x0FFF) &&
            m_links[slot]->BasebandState() == RemoteLink::Baseband::Connected) {
            return static_cast<int>(slot);
        }
    }
    return -1;
}

const Controller::StoredKey* Controller::FindStoredKey(const BdAddr& addr) const {
    for (const StoredKey& key : m_storedKeys) {
        if (key.addr == addr) return &key;
    }
    return nullptr;
}

void Controller::QueueEvent(std::vector<uint8_t> event) {
    m_events.push_back(std::move(event));
}

void Controller::QueueAcl(uint16_t handle, const std::vector<uint8_t>& l2capFrame) {
    std::vector<uint8_t> packet;
    packet.reserve(4 + l2capFrame.size());
    // Packet boundary 0x2: first (and only) fragment, automatically flushable.
    PutLe16(packet, static_cast<uint16_t>((handle & 0x0FFF) | 0x2000));
    PutLe16(packet, static_cast<uint16_t>(l2capFrame.size()));
    packet.insert(packet.end(), l2capFrame.begin(), l2capFrame.end());
    m_acl.push_back(std::move(packet));
}

bool Controller::PopEvent(std::vector<uint8_t>& event) {
    if (m_events.empty()) return false;
    event = std::move(m_events.front());
    m_events.pop_front();
    return true;
}

bool Controller::PopAcl(std::vector<uint8_t>& packet) {
    if (!m_events.empty() || m_acl.empty()) return false;
    packet = std::move(m_acl.front());
    m_acl.pop_front();
    return true;
}

void Controller::CommandComplete(uint16_t opcode, const std::vector<uint8_t>& params) {
    std::vector<uint8_t> body = {0x01};
    PutLe16(body, opcode);
    body.insert(body.end(), params.begin(), params.end());
    QueueEvent(Event(kEventCommandComplete, body));
}

void Controller::CommandStatus(uint16_t opcode, uint8_t status) {
    std::vector<uint8_t> body = {status, 0x01};
    PutLe16(body, opcode);
    QueueEvent(Event(kEventCommandStatus, body));
}

void Controller::UnknownCommand(uint16_t opcode) {
    std::cerr << "[bluetooth] unsupported HCI command 0x" << std::hex << opcode << std::dec << std::endl;
    CommandStatus(opcode, kStatusUnknownCommand);
}

void Controller::ExecuteCommand(const uint8_t* packet, size_t size, uint64_t nowUs) {
    if (size < 3) return;
    const uint16_t opcode = Le16(packet);
    const size_t length = packet[2];
    if (3 + length > size) {
        std::cerr << "[bluetooth] truncated HCI command 0x" << std::hex << opcode << std::dec << std::endl;
        return;
    }
    const uint8_t* p = packet + 3;
    switch (opcode >> 10) {
    case kOgfLinkControl: HandleLinkControl(opcode, p, length, nowUs); break;
    case kOgfLinkPolicy: HandleLinkPolicy(opcode, p, length); break;
    case kOgfControllerBaseband: HandleControllerBaseband(opcode, p, length, nowUs); break;
    case kOgfInformational: HandleInformational(opcode, p, length); break;
    case kOgfStatus: HandleStatusParameters(opcode, p, length); break;
    case kOgfVendor: HandleVendor(opcode, p, length); break;
    default: UnknownCommand(opcode); break;
    }
}

void Controller::HandleLinkControl(uint16_t opcode, const uint8_t* p, size_t n, uint64_t nowUs) {
    const auto handleSlot = [&](size_t offset) { return n >= offset + 2 ? SlotForHandle(Le16(p + offset)) : -1; };
    switch (opcode & 0x3FF) {
    case 0x0001: { // Inquiry
        if (n < 5) break;
        CommandStatus(opcode, kStatusSuccess);
        m_inquiry = {};
        m_inquiry.active = true;
        m_inquiry.endUs = nowUs + std::max<uint8_t>(p[3], 1) * kInquiryUnitUs;
        m_inquiry.maxResponses = p[4];
        if (!m_hostInquiryNotified) {
            m_host.OnGuestInquiry(true);
            m_hostInquiryNotified = true;
        }
        return;
    }
    case 0x0002: // Inquiry Cancel
        m_inquiry = {};
        if (m_hostInquiryNotified) {
            m_host.OnGuestInquiry(false);
            m_hostInquiryNotified = false;
        }
        CommandComplete(opcode, {kStatusSuccess});
        return;
    case 0x0003: // Periodic Inquiry Mode
    case 0x0004: // Exit Periodic Inquiry Mode
        CommandComplete(opcode, {kStatusUnsupportedFeature});
        return;
    case 0x0005: { // Create Connection
        if (n < 6) break;
        const BdAddr addr = ReadAddr(p);
        CommandStatus(opcode, kStatusSuccess);
        const int slot = SlotForAddress(addr);
        if (slot >= 0 && m_links[slot]->OnPaged(nowUs)) {
            std::vector<uint8_t> body = {kStatusSuccess};
            PutLe16(body, ConnectionHandle(slot));
            PutAddr(body, addr);
            body.insert(body.end(), {0x01, 0x00});
            QueueEvent(Event(kEventConnectionComplete, body));
        } else {
            std::vector<uint8_t> body = {kStatusPageTimeout, 0x00, 0x00};
            PutAddr(body, addr);
            body.insert(body.end(), {0x01, 0x00});
            m_pending.push_back({nowUs + PageTimeoutUs(), Event(kEventConnectionComplete, body)});
        }
        return;
    }
    case 0x0006: { // Disconnect
        const int slot = handleSlot(0);
        if (slot < 0 || n < 3) {
            CommandStatus(opcode, kStatusUnknownConnection);
            return;
        }
        CommandStatus(opcode, kStatusSuccess);
        std::vector<uint8_t> body = {kStatusSuccess};
        PutLe16(body, ConnectionHandle(slot));
        body.push_back(kReasonLocalHostTerminated);
        QueueEvent(Event(kEventDisconnectionComplete, body));
        m_completedPackets[slot] = 0;
        m_reassembly[slot] = {};
        m_authPending[slot] = false;
        m_links[slot]->OnDisconnected(true, nowUs);
        return;
    }
    case 0x0008: { // Create Connection Cancel
        if (n < 6) break;
        const BdAddr addr = ReadAddr(p);
        auto it = std::find_if(m_pending.begin(), m_pending.end(), [&](const PendingEvent& e) {
            return e.event[0] == kEventConnectionComplete && ReadAddr(&e.event[5]) == addr;
        });
        std::vector<uint8_t> body = {static_cast<uint8_t>(it == m_pending.end() ? kStatusUnknownConnection
                                                                                      : kStatusSuccess)};
        PutAddr(body, addr);
        CommandComplete(opcode, body);
        if (it != m_pending.end()) {
            std::vector<uint8_t> complete = it->event;
            complete[2] = kStatusUnknownConnection;
            m_pending.erase(it);
            QueueEvent(std::move(complete));
        }
        return;
    }
    case 0x0009: { // Accept Connection Request
        if (n < 7) break;
        const BdAddr addr = ReadAddr(p);
        const uint8_t role = p[6];
        const int slot = SlotForAddress(addr);
        if (slot < 0 || !m_links[slot]->IsRequesting()) {
            CommandStatus(opcode, kStatusUnknownConnection);
            return;
        }
        CommandStatus(opcode, kStatusSuccess);
        if (role == 0x00) {
            std::vector<uint8_t> change = {kStatusSuccess};
            PutAddr(change, addr);
            change.push_back(0x00);
            QueueEvent(Event(kEventRoleChange, change));
        }
        std::vector<uint8_t> body = {kStatusSuccess};
        PutLe16(body, ConnectionHandle(slot));
        PutAddr(body, addr);
        body.insert(body.end(), {0x01, 0x00});
        QueueEvent(Event(kEventConnectionComplete, body));
        m_completedPackets[slot] = 0;
        m_reassembly[slot] = {};
        m_links[slot]->OnAccepted(role);
        return;
    }
    case 0x000A: { // Reject Connection Request
        if (n < 7) break;
        const BdAddr addr = ReadAddr(p);
        const int slot = SlotForAddress(addr);
        if (slot < 0 || !m_links[slot]->IsRequesting()) {
            CommandStatus(opcode, kStatusUnknownConnection);
            return;
        }
        CommandStatus(opcode, kStatusSuccess);
        std::vector<uint8_t> body = {p[6], 0x00, 0x00};
        PutAddr(body, addr);
        body.insert(body.end(), {0x01, 0x00});
        QueueEvent(Event(kEventConnectionComplete, body));
        m_links[slot]->OnRejected(p[6], nowUs);
        return;
    }
    case 0x000B: { // Link Key Request Reply
        if (n < 22) break;
        const BdAddr addr = ReadAddr(p);
        std::vector<uint8_t> body = {kStatusSuccess};
        PutAddr(body, addr);
        CommandComplete(opcode, body);
        const int slot = SlotForAddress(addr);
        if (slot >= 0 && m_authPending[slot]) {
            m_authPending[slot] = false;
            const bool match = std::equal(p + 6, p + 22, SlotLinkKey(slot).begin());
            std::vector<uint8_t> auth = {static_cast<uint8_t>(match ? kStatusSuccess : kStatusKeyMissing)};
            PutLe16(auth, ConnectionHandle(slot));
            QueueEvent(Event(kEventAuthenticationComplete, auth));
        }
        return;
    }
    case 0x000C: { // Link Key Request Negative Reply
        if (n < 6) break;
        const BdAddr addr = ReadAddr(p);
        std::vector<uint8_t> body = {kStatusSuccess};
        PutAddr(body, addr);
        CommandComplete(opcode, body);
        const int slot = SlotForAddress(addr);
        if (slot >= 0 && m_authPending[slot]) {
            std::vector<uint8_t> request;
            PutAddr(request, addr);
            QueueEvent(Event(kEventPinCodeRequest, request));
        }
        return;
    }
    case 0x000D: { // PIN Code Request Reply
        if (n < 23) break;
        const BdAddr addr = ReadAddr(p);
        std::vector<uint8_t> body = {kStatusSuccess};
        PutAddr(body, addr);
        CommandComplete(opcode, body);
        const int slot = SlotForAddress(addr);
        if (slot < 0 || !m_authPending[slot]) return;
        m_authPending[slot] = false;
        // A remote takes the host's address (SYNC button) or its own (1+2) as PIN.
        const uint8_t pinLength = p[6];
        const uint8_t* pin = p + 7;
        const BdAddr local = LocalAddress();
        const bool valid = pinLength == 6 && (std::equal(pin, pin + 6, local.begin()) ||
                                              std::equal(pin, pin + 6, addr.begin()));
        if (valid) {
            std::vector<uint8_t> notification;
            PutAddr(notification, addr);
            const auto key = SlotLinkKey(slot);
            notification.insert(notification.end(), key.begin(), key.end());
            notification.push_back(0x00); // combination key
            QueueEvent(Event(kEventLinkKeyNotification, notification));
        }
        std::vector<uint8_t> auth = {static_cast<uint8_t>(valid ? kStatusSuccess : kStatusAuthenticationFailure)};
        PutLe16(auth, ConnectionHandle(slot));
        QueueEvent(Event(kEventAuthenticationComplete, auth));
        return;
    }
    case 0x000E: { // PIN Code Request Negative Reply
        if (n < 6) break;
        const BdAddr addr = ReadAddr(p);
        std::vector<uint8_t> body = {kStatusSuccess};
        PutAddr(body, addr);
        CommandComplete(opcode, body);
        const int slot = SlotForAddress(addr);
        if (slot >= 0 && m_authPending[slot]) {
            m_authPending[slot] = false;
            std::vector<uint8_t> auth = {kStatusKeyMissing};
            PutLe16(auth, ConnectionHandle(slot));
            QueueEvent(Event(kEventAuthenticationComplete, auth));
        }
        return;
    }
    case 0x000F: { // Change Connection Packet Type
        const int slot = handleSlot(0);
        if (slot < 0 || n < 4) {
            CommandStatus(opcode, kStatusUnknownConnection);
            return;
        }
        CommandStatus(opcode, kStatusSuccess);
        std::vector<uint8_t> body = {kStatusSuccess};
        PutLe16(body, ConnectionHandle(slot));
        PutLe16(body, Le16(p + 2));
        QueueEvent(Event(kEventPacketTypeChanged, body));
        return;
    }
    case 0x0011: { // Authentication Requested
        const int slot = handleSlot(0);
        if (slot < 0) {
            CommandStatus(opcode, kStatusUnknownConnection);
            return;
        }
        CommandStatus(opcode, kStatusSuccess);
        m_authPending[slot] = true;
        std::vector<uint8_t> request;
        PutAddr(request, RemoteAddress(slot));
        QueueEvent(Event(kEventLinkKeyRequest, request));
        return;
    }
    case 0x0013: { // Set Connection Encryption
        const int slot = handleSlot(0);
        if (slot < 0 || n < 3) {
            CommandStatus(opcode, kStatusUnknownConnection);
            return;
        }
        CommandStatus(opcode, kStatusSuccess);
        std::vector<uint8_t> body = {kStatusSuccess};
        PutLe16(body, ConnectionHandle(slot));
        body.push_back(p[2]);
        QueueEvent(Event(kEventEncryptionChange, body));
        return;
    }
    case 0x0019: { // Remote Name Request
        if (n < 6) break;
        const BdAddr addr = ReadAddr(p);
        CommandStatus(opcode, kStatusSuccess);
        const int slot = SlotForAddress(addr);
        const bool reachable = slot >= 0 && (m_links[slot]->BasebandState() == RemoteLink::Baseband::Connected ||
                                             m_links[slot]->Discoverable());
        std::vector<uint8_t> body = {static_cast<uint8_t>(reachable ? kStatusSuccess : kStatusPageTimeout)};
        PutAddr(body, addr);
        std::array<char, 248> name{};
        if (reachable) {
            std::strncpy(name.data(), m_host.DeviceName(slot), name.size() - 1);
        }
        body.insert(body.end(), name.begin(), name.end());
        if (reachable) {
            QueueEvent(Event(kEventRemoteNameComplete, body));
        } else {
            m_pending.push_back({nowUs + PageTimeoutUs(), Event(kEventRemoteNameComplete, body)});
        }
        return;
    }
    case 0x001A: { // Remote Name Request Cancel
        if (n < 6) break;
        const BdAddr addr = ReadAddr(p);
        auto it = std::find_if(m_pending.begin(), m_pending.end(), [&](const PendingEvent& e) {
            return e.event[0] == kEventRemoteNameComplete && ReadAddr(&e.event[3]) == addr;
        });
        std::vector<uint8_t> body = {static_cast<uint8_t>(it == m_pending.end() ? kStatusUnknownConnection
                                                                                      : kStatusSuccess)};
        PutAddr(body, addr);
        CommandComplete(opcode, body);
        if (it != m_pending.end()) {
            std::vector<uint8_t> complete = it->event;
            complete[2] = kStatusUnknownConnection;
            m_pending.erase(it);
            QueueEvent(std::move(complete));
        }
        return;
    }
    case 0x001B: { // Read Remote Supported Features
        const int slot = handleSlot(0);
        if (slot < 0) {
            CommandStatus(opcode, kStatusUnknownConnection);
            return;
        }
        CommandStatus(opcode, kStatusSuccess);
        std::vector<uint8_t> body = {kStatusSuccess};
        PutLe16(body, ConnectionHandle(slot));
        body.insert(body.end(), kRemoteFeatures.begin(), kRemoteFeatures.end());
        QueueEvent(Event(kEventRemoteFeatures, body));
        return;
    }
    case 0x001D: { // Read Remote Version Information
        const int slot = handleSlot(0);
        if (slot < 0) {
            CommandStatus(opcode, kStatusUnknownConnection);
            return;
        }
        CommandStatus(opcode, kStatusSuccess);
        std::vector<uint8_t> body = {kStatusSuccess};
        PutLe16(body, ConnectionHandle(slot));
        body.push_back(kRemoteLmpVersion);
        PutLe16(body, kRemoteManufacturer);
        PutLe16(body, kRemoteLmpSubversion);
        QueueEvent(Event(kEventRemoteVersion, body));
        return;
    }
    case 0x001F: { // Read Clock Offset
        const int slot = handleSlot(0);
        if (slot < 0) {
            CommandStatus(opcode, kStatusUnknownConnection);
            return;
        }
        CommandStatus(opcode, kStatusSuccess);
        std::vector<uint8_t> body = {kStatusSuccess};
        PutLe16(body, ConnectionHandle(slot));
        PutLe16(body, kRemoteClockOffset);
        QueueEvent(Event(kEventClockOffset, body));
        return;
    }
    case 0x0007: // Add SCO Connection
    case 0x0028: // Setup Synchronous Connection
    case 0x0029: // Accept Synchronous Connection Request
    case 0x002A: // Reject Synchronous Connection Request
        CommandStatus(opcode, kStatusUnsupportedFeature);
        return;
    default:
        UnknownCommand(opcode);
        return;
    }
    std::cerr << "[bluetooth] short HCI command 0x" << std::hex << opcode << std::dec << std::endl;
    CommandStatus(opcode, kStatusUnknownCommand);
}

void Controller::HandleLinkPolicy(uint16_t opcode, const uint8_t* p, size_t n) {
    const int slot = n >= 2 ? SlotForHandle(Le16(p)) : -1;
    const auto modeChange = [&](uint8_t mode, uint16_t interval) {
        if (slot < 0) {
            CommandStatus(opcode, kStatusUnknownConnection);
            return;
        }
        CommandStatus(opcode, kStatusSuccess);
        std::vector<uint8_t> body = {kStatusSuccess};
        PutLe16(body, ConnectionHandle(static_cast<size_t>(slot)));
        body.push_back(mode);
        PutLe16(body, interval);
        QueueEvent(Event(kEventModeChange, body));
    };
    const auto handleReply = [&](std::vector<uint8_t> extra) {
        std::vector<uint8_t> body = {static_cast<uint8_t>(slot < 0 ? kStatusUnknownConnection : kStatusSuccess)};
        PutLe16(body, n >= 2 ? Le16(p) : 0);
        body.insert(body.end(), extra.begin(), extra.end());
        CommandComplete(opcode, body);
    };
    switch (opcode & 0x3FF) {
    case 0x0001: modeChange(0x01, n >= 4 ? Le16(p + 2) : 0); return; // Hold Mode
    case 0x0003: modeChange(0x02, n >= 4 ? Le16(p + 2) : 0); return; // Sniff Mode
    case 0x0004: modeChange(0x00, 0); return;                        // Exit Sniff Mode
    case 0x0005: modeChange(0x03, n >= 4 ? Le16(p + 2) : 0); return; // Park State
    case 0x0006: modeChange(0x00, 0); return;                        // Exit Park State
    case 0x0009: handleReply({0x00}); return;                        // Role Discovery
    case 0x000B: { // Switch Role
        if (n < 7) break;
        CommandStatus(opcode, kStatusSuccess);
        std::vector<uint8_t> body = {kStatusSuccess};
        body.insert(body.end(), p, p + 7);
        QueueEvent(Event(kEventRoleChange, body));
        return;
    }
    case 0x000C: handleReply({0x05, 0x00}); return; // Read Link Policy Settings
    case 0x000D: handleReply({}); return;           // Write Link Policy Settings
    case 0x000E: CommandComplete(opcode, {kStatusSuccess, 0x05, 0x00}); return; // Read Default Link Policy
    case 0x000F: CommandComplete(opcode, {kStatusSuccess}); return;            // Write Default Link Policy
    default:
        UnknownCommand(opcode);
        return;
    }
    CommandStatus(opcode, kStatusUnknownCommand);
}

void Controller::HandleControllerBaseband(uint16_t opcode, const uint8_t* p, size_t n, uint64_t nowUs) {
    (void)nowUs;
    const auto handleReply = [&](std::vector<uint8_t> extra) {
        const int slot = n >= 2 ? SlotForHandle(Le16(p)) : -1;
        std::vector<uint8_t> body = {static_cast<uint8_t>(slot < 0 ? kStatusUnknownConnection : kStatusSuccess)};
        PutLe16(body, n >= 2 ? Le16(p) : 0);
        body.insert(body.end(), extra.begin(), extra.end());
        CommandComplete(opcode, body);
    };
    switch (opcode & 0x3FF) {
    case 0x0003: // Reset
        ResetState(true);
        CommandComplete(opcode, {kStatusSuccess});
        return;
    case 0x000D: { // Read Stored Link Key
        if (n < 7) break;
        const BdAddr addr = ReadAddr(p);
        const bool readAll = p[6] == 0x01;
        std::vector<StoredKey> keys;
        for (const StoredKey& key : m_storedKeys) {
            if (readAll || key.addr == addr) keys.push_back(key);
        }
        for (size_t start = 0; start < keys.size(); start += kKeysPerEvent) {
            const size_t count = std::min<size_t>(kKeysPerEvent, keys.size() - start);
            std::vector<uint8_t> body = {static_cast<uint8_t>(count)};
            for (size_t i = start; i < start + count; ++i) {
                PutAddr(body, keys[i].addr);
                body.insert(body.end(), keys[i].key.begin(), keys[i].key.end());
            }
            QueueEvent(Event(kEventReturnLinkKeys, body));
        }
        std::vector<uint8_t> body = {kStatusSuccess};
        PutLe16(body, kMaxStoredKeys);
        PutLe16(body, static_cast<uint16_t>(keys.size()));
        CommandComplete(opcode, body);
        return;
    }
    case 0x0011: { // Write Stored Link Key
        if (n < 1) break;
        const uint8_t count = p[0];
        if (n < 1 + count * size_t{22}) break;
        for (uint8_t i = 0; i < count; ++i) {
            const uint8_t* entry = p + 1 + i * 22;
            StoredKey key{ReadAddr(entry), {}};
            std::copy(entry + 6, entry + 22, key.key.begin());
            auto existing = std::find_if(m_storedKeys.begin(), m_storedKeys.end(),
                                         [&](const StoredKey& k) { return k.addr == key.addr; });
            if (existing != m_storedKeys.end()) {
                *existing = key;
            } else if (m_storedKeys.size() < kMaxStoredKeys) {
                m_storedKeys.push_back(key);
            }
        }
        CommandComplete(opcode, {kStatusSuccess, count});
        return;
    }
    case 0x0012: { // Delete Stored Link Key
        if (n < 7) break;
        const BdAddr addr = ReadAddr(p);
        const bool deleteAll = p[6] == 0x01;
        const size_t before = m_storedKeys.size();
        m_storedKeys.erase(std::remove_if(m_storedKeys.begin(), m_storedKeys.end(),
                                          [&](const StoredKey& k) { return deleteAll || k.addr == addr; }),
                           m_storedKeys.end());
        std::vector<uint8_t> body = {kStatusSuccess};
        PutLe16(body, static_cast<uint16_t>(before - m_storedKeys.size()));
        CommandComplete(opcode, body);
        return;
    }
    case 0x0013: // Change Local Name
        if (n < m_localName.size()) break;
        std::memcpy(m_localName.data(), p, m_localName.size());
        CommandComplete(opcode, {kStatusSuccess});
        return;
    case 0x0014: { // Read Local Name
        std::vector<uint8_t> body = {kStatusSuccess};
        body.insert(body.end(), m_localName.begin(), m_localName.end());
        CommandComplete(opcode, body);
        return;
    }
    case 0x0018: // Write Page Timeout
        if (n < 2) break;
        m_pageTimeoutSlots = Le16(p);
        CommandComplete(opcode, {kStatusSuccess});
        return;
    case 0x0019: // Read Scan Enable
        CommandComplete(opcode, {kStatusSuccess, m_scanEnable});
        return;
    case 0x001A: // Write Scan Enable
        if (n < 1) break;
        m_scanEnable = p[0];
        CommandComplete(opcode, {kStatusSuccess});
        return;
    case 0x0023: // Read Class of Device
        CommandComplete(opcode, {kStatusSuccess, m_classOfDevice[0], m_classOfDevice[1], m_classOfDevice[2]});
        return;
    case 0x0024: // Write Class of Device
        if (n < 3) break;
        std::copy(p, p + 3, m_classOfDevice.begin());
        CommandComplete(opcode, {kStatusSuccess});
        return;
    case 0x0045: // Write Inquiry Mode
        if (n < 1) break;
        m_inquiryMode = p[0];
        CommandComplete(opcode, {kStatusSuccess});
        return;
    case 0x0028: // Write Automatic Flush Timeout
    case 0x0037: // Write Link Supervision Timeout
        handleReply({});
        return;
    case 0x0036: // Read Link Supervision Timeout
        handleReply({0x80, 0x0C});
        return;
    case 0x0001: // Set Event Mask
    case 0x0005: // Set Event Filter
    case 0x000A: // Write PIN Type
    case 0x0016: // Write Connection Accept Timeout
    case 0x001C: // Write Page Scan Activity
    case 0x001E: // Write Inquiry Scan Activity
    case 0x0020: // Write Authentication Enable
    case 0x0022: // Write Encryption Mode
    case 0x0031: // Set Controller To Host Flow Control
    case 0x0033: // Host Buffer Size
    case 0x003A: // Write Current IAC LAP
    case 0x003F: // Set AFH Host Channel Classification
    case 0x0043: // Write Inquiry Scan Type
    case 0x0047: // Write Page Scan Type
        CommandComplete(opcode, {kStatusSuccess});
        return;
    case 0x0035: // Host Number Of Completed Packets: no event
        return;
    default:
        UnknownCommand(opcode);
        return;
    }
    std::cerr << "[bluetooth] short HCI command 0x" << std::hex << opcode << std::dec << std::endl;
    CommandComplete(opcode, {kStatusUnknownCommand});
}

void Controller::HandleInformational(uint16_t opcode, const uint8_t* p, size_t n) {
    (void)p;
    (void)n;
    switch (opcode & 0x3FF) {
    case 0x0001: { // Read Local Version Information
        std::vector<uint8_t> body = {kStatusSuccess, 0x03};
        PutLe16(body, 0x40A7);
        body.push_back(0x03);
        PutLe16(body, 0x000F);
        PutLe16(body, 0x430E);
        CommandComplete(opcode, body);
        return;
    }
    case 0x0003: { // Read Local Supported Features
        std::vector<uint8_t> body = {kStatusSuccess};
        body.insert(body.end(), kLocalFeatures.begin(), kLocalFeatures.end());
        CommandComplete(opcode, body);
        return;
    }
    case 0x0005: { // Read Buffer Size
        std::vector<uint8_t> body = {kStatusSuccess};
        PutLe16(body, kAclBufferSize);
        body.push_back(kScoBufferSize);
        PutLe16(body, kAclBufferCount);
        PutLe16(body, 0);
        CommandComplete(opcode, body);
        return;
    }
    case 0x0009: { // Read BD_ADDR
        std::vector<uint8_t> body = {kStatusSuccess};
        PutAddr(body, LocalAddress());
        CommandComplete(opcode, body);
        return;
    }
    default:
        UnknownCommand(opcode);
        return;
    }
}

void Controller::HandleStatusParameters(uint16_t opcode, const uint8_t* p, size_t n) {
    const int slot = n >= 2 ? SlotForHandle(Le16(p)) : -1;
    std::vector<uint8_t> body = {static_cast<uint8_t>(slot < 0 ? kStatusUnknownConnection : kStatusSuccess)};
    PutLe16(body, n >= 2 ? Le16(p) : 0);
    switch (opcode & 0x3FF) {
    case 0x0003: // Get Link Quality
        body.push_back(0xFF);
        CommandComplete(opcode, body);
        return;
    case 0x0005: // Read RSSI: 0 = inside the golden receive power range
        body.push_back(0x00);
        CommandComplete(opcode, body);
        return;
    default:
        UnknownCommand(opcode);
        return;
    }
}

void Controller::HandleVendor(uint16_t opcode, const uint8_t* p, size_t n) {
    (void)p;
    (void)n;
    // WUD's firmware patch upload (0xFC4C writes, 0xFC4F install) and friends.
    CommandComplete(opcode, {kStatusSuccess});
}

void Controller::ReceiveAcl(const uint8_t* packet, size_t size, uint64_t nowUs) {
    (void)nowUs;
    if (size < 4) return;
    const uint16_t header = Le16(packet);
    const uint16_t length = Le16(packet + 2);
    if (length != size - 4) {
        std::cerr << "[bluetooth] dropped malformed ACL packet" << std::endl;
        return;
    }
    const int slot = SlotForHandle(header & 0x0FFF);
    if (slot < 0) return;
    ++m_completedPackets[slot];
    const uint8_t boundary = static_cast<uint8_t>((header >> 12) & 0x3);
    Reassembly& r = m_reassembly[slot];
    const uint8_t* data = packet + 4;
    if (boundary == 0x1) {
        if (r.expected == 0) return;
        r.frame.insert(r.frame.end(), data, data + length);
    } else {
        r.frame.assign(data, data + length);
        r.expected = length >= 2 ? static_cast<size_t>(Le16(data)) + 4 : 0;
    }
    if (r.expected != 0 && r.frame.size() >= r.expected) {
        const std::vector<uint8_t> frame(r.frame.begin(), r.frame.begin() + static_cast<std::ptrdiff_t>(r.expected));
        r = {};
        m_links[slot]->ReceiveL2cap(frame.data(), frame.size());
    }
}

void Controller::Tick(uint64_t nowUs) {
    if (!m_open) return;
    for (size_t i = 0; i < m_pending.size();) {
        if (nowUs >= m_pending[i].dueUs) {
            QueueEvent(std::move(m_pending[i].event));
            m_pending.erase(m_pending.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
    TickInquiry(nowUs);
    for (auto& link : m_links) {
        link->Tick(nowUs);
    }
    SendNumberOfCompletedPackets();
}

void Controller::TickInquiry(uint64_t nowUs) {
    if (!m_inquiry.active) return;
    if (nowUs >= m_inquiry.endUs ||
        (m_inquiry.maxResponses != 0 && m_inquiry.reported >= m_inquiry.maxResponses)) {
        FinishInquiry();
        return;
    }
    // One result per tick, lowest slot first, so remotes keep their order.
    for (size_t slot = 0; slot < kRemoteSlots; ++slot) {
        if (m_inquiry.slotReported[slot] || !m_links[slot]->Discoverable()) continue;
        m_inquiry.slotReported[slot] = true;
        ++m_inquiry.reported;
        const BdAddr addr = RemoteAddress(slot);
        std::vector<uint8_t> body = {0x01};
        PutAddr(body, addr);
        body.push_back(0x01); // page scan repetition mode R1
        if (m_inquiryMode == 0) {
            body.insert(body.end(), {0x00, 0x00, 0x04, 0x25, 0x00});
            PutLe16(body, kRemoteClockOffset);
            QueueEvent(Event(kEventInquiryResult, body));
        } else {
            body.insert(body.end(), {0x00, 0x04, 0x25, 0x00});
            PutLe16(body, kRemoteClockOffset);
            body.push_back(kInquiryRssi);
            if (m_inquiryMode == 2) {
                std::array<uint8_t, 240> eir{};
                const char* name = m_host.DeviceName(slot);
                const size_t nameLength = std::min<size_t>(std::strlen(name), eir.size() - 2);
                eir[0] = static_cast<uint8_t>(nameLength + 1);
                eir[1] = 0x09; // complete local name
                std::memcpy(&eir[2], name, nameLength);
                body.insert(body.end(), eir.begin(), eir.end());
                QueueEvent(Event(kEventExtendedInquiryResult, body));
            } else {
                QueueEvent(Event(kEventInquiryResultWithRssi, body));
            }
        }
        return;
    }
}

void Controller::FinishInquiry() {
    m_inquiry = {};
    QueueEvent(Event(kEventInquiryComplete, {kStatusSuccess}));
    if (m_hostInquiryNotified) {
        m_host.OnGuestInquiry(false);
        m_hostInquiryNotified = false;
    }
}

void Controller::SendNumberOfCompletedPackets() {
    std::vector<uint8_t> body = {0};
    for (size_t slot = 0; slot < kRemoteSlots; ++slot) {
        if (m_completedPackets[slot] == 0) continue;
        PutLe16(body, ConnectionHandle(slot));
        PutLe16(body, m_completedPackets[slot]);
        m_completedPackets[slot] = 0;
        ++body[0];
    }
    if (body[0] != 0) {
        QueueEvent(Event(kEventNumberOfCompletedPackets, body));
    }
}

} // namespace Bluetooth
