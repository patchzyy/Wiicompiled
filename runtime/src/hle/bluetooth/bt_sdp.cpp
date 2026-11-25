#include "bluetooth/bt_sdp.h"

#include <algorithm>
#include <array>
#include <initializer_list>
#include <optional>
#include <string_view>
#include <utility>

namespace Bluetooth::Sdp {
namespace {

using Bytes = std::vector<uint8_t>;

constexpr uint8_t kErrorResponse = 0x01;
constexpr uint8_t kServiceSearchRequest = 0x02;
constexpr uint8_t kServiceSearchResponse = 0x03;
constexpr uint8_t kServiceAttributeRequest = 0x04;
constexpr uint8_t kServiceAttributeResponse = 0x05;
constexpr uint8_t kServiceSearchAttributeRequest = 0x06;
constexpr uint8_t kServiceSearchAttributeResponse = 0x07;

constexpr uint16_t kErrInvalidHandle = 0x0002;
constexpr uint16_t kErrInvalidSyntax = 0x0003;
constexpr uint16_t kErrInvalidPduSize = 0x0004;
constexpr uint16_t kErrInvalidContinuation = 0x0005;

void PutBe16(Bytes& out, uint16_t v) {
    out.push_back(static_cast<uint8_t>(v >> 8));
    out.push_back(static_cast<uint8_t>(v));
}

void PutBe32(Bytes& out, uint32_t v) {
    PutBe16(out, static_cast<uint16_t>(v >> 16));
    PutBe16(out, static_cast<uint16_t>(v));
}

uint16_t Be16(const uint8_t* p) {
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

uint32_t Be32(const uint8_t* p) {
    return (static_cast<uint32_t>(Be16(p)) << 16) | Be16(p + 2);
}

// Data element constructors (Bluetooth Core, Vol 3 Part B, 3.2).
Bytes U8(uint8_t v) { return {0x08, v}; }
Bytes U16(uint16_t v) { Bytes b{0x09}; PutBe16(b, v); return b; }
Bytes U32(uint32_t v) { Bytes b{0x0A}; PutBe32(b, v); return b; }
Bytes Uuid16(uint16_t v) { Bytes b{0x19}; PutBe16(b, v); return b; }
Bytes Bool(bool v) { return {0x28, static_cast<uint8_t>(v ? 1 : 0)}; }

Bytes WithLength(uint8_t type, size_t length, bool wide = false) {
    if (length <= 0xFF && !wide) {
        return {static_cast<uint8_t>((type << 3) | 5), static_cast<uint8_t>(length)};
    }
    Bytes b{static_cast<uint8_t>((type << 3) | 6)};
    PutBe16(b, static_cast<uint16_t>(length));
    return b;
}

Bytes Text(const Bytes& raw) {
    Bytes b = WithLength(4, raw.size());
    b.insert(b.end(), raw.begin(), raw.end());
    return b;
}

Bytes Text(std::string_view s) {
    return Text(Bytes(s.begin(), s.end()));
}

Bytes Seq(std::initializer_list<Bytes> items) {
    size_t length = 0;
    for (const Bytes& item : items) length += item.size();
    Bytes b = WithLength(6, length);
    for (const Bytes& item : items) b.insert(b.end(), item.begin(), item.end());
    return b;
}

using Record = std::vector<std::pair<uint16_t, Bytes>>;

const Bytes kHidDescriptor = {
    0x05, 0x01, 0x09, 0x05, 0xA1, 0x01, 0x85, 0x10, 0x15, 0x00, 0x26, 0xFF, 0x00, 0x75, 0x08, 0x95,
    0x01, 0x06, 0x00, 0xFF, 0x09, 0x01, 0x91, 0x00, 0x85, 0x11, 0x95, 0x01, 0x09, 0x01, 0x91, 0x00,
    0x85, 0x12, 0x95, 0x02, 0x09, 0x01, 0x91, 0x00, 0x85, 0x13, 0x95, 0x01, 0x09, 0x01, 0x91, 0x00,
    0x85, 0x14, 0x95, 0x01, 0x09, 0x01, 0x91, 0x00, 0x85, 0x15, 0x95, 0x01, 0x09, 0x01, 0x91, 0x00,
    0x85, 0x16, 0x95, 0x15, 0x09, 0x01, 0x91, 0x00, 0x85, 0x17, 0x95, 0x06, 0x09, 0x01, 0x91, 0x00,
    0x85, 0x18, 0x95, 0x15, 0x09, 0x01, 0x91, 0x00, 0x85, 0x19, 0x95, 0x01, 0x09, 0x01, 0x91, 0x00,
    0x85, 0x1A, 0x95, 0x01, 0x09, 0x01, 0x91, 0x00, 0x85, 0x20, 0x95, 0x06, 0x09, 0x01, 0x81, 0x00,
    0x85, 0x21, 0x95, 0x15, 0x09, 0x01, 0x81, 0x00, 0x85, 0x22, 0x95, 0x04, 0x09, 0x01, 0x81, 0x00,
    0x85, 0x30, 0x95, 0x02, 0x09, 0x01, 0x81, 0x00, 0x85, 0x31, 0x95, 0x05, 0x09, 0x01, 0x81, 0x00,
    0x85, 0x32, 0x95, 0x0A, 0x09, 0x01, 0x81, 0x00, 0x85, 0x33, 0x95, 0x11, 0x09, 0x01, 0x81, 0x00,
    0x85, 0x34, 0x95, 0x15, 0x09, 0x01, 0x81, 0x00, 0x85, 0x35, 0x95, 0x15, 0x09, 0x01, 0x81, 0x00,
    0x85, 0x36, 0x95, 0x15, 0x09, 0x01, 0x81, 0x00, 0x85, 0x37, 0x95, 0x15, 0x09, 0x01, 0x81, 0x00,
    0x85, 0x3D, 0x95, 0x15, 0x09, 0x01, 0x81, 0x00, 0x85, 0x3E, 0x95, 0x15, 0x09, 0x01, 0x81, 0x00,
    0x85, 0x3F, 0x95, 0x15, 0x09, 0x01, 0x81, 0x00, 0xC0,
};

constexpr uint32_t kHidRecordHandle = 0x00010000;
constexpr uint32_t kDeviceIdRecordHandle = 0x00010001;

const Record& HidRecord() {
    static const Record record = {
        {0x0000, U32(kHidRecordHandle)},
        {0x0001, Seq({Uuid16(0x1124)})},
        {0x0004, Seq({Seq({Uuid16(0x0100), U16(0x0011)}), Seq({Uuid16(0x0011)})})},
        {0x0005, Seq({Uuid16(0x1002)})},
        {0x0006, Seq({U16(0x656E), U16(0x006A), U16(0x0100)})},
        {0x0009, Seq({Seq({Uuid16(0x1124), U16(0x0100)})})},
        {0x000D, Seq({Seq({Seq({Uuid16(0x0100), U16(0x0013)}), Seq({Uuid16(0x0011)})})})},
        {0x0100, Text("Nintendo RVL-CNT-01")},
        {0x0101, Text("Nintendo RVL-CNT-01")},
        {0x0102, Text("Nintendo")},
        {0x0200, U16(0x0100)},
        {0x0201, U16(0x0111)},
        {0x0202, U8(0x04)},
        {0x0203, U8(0x33)},
        {0x0204, Bool(false)},
        {0x0205, Bool(true)},
        {0x0206, Seq({Seq({U8(0x22), Text(kHidDescriptor)})})},
        {0x0207, Seq({Seq({U16(0x0409), U16(0x0100)})})},
        {0x0208, Bool(false)},
        {0x0209, Bool(true)},
        {0x020A, Bool(true)},
        {0x020B, U16(0x0100)},
        {0x020C, U16(0x0C80)},
        {0x020D, Bool(false)},
        {0x020E, Bool(false)},
    };
    return record;
}

const Record& DeviceIdRecord() {
    static const Record record = {
        {0x0000, U32(kDeviceIdRecordHandle)},
        {0x0001, Seq({Uuid16(0x1200)})},
        {0x0004, Seq({Seq({Uuid16(0x0100), U16(0x0001)}), Seq({Uuid16(0x0001)})})},
        {0x0005, Seq({Uuid16(0x1002)})},
        {0x0009, Seq({Seq({Uuid16(0x1200), U16(0x0100)})})},
        {0x0200, U16(0x0100)},
        {0x0201, U16(0x057E)},
        {0x0202, U16(0x0306)},
        {0x0203, U16(0x0600)},
        {0x0204, Bool(true)},
        {0x0205, U16(0x0002)},
    };
    return record;
}

struct NamedRecord {
    uint32_t handle;
    const Record& (*get)();
};
constexpr std::array<NamedRecord, 2> kRecords = {{
    {kHidRecordHandle, HidRecord},
    {kDeviceIdRecordHandle, DeviceIdRecord},
}};

using Uuid = std::array<uint8_t, 16>;

Uuid FromShort(uint32_t value) {
    // Bluetooth base UUID 00000000-0000-1000-8000-00805F9B34FB.
    Uuid u = {0, 0, 0, 0, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0x80, 0x5F, 0x9B, 0x34, 0xFB};
    u[0] = static_cast<uint8_t>(value >> 24);
    u[1] = static_cast<uint8_t>(value >> 16);
    u[2] = static_cast<uint8_t>(value >> 8);
    u[3] = static_cast<uint8_t>(value);
    return u;
}

// One parsed data element header: type, header size and payload size.
struct ElementHeader {
    uint8_t type = 0;
    size_t headerSize = 0;
    size_t dataSize = 0;
};

std::optional<ElementHeader> ParseHeader(const uint8_t* p, size_t n) {
    if (n == 0) return std::nullopt;
    ElementHeader h;
    h.type = static_cast<uint8_t>(p[0] >> 3);
    const uint8_t sizeIndex = p[0] & 7;
    if (h.type == 0) {
        h.headerSize = 1;
        h.dataSize = 0;
    } else if (sizeIndex <= 4) {
        h.headerSize = 1;
        h.dataSize = size_t{1} << sizeIndex;
    } else if (sizeIndex == 5) {
        if (n < 2) return std::nullopt;
        h.headerSize = 2;
        h.dataSize = p[1];
    } else if (sizeIndex == 6) {
        if (n < 3) return std::nullopt;
        h.headerSize = 3;
        h.dataSize = Be16(p + 1);
    } else {
        if (n < 5) return std::nullopt;
        h.headerSize = 5;
        h.dataSize = Be32(p + 1);
    }
    if (h.headerSize + h.dataSize > n) return std::nullopt;
    return h;
}

std::optional<Uuid> ReadUuid(const uint8_t* p, const ElementHeader& h) {
    if (h.type != 3) return std::nullopt;
    const uint8_t* d = p + h.headerSize;
    if (h.dataSize == 2) return FromShort(Be16(d));
    if (h.dataSize == 4) return FromShort(Be32(d));
    if (h.dataSize == 16) {
        Uuid u{};
        std::copy(d, d + 16, u.begin());
        return u;
    }
    return std::nullopt;
}

void CollectUuids(const uint8_t* p, size_t n, std::vector<Uuid>& out) {
    while (n > 0) {
        const auto h = ParseHeader(p, n);
        if (!h) return;
        if (h->type == 6 || h->type == 7) {
            CollectUuids(p + h->headerSize, h->dataSize, out);
        } else if (const auto uuid = ReadUuid(p, *h)) {
            out.push_back(*uuid);
        }
        const size_t total = h->headerSize + h->dataSize;
        p += total;
        n -= total;
    }
}

bool RecordMatches(const Record& record, const std::vector<Uuid>& pattern) {
    std::vector<Uuid> present;
    for (const auto& [id, value] : record) CollectUuids(value.data(), value.size(), present);
    return std::all_of(pattern.begin(), pattern.end(), [&](const Uuid& wanted) {
        return std::find(present.begin(), present.end(), wanted) != present.end();
    });
}

// Parses a data element sequence at `p`; advances `p`/`n` past it.
std::optional<std::pair<const uint8_t*, size_t>> TakeSequence(const uint8_t*& p, size_t& n) {
    const auto h = ParseHeader(p, n);
    if (!h || (h->type != 6)) return std::nullopt;
    const std::pair<const uint8_t*, size_t> body{p + h->headerSize, h->dataSize};
    p += h->headerSize + h->dataSize;
    n -= h->headerSize + h->dataSize;
    return body;
}

std::optional<std::vector<Uuid>> ParsePattern(const uint8_t*& p, size_t& n) {
    const auto body = TakeSequence(p, n);
    if (!body) return std::nullopt;
    std::vector<Uuid> pattern;
    const uint8_t* q = body->first;
    size_t m = body->second;
    while (m > 0) {
        const auto h = ParseHeader(q, m);
        if (!h) return std::nullopt;
        const auto uuid = ReadUuid(q, *h);
        if (!uuid) return std::nullopt;
        pattern.push_back(*uuid);
        q += h->headerSize + h->dataSize;
        m -= h->headerSize + h->dataSize;
    }
    if (pattern.empty() || pattern.size() > 12) return std::nullopt;
    return pattern;
}

using IdRange = std::pair<uint16_t, uint16_t>;

std::optional<std::vector<IdRange>> ParseAttributeIds(const uint8_t*& p, size_t& n) {
    const auto body = TakeSequence(p, n);
    if (!body) return std::nullopt;
    std::vector<IdRange> ranges;
    const uint8_t* q = body->first;
    size_t m = body->second;
    while (m > 0) {
        const auto h = ParseHeader(q, m);
        if (!h || h->type != 1) return std::nullopt;
        const uint8_t* d = q + h->headerSize;
        if (h->dataSize == 2) {
            ranges.emplace_back(Be16(d), Be16(d));
        } else if (h->dataSize == 4) {
            ranges.emplace_back(Be16(d), Be16(d + 2));
        } else {
            return std::nullopt;
        }
        q += h->headerSize + h->dataSize;
        m -= h->headerSize + h->dataSize;
    }
    if (ranges.empty()) return std::nullopt;
    return ranges;
}

// Continuation state is the byte offset into the complete response (2 bytes BE).
std::optional<size_t> ParseContinuation(const uint8_t* p, size_t n) {
    if (n < 1 || p[0] + size_t{1} != n) return std::nullopt;
    if (p[0] == 0) return 0;
    if (p[0] != 2) return std::nullopt;
    return Be16(p + 1);
}

Bytes AttributeList(const Record& record, const std::vector<IdRange>& ranges) {
    Bytes inner;
    for (const auto& [id, value] : record) {
        const bool wanted = std::any_of(ranges.begin(), ranges.end(),
                                        [id = id](const IdRange& r) { return id >= r.first && id <= r.second; });
        if (!wanted) continue;
        const Bytes idElement = U16(id);
        inner.insert(inner.end(), idElement.begin(), idElement.end());
        inner.insert(inner.end(), value.begin(), value.end());
    }
    // A real remote always sends attribute lists with a 16-bit length.
    Bytes list = WithLength(6, inner.size(), true);
    list.insert(list.end(), inner.begin(), inner.end());
    return list;
}

Bytes Pdu(uint8_t id, uint16_t tid, const Bytes& params) {
    Bytes out{id};
    PutBe16(out, tid);
    PutBe16(out, static_cast<uint16_t>(params.size()));
    out.insert(out.end(), params.begin(), params.end());
    return out;
}

Bytes Error(uint16_t tid, uint16_t code) {
    Bytes params;
    PutBe16(params, code);
    return Pdu(kErrorResponse, tid, params);
}

// Emits the slice of `full` starting at `offset` that fits, plus continuation.
Bytes Chunked(uint8_t responseId, uint16_t tid, const Bytes& full, size_t offset, uint16_t maxBytes,
              size_t maxResponse) {
    // PDU header 5, byte count 2, continuation up to 3.
    const size_t room = maxResponse > 10 ? maxResponse - 10 : 1;
    const size_t limit = std::max<size_t>(1, std::min<size_t>(maxBytes, room));
    const size_t count = std::min(limit, full.size() - offset);
    Bytes params;
    PutBe16(params, static_cast<uint16_t>(count));
    params.insert(params.end(), full.begin() + static_cast<std::ptrdiff_t>(offset),
                  full.begin() + static_cast<std::ptrdiff_t>(offset + count));
    const size_t next = offset + count;
    if (next < full.size()) {
        params.push_back(2);
        PutBe16(params, static_cast<uint16_t>(next));
    } else {
        params.push_back(0);
    }
    return Pdu(responseId, tid, params);
}

} // namespace

const std::vector<uint8_t>& WiiRemoteHidDescriptor() {
    return kHidDescriptor;
}

std::vector<uint8_t> HandleRequest(const uint8_t* pdu, size_t size, size_t maxResponse) {
    if (size < 5) {
        return Error(0, kErrInvalidPduSize);
    }
    const uint8_t id = pdu[0];
    const uint16_t tid = Be16(pdu + 1);
    const uint16_t paramLength = Be16(pdu + 3);
    if (paramLength != size - 5) {
        return Error(tid, kErrInvalidPduSize);
    }
    const uint8_t* p = pdu + 5;
    size_t n = paramLength;

    switch (id) {
    case kServiceSearchRequest: {
        const auto pattern = ParsePattern(p, n);
        if (!pattern || n < 2) return Error(tid, kErrInvalidSyntax);
        const uint16_t maxCount = Be16(p);
        if (!ParseContinuation(p + 2, n - 2)) return Error(tid, kErrInvalidContinuation);
        Bytes handles;
        uint16_t found = 0;
        for (const NamedRecord& record : kRecords) {
            if (found < maxCount && RecordMatches(record.get(), *pattern)) {
                PutBe32(handles, record.handle);
                ++found;
            }
        }
        Bytes params;
        PutBe16(params, found);
        PutBe16(params, found);
        params.insert(params.end(), handles.begin(), handles.end());
        params.push_back(0);
        return Pdu(kServiceSearchResponse, tid, params);
    }
    case kServiceAttributeRequest: {
        if (n < 6) return Error(tid, kErrInvalidSyntax);
        const uint32_t handle = Be32(p);
        const uint16_t maxBytes = Be16(p + 4);
        p += 6;
        n -= 6;
        const auto ranges = ParseAttributeIds(p, n);
        if (!ranges || maxBytes < 7) return Error(tid, kErrInvalidSyntax);
        const auto record = std::find_if(kRecords.begin(), kRecords.end(),
                                         [&](const NamedRecord& r) { return r.handle == handle; });
        if (record == kRecords.end()) return Error(tid, kErrInvalidHandle);
        const Bytes full = AttributeList(record->get(), *ranges);
        const auto offset = ParseContinuation(p, n);
        if (!offset || *offset > full.size()) return Error(tid, kErrInvalidContinuation);
        return Chunked(kServiceAttributeResponse, tid, full, *offset, maxBytes, maxResponse);
    }
    case kServiceSearchAttributeRequest: {
        const auto pattern = ParsePattern(p, n);
        if (!pattern || n < 2) return Error(tid, kErrInvalidSyntax);
        const uint16_t maxBytes = Be16(p);
        p += 2;
        n -= 2;
        const auto ranges = ParseAttributeIds(p, n);
        if (!ranges || maxBytes < 7) return Error(tid, kErrInvalidSyntax);
        Bytes lists;
        for (const NamedRecord& record : kRecords) {
            if (RecordMatches(record.get(), *pattern)) {
                const Bytes list = AttributeList(record.get(), *ranges);
                lists.insert(lists.end(), list.begin(), list.end());
            }
        }
        Bytes full = WithLength(6, lists.size());
        full.insert(full.end(), lists.begin(), lists.end());
        const auto offset = ParseContinuation(p, n);
        if (!offset || *offset > full.size()) return Error(tid, kErrInvalidContinuation);
        return Chunked(kServiceSearchAttributeResponse, tid, full, *offset, maxBytes, maxResponse);
    }
    default:
        return Error(tid, kErrInvalidSyntax);
    }
}

} // namespace Bluetooth::Sdp
