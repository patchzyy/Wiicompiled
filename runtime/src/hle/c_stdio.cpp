#include "hle_stubs.h"
#include "hle/guest_printf.h"
#include "memory.h"

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>

// Hardware boundary used by the translated MetroWerks stdio implementation.
// The original routine forwards completed FILE-buffer writes to UART/EXI or TRK.
// Keep __FileWrite (0x80011620), __fwrite, and fwide translated so their buffering,
// orientation, short-write, and return-value semantics remain guest-owned.
extern "C" uint32_t ConsoleWrite_HLE_80015500(
    uint32_t handle, uint32_t bufferAddr, uint32_t lengthPtr, uint32_t refCon)
{
    (void)handle;
    (void)refCon;

    if (!lengthPtr) {
        return 1;
    }

    try {
        const uint32_t length = Memory::Read32(lengthPtr);
        if (length == 0) {
            return 0;
        }

        const uint8_t* data = Memory::GetPointer(bufferAddr, length);
        if (!data) {
            Memory::Write32(lengthPtr, 0);
            return 1;
        }

        return 0;
    } catch (const Memory::AccessViolation&) {
        try {
            Memory::Write32(lengthPtr, 0);
        } catch (const Memory::AccessViolation&) {
        }
        return 1;
    }
}

PPC_NATIVE_OVERRIDE(80015500, ConsoleWrite_HLE_80015500, uint32_t,
         (uint32_t handle, uint32_t bufferAddr, uint32_t lengthPtr, uint32_t refCon),
         (handle, bufferAddr, lengthPtr, refCon));



// --------------------------------------------------------------------------
// Shared guest printf formatter used by CPU-context stdio and OSReport paths.
// --------------------------------------------------------------------------
std::string RuntimeHle::FormatGuestPrintf(const std::string& fmt,
                                          size_t& outChars,
                                          const PrintfNext32& next32,
                                          const PrintfNext64& next64,
                                          const PrintfNextDouble& nextDouble,
                                          const PrintfReadString& readString)
{
    std::ostringstream out;
    outChars = 0;

    auto appendText = [&](const std::string& text) {
        out << text;
        outChars += text.size();
    };

    auto appendChar = [&](char ch) {
        out.put(ch);
        ++outChars;
    };

    size_t i = 0;
    while (i < fmt.size()) {
        if (fmt[i] != '%') {
            appendChar(fmt[i]);
            ++i;
            continue;
        }

        // Handle escaped %%
        if (i + 1 < fmt.size() && fmt[i + 1] == '%') {
            appendChar('%');
            i += 2;
            continue;
        }

        size_t cur = i + 1;
        std::string flags;
        while (cur < fmt.size() && std::strchr("-+ #0", fmt[cur])) {
            flags.push_back(fmt[cur]);
            ++cur;
        }

        bool widthFromArg = false;
        int width = -1;
        if (cur < fmt.size() && fmt[cur] == '*') {
            widthFromArg = true;
            ++cur;
        } else {
            int parsed = 0;
            bool seen = false;
            while (cur < fmt.size() && std::isdigit(static_cast<unsigned char>(fmt[cur]))) {
                seen = true;
                parsed = (parsed * 10) + (fmt[cur] - '0');
                ++cur;
            }
            if (seen) {
                width = parsed;
            }
        }

        bool precisionFromArg = false;
        int precision = -1;
        if (cur < fmt.size() && fmt[cur] == '.') {
            ++cur;
            if (cur < fmt.size() && fmt[cur] == '*') {
                precisionFromArg = true;
                ++cur;
            } else {
                int parsed = 0;
                bool seen = false;
                while (cur < fmt.size() && std::isdigit(static_cast<unsigned char>(fmt[cur]))) {
                    seen = true;
                    parsed = (parsed * 10) + (fmt[cur] - '0');
                    ++cur;
                }
                precision = seen ? parsed : 0; // "%.s" -> precision 0
            }
        }

        std::string length;
        if (cur < fmt.size()) {
            if (fmt[cur] == 'h') {
                length.push_back('h');
                ++cur;
                if (cur < fmt.size() && fmt[cur] == 'h') {
                    length.push_back('h');
                    ++cur;
                }
            } else if (fmt[cur] == 'l') {
                length.push_back('l');
                ++cur;
                if (cur < fmt.size() && fmt[cur] == 'l') {
                    length.push_back('l');
                    ++cur;
                }
            } else if (fmt[cur] == 'z' || fmt[cur] == 't') {
                length.push_back(fmt[cur]);
                ++cur;
            }
        }

        if (widthFromArg) {
            width = static_cast<int32_t>(next32());
            if (width < 0) {
                flags.push_back('-');
                width = -width;
            }
        }
        if (precisionFromArg) {
            precision = static_cast<int32_t>(next32());
            if (precision < 0) {
                precision = -1; // Negative precision is treated as if it's omitted.
            }
        }

        if (cur >= fmt.size()) {
            appendText(fmt.substr(i));
            break;
        }

        char spec = fmt[cur];
        ++cur;

        std::ostringstream pieceBuilder;
        pieceBuilder << "%";
        if (!flags.empty()) pieceBuilder << flags;
        if (width >= 0) pieceBuilder << width;
        if (precision >= 0) pieceBuilder << "." << precision;
        // Only the integer conversions keep a length modifier, and only where guest and
        // host agree on it: the guest's l/z/t are 32-bit, and %ls/%lc would make the host
        // read the narrow value passed below as wide characters.
        const bool integerSpec = spec != '\0' && std::strchr("diuxXo", spec) != nullptr;
        if (integerSpec && (length == "ll" || length == "h" || length == "hh")) {
            pieceBuilder << length;
        }
        pieceBuilder << spec;
        const std::string piece = pieceBuilder.str();

        auto appendWithSnprintf = [&](auto value) {
            int needed = std::snprintf(nullptr, 0, piece.c_str(), value);
            if (needed <= 0) {
                return;
            }
            std::string buf(static_cast<size_t>(needed) + 1, '\0');
            std::snprintf(buf.data(), buf.size(), piece.c_str(), value);
            buf.resize(static_cast<size_t>(needed));
            appendText(buf);
        };

        switch (spec) {
            case 's': {
                uint32_t ptr = next32();
                std::string guest = readString(ptr);
                appendWithSnprintf(guest.c_str());
                break;
            }
            case 'c': {
                char ch = static_cast<char>(next32() & 0xFF);
                appendWithSnprintf(ch);
                break;
            }
            case 'p': {
                uint32_t ptr = next32();
                appendWithSnprintf(reinterpret_cast<void*>(static_cast<uintptr_t>(ptr)));
                break;
            }
            case 'd':
            case 'i': {
                if (length == "ll") {
                    int64_t v = static_cast<int64_t>(next64());
                    appendWithSnprintf(v);
                } else {
                    int32_t v = static_cast<int32_t>(next32());
                    appendWithSnprintf(v);
                }
                break;
            }
            case 'u':
            case 'x':
            case 'X':
            case 'o': {
                if (length == "ll") {
                    uint64_t v = next64();
                    appendWithSnprintf(v);
                } else {
                    uint32_t v = next32();
                    appendWithSnprintf(v);
                }
                break;
            }
            case 'f': case 'F':
            case 'e': case 'E':
            case 'g': case 'G':
            case 'a': case 'A': {
                double v = nextDouble();
                appendWithSnprintf(v);
                break;
            }
            case 'n': {
                uint32_t ptr = next32();
                if (ptr != 0) {
                    Memory::Write32(ptr, static_cast<uint32_t>(outChars));
                }
                break;
            }
            default: {
                // Unknown specifier, copy literally so we don't drop information.
                appendText(fmt.substr(i, cur - i));
                break;
            }
        }

        i = cur;
    }

    return out.str();
}

// 0x801D09CC is WUD_DEBUGPrint in the PAL executable. Its retail body only
// performs the compiler-generated variadic prologue and returns; it does not
// format or emit text. Keep it on the ordinary translated path so its guest
// memory effects remain exact without inventing expensive host-side logging.
