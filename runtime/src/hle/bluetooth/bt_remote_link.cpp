#include "bluetooth/bt_remote_link.h"

#include "bluetooth/bt_bytes.h"
#include "bluetooth/bt_sdp.h"

#include <algorithm>
#include <iostream>

namespace Bluetooth {
namespace L2capDetail {

constexpr uint16_t kSignallingCid = 0x0001;
constexpr uint16_t kFirstDynamicCid = 0x0040;

constexpr uint16_t kPsmSdp = 0x0001;
constexpr uint16_t kPsmHidControl = 0x0011;
constexpr uint16_t kPsmHidInterrupt = 0x0013;

constexpr uint8_t kCommandReject = 0x01;
constexpr uint8_t kConnectionRequest = 0x02;
constexpr uint8_t kConnectionResponse = 0x03;
constexpr uint8_t kConfigurationRequest = 0x04;
constexpr uint8_t kConfigurationResponse = 0x05;
constexpr uint8_t kDisconnectionRequest = 0x06;
constexpr uint8_t kDisconnectionResponse = 0x07;
constexpr uint8_t kEchoRequest = 0x08;
constexpr uint8_t kEchoResponse = 0x09;
constexpr uint8_t kInformationRequest = 0x0A;
constexpr uint8_t kInformationResponse = 0x0B;

constexpr uint16_t kResultSuccess = 0x0000;
constexpr uint16_t kResultPending = 0x0001;
constexpr uint16_t kResultPsmNotSupported = 0x0002;
constexpr uint16_t kResultNoResources = 0x0004;

constexpr uint8_t kOptionMtu = 0x01;
constexpr uint8_t kOptionFlushTimeout = 0x02;
// A real remote asks for this MTU on its own channels.
constexpr uint16_t kRemoteMtu = 185;

constexpr uint8_t kHidHandshake = 0x0;
constexpr uint8_t kHidControl = 0x1;
constexpr uint8_t kHidSetReport = 0x5;
constexpr uint8_t kHidData = 0xA;
constexpr uint8_t kHidParamOutput = 0x2;
constexpr uint8_t kHidHandshakeSuccess = 0x00;
constexpr uint8_t kHidHandshakeUnsupported = 0x03;
constexpr uint8_t kHidVirtualCableUnplug = 0x5;
constexpr uint8_t kHidDataInput = 0xA1;

constexpr uint64_t kRequestTimeoutUs = 10'000'000;
constexpr uint64_t kRetryDelayUs = 3'000'000;
constexpr uint8_t kReasonRemoteUserTerminated = 0x13;
// Output reports carry at most 21 payload bytes; anything longer is not a Wii Remote report.
constexpr size_t kMaxReportSize = 22;
constexpr size_t kInputBacklog = 8;

} // namespace L2capDetail

using namespace L2capDetail;

RemoteLink::RemoteLink(Controller& controller, size_t slot) : m_controller(controller), m_slot(slot) {}

void RemoteLink::Reset() {
    ClearConnection();
    m_baseband = Baseband::Idle;
    SetState(LinkState::Down);
    m_retryAtUs = 0;
}

void RemoteLink::ClearConnection() {
    m_channels.clear();
    m_guestInitiated = false;
    m_everReady = false;
}

bool RemoteLink::Discoverable() const {
    return m_baseband == Baseband::Idle && m_connectWanted && m_controller.Host().IsAttached(m_slot);
}

void RemoteLink::Tick(uint64_t nowUs) {
    m_nowUs = nowUs;
    RemoteHost& host = m_controller.Host();
    if (m_baseband == Baseband::Idle) {
        if (host.ConsumeWake(m_slot)) {
            m_connectWanted = true;
            m_retryAtUs = 0;
        }
        if (!host.IsAttached(m_slot)) {
            m_connectWanted = false;
        }
        if (m_connectWanted && m_controller.PageScanEnabled() && nowUs >= m_retryAtUs) {
            std::vector<uint8_t> event = {0x04, 10};
            const BdAddr addr = Controller::RemoteAddress(m_slot);
            event.insert(event.end(), addr.begin(), addr.end());
            // Class of device 0x002504: peripheral, joystick.
            event.insert(event.end(), {0x04, 0x25, 0x00, 0x01});
            m_controller.QueueEvent(std::move(event));
            m_baseband = Baseband::Requesting;
            m_stateSinceUs = nowUs;
        }
    } else if (m_baseband == Baseband::Requesting) {
        if (nowUs - m_stateSinceUs > kRequestTimeoutUs) {
            m_baseband = Baseband::Idle;
            m_retryAtUs = nowUs + kRetryDelayUs;
        }
    } else if (m_baseband == Baseband::Connected) {
        if (!host.IsAttached(m_slot)) {
            DropFromRemote(kReasonRemoteUserTerminated);
            m_connectWanted = false;
            return;
        }
        if (!m_guestInitiated) {
            const Channel* control = FindByPsm(kPsmHidControl);
            if (control == nullptr) {
                RequestChannel(kPsmHidControl, nowUs);
            } else if (control->Open() && FindByPsm(kPsmHidInterrupt) == nullptr) {
                RequestChannel(kPsmHidInterrupt, nowUs);
            }
        }
        for (const Channel& channel : m_channels) {
            const bool waiting = (channel.weInitiated && !channel.connected) ||
                                 (channel.configSent && !channel.localConfigured);
            if (waiting && nowUs - channel.requestUs > kRequestTimeoutUs) {
                std::cerr << "[bluetooth] Wii Remote " << (m_slot + 1) << ": L2CAP channel 0x" << std::hex
                          << channel.psm << std::dec << " did not open, dropping the connection" << std::endl;
                DropFromRemote(kReasonRemoteUserTerminated);
                m_retryAtUs = nowUs + kRetryDelayUs;
                return;
            }
        }
    }
    ForwardInput();
}

void RemoteLink::ForwardInput() {
    RemoteHost& host = m_controller.Host();
    if (m_state != LinkState::Ready) {
        while (host.PopInputReport(m_slot, m_report)) {
        }
        return;
    }
    const Channel* interrupt = FindByPsm(kPsmHidInterrupt);
    while (m_controller.AclQueued() < kInputBacklog && host.PopInputReport(m_slot, m_report)) {
        if (m_report.empty() || m_report.size() > kMaxReportSize) continue;
        SendInput(*interrupt, m_report);
    }
}

void RemoteLink::SendInput(const Channel& interrupt, const std::vector<uint8_t>& report) {
    std::vector<uint8_t> payload;
    payload.reserve(report.size() + 1);
    payload.push_back(kHidDataInput);
    payload.insert(payload.end(), report.begin(), report.end());
    if (payload.size() <= interrupt.remoteMtu) {
        SendData(interrupt, payload);
    }
}


void RemoteLink::OnAccepted(uint8_t role) {
    (void)role;
    m_baseband = Baseband::Connected;
    m_guestInitiated = false;
    m_everReady = false;
    m_stateSinceUs = m_nowUs;
    SetState(LinkState::Connecting);
}

void RemoteLink::OnRejected(uint8_t reason, uint64_t nowUs) {
    std::cerr << "[bluetooth] Wii Remote " << (m_slot + 1) << ": connection rejected (0x" << std::hex
              << static_cast<int>(reason) << std::dec << ")" << std::endl;
    m_baseband = Baseband::Idle;
    m_retryAtUs = nowUs + kRetryDelayUs;
}

bool RemoteLink::OnPaged(uint64_t nowUs) {
    if (!Discoverable()) {
        return false;
    }
    m_nowUs = nowUs;
    m_baseband = Baseband::Connected;
    m_guestInitiated = true;
    m_everReady = false;
    m_stateSinceUs = nowUs;
    SetState(LinkState::Connecting);
    return true;
}

void RemoteLink::OnDisconnected(bool byGuest, uint64_t nowUs) {
    const bool wasReady = m_everReady;
    ClearConnection();
    m_baseband = Baseband::Idle;
    SetState(LinkState::Down);
    if (byGuest && wasReady) {
        m_connectWanted = false;
        m_controller.Host().OnGuestDisconnect(m_slot);
    } else {
        m_retryAtUs = nowUs + kRetryDelayUs;
    }
}

void RemoteLink::DropFromRemote(uint8_t reason) {
    std::vector<uint8_t> event = {0x05, 4, 0x00};
    PutLe16(event, Controller::ConnectionHandle(m_slot));
    event.push_back(reason);
    m_controller.QueueEvent(std::move(event));
    ClearConnection();
    m_baseband = Baseband::Idle;
    SetState(LinkState::Down);
}

void RemoteLink::SetState(LinkState state) {
    if (m_state == state) return;
    m_state = state;
    if (state == LinkState::Ready) {
        m_everReady = true;
        m_connectWanted = false;
    }
    m_controller.Host().OnLinkStateChanged(m_slot, state);
}

void RemoteLink::UpdateState() {
    if (m_baseband != Baseband::Connected) {
        SetState(LinkState::Down);
        return;
    }
    const Channel* control = FindByPsm(kPsmHidControl);
    const Channel* interrupt = FindByPsm(kPsmHidInterrupt);
    const bool ready = control != nullptr && interrupt != nullptr && control->Open() && interrupt->Open();
    SetState(ready ? LinkState::Ready : LinkState::Connecting);
}

RemoteLink::Channel* RemoteLink::FindByLocal(uint16_t cid) {
    for (Channel& channel : m_channels) {
        if (channel.localCid == cid) return &channel;
    }
    return nullptr;
}

RemoteLink::Channel* RemoteLink::FindByPsm(uint16_t psm) {
    for (Channel& channel : m_channels) {
        if (channel.psm == psm) return &channel;
    }
    return nullptr;
}

uint16_t RemoteLink::AllocateCid() const {
    uint16_t cid = kFirstDynamicCid;
    while (std::any_of(m_channels.begin(), m_channels.end(), [&](const Channel& c) { return c.localCid == cid; })) {
        ++cid;
    }
    return cid;
}

uint8_t RemoteLink::NextIdent() {
    m_ident = static_cast<uint8_t>(m_ident + 1);
    if (m_ident == 0) m_ident = 1;
    return m_ident;
}

void RemoteLink::SendSignal(uint8_t code, uint8_t ident, const std::vector<uint8_t>& data) {
    std::vector<uint8_t> frame;
    frame.reserve(8 + data.size());
    PutLe16(frame, static_cast<uint16_t>(4 + data.size()));
    PutLe16(frame, kSignallingCid);
    frame.push_back(code);
    frame.push_back(ident);
    PutLe16(frame, static_cast<uint16_t>(data.size()));
    frame.insert(frame.end(), data.begin(), data.end());
    m_controller.QueueAcl(Controller::ConnectionHandle(m_slot), frame);
}

void RemoteLink::SendData(const Channel& channel, const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> frame;
    frame.reserve(4 + payload.size());
    PutLe16(frame, static_cast<uint16_t>(payload.size()));
    PutLe16(frame, channel.remoteCid);
    frame.insert(frame.end(), payload.begin(), payload.end());
    m_controller.QueueAcl(Controller::ConnectionHandle(m_slot), frame);
}

void RemoteLink::RequestChannel(uint16_t psm, uint64_t nowUs) {
    Channel channel;
    channel.psm = psm;
    channel.localCid = AllocateCid();
    channel.weInitiated = true;
    channel.connectIdent = NextIdent();
    channel.requestUs = nowUs;
    std::vector<uint8_t> data;
    PutLe16(data, psm);
    PutLe16(data, channel.localCid);
    SendSignal(kConnectionRequest, channel.connectIdent, data);
    m_channels.push_back(channel);
}

void RemoteLink::SendConfigRequest(Channel& channel) {
    channel.configSent = true;
    channel.configIdent = NextIdent();
    channel.requestUs = m_nowUs;
    std::vector<uint8_t> data;
    PutLe16(data, channel.remoteCid);
    PutLe16(data, 0);
    data.push_back(kOptionMtu);
    data.push_back(2);
    PutLe16(data, kRemoteMtu);
    SendSignal(kConfigurationRequest, channel.configIdent, data);
}

void RemoteLink::ReceiveL2cap(const uint8_t* frame, size_t size) {
    if (m_baseband != Baseband::Connected || size < 4) return;
    const uint16_t length = Le16(frame);
    const uint16_t cid = Le16(frame + 2);
    if (length != size - 4) {
        std::cerr << "[bluetooth] Wii Remote " << (m_slot + 1) << ": dropped malformed L2CAP frame" << std::endl;
        return;
    }
    if (cid == kSignallingCid) {
        HandleSignalling(frame + 4, length);
    } else if (Channel* channel = FindByLocal(cid)) {
        HandleChannelData(*channel, frame + 4, length);
    }
}

void RemoteLink::HandleSignalling(const uint8_t* data, size_t size) {
    while (size >= 4) {
        const uint8_t code = data[0];
        const uint8_t ident = data[1];
        const uint16_t length = Le16(data + 2);
        if (static_cast<size_t>(length) + 4 > size) {
            return;
        }
        const uint8_t* p = data + 4;
        switch (code) {
        case kConnectionRequest: HandleConnectionRequest(ident, p, length); break;
        case kConnectionResponse: HandleConnectionResponse(ident, p, length); break;
        case kConfigurationRequest: HandleConfigurationRequest(ident, p, length); break;
        case kConfigurationResponse: HandleConfigurationResponse(ident, p, length); break;
        case kDisconnectionRequest: HandleDisconnectionRequest(ident, p, length); break;
        case kDisconnectionResponse: HandleDisconnectionResponse(p, length); break;
        case kEchoRequest: SendSignal(kEchoResponse, ident, std::vector<uint8_t>(p, p + length)); break;
        case kInformationRequest: {
            if (length >= 2) {
                std::vector<uint8_t> reply;
                PutLe16(reply, Le16(p));
                PutLe16(reply, 0x0001); // not supported
                SendSignal(kInformationResponse, ident, reply);
            }
            break;
        }
        case kCommandReject:
            std::cerr << "[bluetooth] Wii Remote " << (m_slot + 1) << ": the game rejected L2CAP command "
                      << static_cast<int>(ident) << std::endl;
            break;
        case kEchoResponse:
        case kInformationResponse:
            break;
        default: {
            std::vector<uint8_t> reply;
            PutLe16(reply, 0x0000); // command not understood
            SendSignal(kCommandReject, ident, reply);
            break;
        }
        }
        data += 4 + length;
        size -= 4 + length;
    }
}

void RemoteLink::HandleConnectionRequest(uint8_t ident, const uint8_t* p, size_t n) {
    if (n < 4) return;
    const uint16_t psm = Le16(p);
    const uint16_t scid = Le16(p + 2);
    uint16_t result = kResultSuccess;
    uint16_t dcid = 0;
    if (psm != kPsmSdp && psm != kPsmHidControl && psm != kPsmHidInterrupt) {
        result = kResultPsmNotSupported;
    } else if (FindByPsm(psm) != nullptr) {
        // A real remote refuses a second channel on the same PSM.
        result = kResultNoResources;
    } else {
        Channel channel;
        channel.psm = psm;
        channel.localCid = AllocateCid();
        channel.remoteCid = scid;
        channel.connected = true;
        dcid = channel.localCid;
        m_channels.push_back(channel);
    }
    std::vector<uint8_t> reply;
    PutLe16(reply, dcid);
    PutLe16(reply, scid);
    PutLe16(reply, result);
    PutLe16(reply, 0);
    SendSignal(kConnectionResponse, ident, reply);
    if (result == kResultSuccess) {
        SendConfigRequest(*FindByLocal(dcid));
    }
}

void RemoteLink::HandleConnectionResponse(uint8_t ident, const uint8_t* p, size_t n) {
    if (n < 8) return;
    const uint16_t dcid = Le16(p);
    const uint16_t scid = Le16(p + 2);
    const uint16_t result = Le16(p + 4);
    Channel* channel = FindByLocal(scid);
    if (channel == nullptr || !channel->weInitiated || channel->connected || channel->connectIdent != ident) {
        return;
    }
    if (result == kResultSuccess) {
        channel->remoteCid = dcid;
        channel->connected = true;
        SendConfigRequest(*channel);
    } else if (result == kResultPending) {
        channel->requestUs = m_nowUs;
    } else {
        std::cerr << "[bluetooth] Wii Remote " << (m_slot + 1) << ": the game refused L2CAP PSM 0x" << std::hex
                  << channel->psm << " (result 0x" << result << ")" << std::dec << std::endl;
        DropFromRemote(kReasonRemoteUserTerminated);
        m_retryAtUs = m_nowUs + kRetryDelayUs;
    }
}

void RemoteLink::HandleConfigurationRequest(uint8_t ident, const uint8_t* p, size_t n) {
    if (n < 4) return;
    const uint16_t dcid = Le16(p);
    const uint16_t flags = Le16(p + 2);
    Channel* channel = FindByLocal(dcid);
    if (channel == nullptr) {
        std::vector<uint8_t> reply;
        PutLe16(reply, 0x0002); // invalid CID
        PutLe16(reply, dcid);
        PutLe16(reply, 0);
        SendSignal(kCommandReject, ident, reply);
        return;
    }
    channel->pendingOptions.insert(channel->pendingOptions.end(), p + 4, p + n);
    std::vector<uint8_t> reply;
    PutLe16(reply, channel->remoteCid);
    if (flags & 1) {
        PutLe16(reply, 1);
        PutLe16(reply, kResultSuccess);
        SendSignal(kConfigurationResponse, ident, reply);
        return;
    }
    PutLe16(reply, 0);
    PutLe16(reply, kResultSuccess);
    const std::vector<uint8_t>& options = channel->pendingOptions;
    for (size_t i = 0; i + 2 <= options.size();) {
        const uint8_t type = options[i] & 0x7F;
        const uint8_t length = options[i + 1];
        if (i + 2 + length > options.size()) break;
        if (type == kOptionMtu && length == 2) {
            channel->remoteMtu = Le16(&options[i + 2]);
            reply.insert(reply.end(), options.begin() + static_cast<std::ptrdiff_t>(i),
                         options.begin() + static_cast<std::ptrdiff_t>(i + 4));
        } else if (type == kOptionFlushTimeout && length == 2) {
            reply.insert(reply.end(), options.begin() + static_cast<std::ptrdiff_t>(i),
                         options.begin() + static_cast<std::ptrdiff_t>(i + 4));
        }
        i += 2 + length;
    }
    channel->pendingOptions.clear();
    channel->remoteConfigured = true;
    SendSignal(kConfigurationResponse, ident, reply);
    UpdateState();
}

void RemoteLink::HandleConfigurationResponse(uint8_t ident, const uint8_t* p, size_t n) {
    if (n < 6) return;
    const uint16_t scid = Le16(p);
    const uint16_t flags = Le16(p + 2);
    const uint16_t result = Le16(p + 4);
    Channel* channel = FindByLocal(scid);
    if (channel == nullptr || !channel->configSent || channel->configIdent != ident) return;
    if (flags & 1) {
        return;
    }
    if (result == kResultSuccess) {
        channel->localConfigured = true;
        UpdateState();
        return;
    }
    // Our only option is the MTU; offer the default instead.
    channel->configIdent = NextIdent();
    channel->requestUs = m_nowUs;
    std::vector<uint8_t> data;
    PutLe16(data, channel->remoteCid);
    PutLe16(data, 0);
    SendSignal(kConfigurationRequest, channel->configIdent, data);
}

void RemoteLink::HandleDisconnectionRequest(uint8_t ident, const uint8_t* p, size_t n) {
    if (n < 4) return;
    const uint16_t dcid = Le16(p);
    const uint16_t scid = Le16(p + 2);
    std::vector<uint8_t> reply;
    PutLe16(reply, dcid);
    PutLe16(reply, scid);
    SendSignal(kDisconnectionResponse, ident, reply);
    m_channels.erase(std::remove_if(m_channels.begin(), m_channels.end(),
                                    [&](const Channel& c) { return c.localCid == dcid; }),
                     m_channels.end());
    UpdateState();
}

void RemoteLink::HandleDisconnectionResponse(const uint8_t* p, size_t n) {
    if (n < 4) return;
    const uint16_t scid = Le16(p + 2);
    m_channels.erase(std::remove_if(m_channels.begin(), m_channels.end(),
                                    [&](const Channel& c) { return c.localCid == scid; }),
                     m_channels.end());
    UpdateState();
}

void RemoteLink::HandleChannelData(Channel& channel, const uint8_t* data, size_t size) {
    if (size == 0) return;
    if (channel.psm == kPsmSdp) {
        const std::vector<uint8_t> response = Sdp::HandleRequest(data, size, channel.remoteMtu);
        SendData(channel, response);
        return;
    }
    if (channel.psm == kPsmHidControl) {
        HandleHidControl(channel, data, size);
        return;
    }
    const uint8_t type = data[0] >> 4;
    const uint8_t param = data[0] & 0x0F;
    if (type == kHidData && param == kHidParamOutput && size >= 2 && size - 1 <= kMaxReportSize) {
        m_controller.Host().SendOutputReport(m_slot, data + 1, size - 1);
    }
}

void RemoteLink::HandleHidControl(Channel& channel, const uint8_t* data, size_t size) {
    const uint8_t type = data[0] >> 4;
    const uint8_t param = data[0] & 0x0F;
    switch (type) {
    case kHidHandshake:
        return;
    case kHidControl:
        if (param == kHidVirtualCableUnplug) {
            const bool wasReady = m_everReady;
            DropFromRemote(kReasonRemoteUserTerminated);
            if (wasReady) {
                m_connectWanted = false;
                m_controller.Host().OnGuestDisconnect(m_slot);
            }
        }
        return;
    case kHidSetReport:
        SendData(channel, {kHidHandshakeSuccess});
        if ((param & 0x3) == kHidParamOutput && size >= 2 && size - 1 <= kMaxReportSize) {
            m_controller.Host().SendOutputReport(m_slot, data + 1, size - 1);
        }
        return;
    case kHidData:
        if (param == kHidParamOutput && size >= 2 && size - 1 <= kMaxReportSize) {
            m_controller.Host().SendOutputReport(m_slot, data + 1, size - 1);
        }
        return;
    default:
        SendData(channel, {kHidHandshakeUnsupported});
        return;
    }
}

} // namespace Bluetooth
