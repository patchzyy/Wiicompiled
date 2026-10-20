#pragma once

// SDP server of an emulated Wii Remote: the HID service record (0x00010000)
// and the Device ID record (0x00010001) a real RVL-CNT-01 publishes.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace Bluetooth::Sdp {

// The Wii Remote's HID report descriptor, as the game also embeds it.
const std::vector<uint8_t>& WiiRemoteHidDescriptor();

// Answers one SDP request PDU. `maxResponse` bounds the PDU returned (the
// guest's L2CAP MTU on the SDP channel).
std::vector<uint8_t> HandleRequest(const uint8_t* pdu, size_t size, size_t maxResponse);

} // namespace Bluetooth::Sdp
