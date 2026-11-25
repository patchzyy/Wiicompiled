#pragma once

// One emulated Wii Remote as seen from the controller: its baseband connection,
// L2CAP channels (SDP, HID control, HID interrupt) and the HID transaction layer
// that carries the physical remote's reports.

#include "bluetooth/bt_controller.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace Bluetooth {

class RemoteLink {
public:
    enum class Baseband : uint8_t {
        Idle,
        Requesting, // Connection Request sent, waiting for the guest to accept
        Paging,     // the guest paged this remote (Create Connection)
        Connected,
    };

    RemoteLink(Controller& controller, size_t slot);

    // Drops every connection without telling the guest (controller reset/close).
    void Reset();
    void Tick(uint64_t nowUs);

    // Remote side wants a connection and should be offered to the guest's inquiry.
    bool Discoverable() const;
    bool ConnectWanted() const { return m_connectWanted; }

    // Guest accepted / rejected this remote's Connection Request.
    bool IsRequesting() const { return m_baseband == Baseband::Requesting; }
    void OnAccepted(uint8_t role);
    void OnRejected(uint8_t reason, uint64_t nowUs);
    // Guest paged this remote. Returns false when the remote cannot answer.
    bool OnPaged(uint64_t nowUs);
    // Baseband went down. `byGuest` = the guest's HCI Disconnect.
    void OnDisconnected(bool byGuest, uint64_t nowUs);
    // The physical remote vanished while connected: tells the guest.
    void DropFromRemote(uint8_t reason);

    void ReceiveL2cap(const uint8_t* frame, size_t size);

    Baseband BasebandState() const { return m_baseband; }
    LinkState State() const { return m_state; }

private:
    struct Channel {
        uint16_t psm = 0;
        uint16_t localCid = 0;
        uint16_t remoteCid = 0;
        uint16_t remoteMtu = 672;
        bool weInitiated = false;
        bool connected = false;        // connection response success seen/sent
        bool configSent = false;       // our Configuration Request is out
        bool localConfigured = false;  // the guest accepted our configuration
        bool remoteConfigured = false; // we accepted the guest's configuration
        uint8_t connectIdent = 0;
        uint8_t configIdent = 0;
        uint64_t requestUs = 0;
        std::vector<uint8_t> pendingOptions; // continued Configuration Request
        bool Open() const { return connected && localConfigured && remoteConfigured; }
    };

    Channel* FindByLocal(uint16_t cid);
    Channel* FindByPsm(uint16_t psm);
    uint16_t AllocateCid() const;
    uint8_t NextIdent();

    void SendSignal(uint8_t code, uint8_t ident, const std::vector<uint8_t>& data);
    void SendData(const Channel& channel, const std::vector<uint8_t>& payload);
    void RequestChannel(uint16_t psm, uint64_t nowUs);
    void SendConfigRequest(Channel& channel);
    void HandleSignalling(const uint8_t* data, size_t size);
    void HandleConnectionRequest(uint8_t ident, const uint8_t* p, size_t n);
    void HandleConnectionResponse(uint8_t ident, const uint8_t* p, size_t n);
    void HandleConfigurationRequest(uint8_t ident, const uint8_t* p, size_t n);
    void HandleConfigurationResponse(uint8_t ident, const uint8_t* p, size_t n);
    void HandleDisconnectionRequest(uint8_t ident, const uint8_t* p, size_t n);
    void HandleDisconnectionResponse(const uint8_t* p, size_t n);
    void HandleChannelData(Channel& channel, const uint8_t* data, size_t size);
    void HandleHidControl(Channel& channel, const uint8_t* data, size_t size);
    void UpdateState();
    void SetState(LinkState state);
    void ForwardInput();
    void SendInput(const Channel& interrupt, const std::vector<uint8_t>& report);
    void ClearConnection();

    Controller& m_controller;
    size_t m_slot;
    Baseband m_baseband = Baseband::Idle;
    LinkState m_state = LinkState::Down;
    bool m_connectWanted = false;
    bool m_guestInitiated = false;
    bool m_everReady = false;
    uint8_t m_ident = 0;
    uint64_t m_stateSinceUs = 0;
    uint64_t m_retryAtUs = 0;
    uint64_t m_nowUs = 0;
    std::vector<Channel> m_channels;
    std::vector<uint8_t> m_report;
};

} // namespace Bluetooth
