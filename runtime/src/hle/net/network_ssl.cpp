#include "network_internal.h"
#include "runtime_config.h"
#include "runtime_log.h"

#ifndef _WIN32
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

#include <cstring>
#include <filesystem>
#include <optional>
#endif

namespace NetworkHle {

enum SslError {
    SSL_OK = 0,
    SSL_ERR_FAILED = -1,
    SSL_ERR_RAGAIN = -2,
    SSL_ERR_WAGAIN = -3,
    SSL_ERR_SYSCALL = -5,
    SSL_ERR_ZERO = -6,
    SSL_ERR_ID = -8,
    SSL_ERR_VCOMMONNAME = -9,
};

enum SslIoctlv {
    IOCTLV_NET_SSL_NEW = 0x01,
    IOCTLV_NET_SSL_CONNECT = 0x02,
    IOCTLV_NET_SSL_DOHANDSHAKE = 0x03,
    IOCTLV_NET_SSL_READ = 0x04,
    IOCTLV_NET_SSL_WRITE = 0x05,
    IOCTLV_NET_SSL_SHUTDOWN = 0x06,
    IOCTLV_NET_SSL_SETCLIENTCERT = 0x07,
    IOCTLV_NET_SSL_SETCLIENTCERTDEFAULT = 0x08,
    IOCTLV_NET_SSL_REMOVECLIENTCERT = 0x09,
    IOCTLV_NET_SSL_SETROOTCA = 0x0A,
    IOCTLV_NET_SSL_SETROOTCADEFAULT = 0x0B,
    IOCTLV_NET_SSL_DOHANDSHAKEEX = 0x0C,
    IOCTLV_NET_SSL_SETBUILTINROOTCA = 0x0D,
    IOCTLV_NET_SSL_SETBUILTINCLIENTCERT = 0x0E,
    IOCTLV_NET_SSL_DISABLEVERIFYOPTIONFORDEBUG = 0x0F,
    IOCTLV_NET_SSL_DEBUGGETVERSION = 0x14,
    IOCTLV_NET_SSL_DEBUGGETTIME = 0x15,
};

constexpr int kMaxSslSessions = 4;

struct SslSession {
    bool active = false;
    bool handshaked = false;
    bool plaintextWfc = false;
    uint32_t socketFd = UINT32_MAX;
    NativeSocket native = kInvalidSocket;
    std::string hostname;
    std::vector<uint8_t> nasWriteBuffer;
    std::vector<uint8_t> decrypted;
    std::vector<uint8_t> encryptedExtra;
    // Failure-report one-shots; cleared with the session by ClearSslSession. The
    // handshake re-runs on every read/write, so a failing one repeats forever.
    bool loggedHandshakeFail = false;
    int32_t lastLoggedReadError = 0;
    int32_t lastLoggedWriteError = 0;
#ifdef _WIN32
    bool haveCred = false;
    bool haveContext = false;
    CredHandle cred{};
    CtxtHandle context{};
    SecPkgContext_StreamSizes sizes{};
#else
    bool haveSsl = false;
    mbedtls_ssl_context sslContext{};
    mbedtls_ssl_config sslConfig{};
    mbedtls_net_context netContext{};
#endif
};

static std::array<SslSession, kMaxSslSessions> g_sslSessions;

// Logging wrapper around the platform handshake implementation; see below.
static int32_t SslHandshake(SslSession& ssl);

static bool IsRetroNasSslHost(std::string_view hostname) {
    if (!RetroRewindProfileActive()) {
        return false;
    }
    const std::string lowered = Lower(hostname);
    return StartsWith(lowered, "nas.") || StartsWith(lowered, "naswii.");
}

static bool IsRetroPlaintextSslHost(std::string_view hostname) {
    if (IsRetroNasSslHost(hostname)) {
        return true;
    }
    if (!RetroRewindProfileActive()) {
        return false;
    }

    const std::string lowered = Lower(hostname);
    return StartsWith(lowered, "sake.gs.") ||
           lowered.find(".sake.gs.") != std::string::npos ||
           StartsWith(lowered, "gamestats.gs.") ||
           lowered.find(".gamestats.gs.") != std::string::npos ||
           StartsWith(lowered, "gamestats2.gs.") ||
           lowered.find(".gamestats2.gs.") != std::string::npos ||
           StartsWith(lowered, "race.gs.") ||
           lowered.find(".race.gs.") != std::string::npos;
}

static std::optional<size_t> ParseHttpContentLength(std::string_view headers) {
    std::optional<size_t> parsedLength;
    size_t lineStart = 0;
    while (lineStart <= headers.size()) {
        const size_t lineEnd = headers.find("\r\n", lineStart);
        const std::string_view line = headers.substr(
            lineStart, lineEnd == std::string_view::npos ? headers.size() - lineStart : lineEnd - lineStart);
        const size_t colon = line.find(':');
        if (colon != std::string_view::npos && Lower(line.substr(0, colon)) == "content-length") {
            size_t valueStart = colon + 1;
            while (valueStart < line.size() && (line[valueStart] == ' ' || line[valueStart] == '\t')) {
                ++valueStart;
            }
            size_t valueEnd = line.size();
            while (valueEnd > valueStart && (line[valueEnd - 1] == ' ' || line[valueEnd - 1] == '\t')) {
                --valueEnd;
            }
            size_t value = 0;
            const auto [end, error] = std::from_chars(
                line.data() + valueStart, line.data() + valueEnd, value, 10);
            if (error != std::errc{} || end != line.data() + valueEnd ||
                (parsedLength && *parsedLength != value)) {
                return std::nullopt;
            }
            parsedLength = value;
        }
        if (lineEnd == std::string_view::npos) {
            break;
        }
        lineStart = lineEnd + 2;
    }
    return parsedLength;
}

// The Retro-WFC server rejects a NAS "POST /ac" auth body split across TCP
// segments, so buffer guest chunks until Content-Length is satisfied, then
// flush as one write. Flush at 16 KiB if it never terminates, and immediately
// if Content-Length can't be parsed.
static NasSslWriteAction AccumulateNasRequest(std::vector<uint8_t>& buffer, const uint8_t* data,
                                              uint32_t size, std::vector<uint8_t>& patched) {
    buffer.insert(buffer.end(), data, data + size);
    if (buffer.size() > 16 * 1024) {
        patched.swap(buffer);
        buffer.clear();
        return NasSslWriteAction::Ready;
    }

    const std::string accumulated(reinterpret_cast<const char*>(buffer.data()), buffer.size());
    const size_t headerEnd = accumulated.find("\r\n\r\n");
    if (headerEnd == std::string::npos) {
        return NasSslWriteAction::Buffered;
    }

    const std::optional<size_t> contentLength =
        ParseHttpContentLength(std::string_view(accumulated).substr(0, headerEnd));
    if (!contentLength) {
        patched.swap(buffer);
        buffer.clear();
        return NasSslWriteAction::Ready;
    }

    const size_t requestSize = headerEnd + 4 + *contentLength;
    if (accumulated.size() < requestSize) {
        return NasSslWriteAction::Buffered;
    }

    patched.assign(buffer.begin(), buffer.begin() + requestSize);
    if (accumulated.size() > requestSize) {
        patched.insert(patched.end(), buffer.begin() + requestSize, buffer.end());
    }
    buffer.clear();
    return NasSslWriteAction::Ready;
}

static bool StartsNasAuthRequest(const uint8_t* data, uint32_t size) {
    return size >= 9 && std::memcmp(data, "POST /ac ", 9) == 0;
}

// SSL route: the session carries the hostname the guest asked for, so the NAS
// host is identified by name. Deliberately not IsRetroNasSslHost - the SSL write
// path re-assembles NAS auth regardless of which profile is active.
static NasSslWriteAction PrepareNasSslWrite(SslSession& ssl, const uint8_t* data, uint32_t size,
                                            std::vector<uint8_t>& patched) {
    if (!data || size == 0) {
        return NasSslWriteAction::PassThrough;
    }
    const std::string loweredHost = Lower(ssl.hostname);
    const bool isNasHost = StartsWith(loweredHost, "nas.") || StartsWith(loweredHost, "naswii.");
    if (!isNasHost || (!StartsNasAuthRequest(data, size) && ssl.nasWriteBuffer.empty())) {
        return NasSslWriteAction::PassThrough;
    }
    return AccumulateNasRequest(ssl.nasWriteBuffer, data, size, patched);
}

// Plain-TCP route: a rerouted 443->80 NAS connection has no hostname on the
// socket, so the peer port and the stream type are what identify it.
NasSslWriteAction PreparePlainNasTcpWrite(WiiSocket& socket, const uint8_t* data, uint32_t size,
                                                 std::vector<uint8_t>& patched) {
    if (!data || size == 0 || socket.type != SOCK_STREAM || socket.peerPort != 80) {
        return NasSslWriteAction::PassThrough;
    }
    if (!StartsNasAuthRequest(data, size) && socket.nasWriteBuffer.empty()) {
        return NasSslWriteAction::PassThrough;
    }
    return AccumulateNasRequest(socket.nasWriteBuffer, data, size, patched);
}

static bool WriteSslReturn(const std::vector<IoVector>& in, int32_t value) {
    if (in.empty() || !in[0].address || in[0].size < 4) {
        return false;
    }
    Memory::Write32(in[0].address, static_cast<uint32_t>(value));
    return true;
}

static int ReadSslId(const std::vector<IoVector>& out) {
    if (out.empty() || !out[0].address || out[0].size < 4) {
        return -1;
    }
    return static_cast<int>(Memory::Read32(out[0].address)) - 1;
}

static bool IsSslIdValid(int id) {
    return id >= 0 && id < kMaxSslSessions && g_sslSessions[id].active;
}

#ifdef _WIN32
static bool IsSecuritySuccess(SECURITY_STATUS status) {
    return status == SEC_E_OK || status == SEC_I_CONTINUE_NEEDED || status == SEC_I_INCOMPLETE_CREDENTIALS;
}

static bool SendAll(NativeSocket socket, const uint8_t* data, size_t size) {
    size_t offset = 0;
    while (offset < size) {
        const int chunk = static_cast<int>(std::min<size_t>(size - offset, 64 * 1024));
        const int ret = send(socket, reinterpret_cast<const char*>(data + offset), chunk, 0);
        if (ret <= 0) {
            // Reported by the SSL_WRITE / handshake caller as SSL_ERR_SYSCALL.
            return false;
        }
        offset += static_cast<size_t>(ret);
    }
    return true;
}

static int RecvBlocking(NativeSocket socket, std::vector<uint8_t>& buffer) {
    std::array<uint8_t, 16 * 1024> temp{};
    const int ret = recv(socket, reinterpret_cast<char*>(temp.data()), static_cast<int>(temp.size()), 0);
    if (ret > 0) {
        buffer.insert(buffer.end(), temp.begin(), temp.begin() + ret);
    }
    return ret;
}

static void KeepExtraBuffer(std::vector<uint8_t>& dest, const SecBuffer& buffer) {
    dest.clear();
    if (buffer.BufferType == SECBUFFER_EXTRA && buffer.pvBuffer && buffer.cbBuffer) {
        const auto* begin = static_cast<const uint8_t*>(buffer.pvBuffer);
        dest.assign(begin, begin + buffer.cbBuffer);
    }
}

static void ClearSslSession(SslSession& ssl) {
    if (ssl.haveContext) {
        DeleteSecurityContext(&ssl.context);
    }
    if (ssl.haveCred) {
        FreeCredentialsHandle(&ssl.cred);
    }
    ssl = {};
}

static int32_t EnsureSslCredentials(SslSession& ssl) {
    if (ssl.haveCred) {
        return SSL_OK;
    }

    SCHANNEL_CRED cred{};
    cred.dwVersion = SCHANNEL_CRED_VERSION;
    // Leave certificate validation to Schannel. SCH_CRED_MANUAL_CRED_VALIDATION
    // suppresses that validation and requires an explicit CertGetCertificateChain
    // implementation, which this HLE does not provide. The target hostname passed
    // to InitializeSecurityContextA below is therefore checked together with the
    // server certificate chain.
    cred.dwFlags = SCH_USE_STRONG_CRYPTO | SCH_CRED_NO_DEFAULT_CREDS |
                   SCH_CRED_AUTO_CRED_VALIDATION;

    TimeStamp expiry{};
    const SECURITY_STATUS status = AcquireCredentialsHandleA(
        nullptr, const_cast<LPSTR>(UNISP_NAME_A), SECPKG_CRED_OUTBOUND, nullptr, &cred, nullptr, nullptr,
        &ssl.cred, &expiry);
    if (status != SEC_E_OK) {
        return SSL_ERR_FAILED;
    }
    ssl.haveCred = true;
    return SSL_OK;
}

static int32_t SslHandshakeImpl(SslSession& ssl) {
    if (ssl.plaintextWfc) {
        ssl.handshaked = true;
        return SSL_OK;
    }

    // Schannel can authenticate a certificate chain without authenticating a
    // server identity when no target name is supplied. Refuse that ambiguous
    // mode rather than accepting a certificate for an unrelated endpoint.
    if (ssl.hostname.empty()) {
        return SSL_ERR_VCOMMONNAME;
    }

    const int32_t credRet = EnsureSslCredentials(ssl);
    if (credRet != SSL_OK) {
        return credRet;
    }
    if (ssl.native == kInvalidSocket) {
        return SSL_ERR_SYSCALL;
    }
    if (ssl.handshaked) {
        return SSL_OK;
    }

    DWORD attrs = 0;
    TimeStamp expiry{};
    std::vector<uint8_t> incoming = std::move(ssl.encryptedExtra);
    ssl.encryptedExtra.clear();
    constexpr DWORD flags = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_CONFIDENTIALITY |
                            ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM | ISC_REQ_EXTENDED_ERROR;

    for (int step = 0; step < 128; ++step) {
        SecBuffer outBuffer{};
        outBuffer.BufferType = SECBUFFER_TOKEN;
        SecBufferDesc outDesc{};
        outDesc.ulVersion = SECBUFFER_VERSION;
        outDesc.cBuffers = 1;
        outDesc.pBuffers = &outBuffer;

        SecBuffer inBuffers[2]{};
        SecBufferDesc inDesc{};
        SecBufferDesc* inDescPtr = nullptr;
        if (!incoming.empty()) {
            inBuffers[0].BufferType = SECBUFFER_TOKEN;
            inBuffers[0].pvBuffer = incoming.data();
            inBuffers[0].cbBuffer = static_cast<unsigned long>(incoming.size());
            inBuffers[1].BufferType = SECBUFFER_EMPTY;
            inDesc.ulVersion = SECBUFFER_VERSION;
            inDesc.cBuffers = 2;
            inDesc.pBuffers = inBuffers;
            inDescPtr = &inDesc;
        }

        const SECURITY_STATUS status = InitializeSecurityContextA(
            &ssl.cred, ssl.haveContext ? &ssl.context : nullptr,
            const_cast<char*>(ssl.hostname.c_str()), flags, 0, SECURITY_NATIVE_DREP,
            inDescPtr, 0, &ssl.context, &outDesc, &attrs, &expiry);
        if (status != SEC_E_INVALID_HANDLE) {
            ssl.haveContext = true;
        }

        if (outBuffer.pvBuffer && outBuffer.cbBuffer) {
            const bool sent = SendAll(ssl.native, static_cast<const uint8_t*>(outBuffer.pvBuffer), outBuffer.cbBuffer);
            FreeContextBuffer(outBuffer.pvBuffer);
            if (!sent) {
                return SSL_ERR_SYSCALL;
            }
        }

        if (status == SEC_E_OK) {
            if (inDescPtr) {
                KeepExtraBuffer(ssl.encryptedExtra, inBuffers[1]);
            }
            const SECURITY_STATUS sizeStatus =
                QueryContextAttributesA(&ssl.context, SECPKG_ATTR_STREAM_SIZES, &ssl.sizes);
            if (sizeStatus != SEC_E_OK) {
                return SSL_ERR_FAILED;
            }
            ssl.handshaked = true;
            return SSL_OK;
        }

        if (status == SEC_E_INCOMPLETE_MESSAGE) {
            const int ret = RecvBlocking(ssl.native, incoming);
            if (ret == 0) {
                return SSL_ERR_ZERO;
            }
            if (ret < 0) {
                return SSL_ERR_RAGAIN;
            }
            continue;
        }

        if (status == SEC_I_CONTINUE_NEEDED || status == SEC_I_INCOMPLETE_CREDENTIALS) {
            std::vector<uint8_t> extra;
            if (inDescPtr) {
                KeepExtraBuffer(extra, inBuffers[1]);
            }
            incoming = std::move(extra);
            const int ret = RecvBlocking(ssl.native, incoming);
            if (ret == 0) {
                return SSL_ERR_ZERO;
            }
            if (ret < 0) {
                return SSL_ERR_RAGAIN;
            }
            continue;
        }

        return status == SEC_E_WRONG_PRINCIPAL ? SSL_ERR_VCOMMONNAME : SSL_ERR_FAILED;
    }

    return SSL_ERR_FAILED;
}

static int32_t SslWrite(SslSession& ssl, const uint8_t* data, uint32_t size) {
    if (!data || size == 0) {
        return SSL_ERR_ZERO;
    }
    const int32_t handshakeRet = SslHandshake(ssl);
    if (handshakeRet != SSL_OK) {
        return handshakeRet;
    }

    if (ssl.plaintextWfc) {
        return SendAll(ssl.native, data, size) ? static_cast<int32_t>(size) : SSL_ERR_SYSCALL;
    }

    uint32_t total = 0;
    while (total < size) {
        const uint32_t chunk = std::min<uint32_t>(size - total, ssl.sizes.cbMaximumMessage);
        std::vector<uint8_t> packet(ssl.sizes.cbHeader + chunk + ssl.sizes.cbTrailer);
        std::memcpy(packet.data() + ssl.sizes.cbHeader, data + total, chunk);

        SecBuffer buffers[4]{};
        buffers[0].BufferType = SECBUFFER_STREAM_HEADER;
        buffers[0].pvBuffer = packet.data();
        buffers[0].cbBuffer = ssl.sizes.cbHeader;
        buffers[1].BufferType = SECBUFFER_DATA;
        buffers[1].pvBuffer = packet.data() + ssl.sizes.cbHeader;
        buffers[1].cbBuffer = chunk;
        buffers[2].BufferType = SECBUFFER_STREAM_TRAILER;
        buffers[2].pvBuffer = packet.data() + ssl.sizes.cbHeader + chunk;
        buffers[2].cbBuffer = ssl.sizes.cbTrailer;
        buffers[3].BufferType = SECBUFFER_EMPTY;

        SecBufferDesc desc{};
        desc.ulVersion = SECBUFFER_VERSION;
        desc.cBuffers = 4;
        desc.pBuffers = buffers;

        const SECURITY_STATUS status = EncryptMessage(&ssl.context, 0, &desc, 0);
        if (status != SEC_E_OK) {
            return SSL_ERR_FAILED;
        }

        const size_t encryptedSize =
            static_cast<size_t>(buffers[0].cbBuffer) + buffers[1].cbBuffer + buffers[2].cbBuffer;
        if (!SendAll(ssl.native, packet.data(), encryptedSize)) {
            return SSL_ERR_SYSCALL;
        }
        total += chunk;
    }
    return static_cast<int32_t>(total);
}

static int32_t SslRead(SslSession& ssl, uint8_t* out, uint32_t size) {
    if (!out || size == 0) {
        return SSL_ERR_ZERO;
    }
    const int32_t handshakeRet = SslHandshake(ssl);
    if (handshakeRet != SSL_OK) {
        return handshakeRet;
    }

    if (ssl.plaintextWfc) {
        const int ret = recv(ssl.native, reinterpret_cast<char*>(out), static_cast<int>(size), 0);
        if (ret == 0) {
            return SSL_ERR_ZERO;
        }
        if (ret < 0) {
            return SSL_ERR_RAGAIN;
        }
        return ret;
    }

    while (ssl.decrypted.empty()) {
        std::vector<uint8_t> encrypted = std::move(ssl.encryptedExtra);
        ssl.encryptedExtra.clear();
        if (encrypted.empty()) {
            const int ret = RecvBlocking(ssl.native, encrypted);
            if (ret == 0) {
                return SSL_ERR_ZERO;
            }
            if (ret < 0) {
                return SSL_ERR_RAGAIN;
            }
        }

        for (;;) {
            SecBuffer buffers[4]{};
            buffers[0].BufferType = SECBUFFER_DATA;
            buffers[0].pvBuffer = encrypted.data();
            buffers[0].cbBuffer = static_cast<unsigned long>(encrypted.size());
            buffers[1].BufferType = SECBUFFER_EMPTY;
            buffers[2].BufferType = SECBUFFER_EMPTY;
            buffers[3].BufferType = SECBUFFER_EMPTY;

            SecBufferDesc desc{};
            desc.ulVersion = SECBUFFER_VERSION;
            desc.cBuffers = 4;
            desc.pBuffers = buffers;

            const SECURITY_STATUS status = DecryptMessage(&ssl.context, &desc, 0, nullptr);
            if (status == SEC_E_INCOMPLETE_MESSAGE) {
                const int ret = RecvBlocking(ssl.native, encrypted);
                if (ret == 0) {
                    return SSL_ERR_ZERO;
                }
                if (ret < 0) {
                    return SSL_ERR_RAGAIN;
                }
                continue;
            }
            if (status == SEC_I_CONTEXT_EXPIRED) {
                return SSL_ERR_ZERO;
            }
            if (!IsSecuritySuccess(status) && status != SEC_I_RENEGOTIATE) {
                return SSL_ERR_FAILED;
            }

            for (const SecBuffer& buffer : buffers) {
                if (buffer.BufferType == SECBUFFER_DATA && buffer.pvBuffer && buffer.cbBuffer) {
                    const auto* begin = static_cast<const uint8_t*>(buffer.pvBuffer);
                    ssl.decrypted.insert(ssl.decrypted.end(), begin, begin + buffer.cbBuffer);
                } else if (buffer.BufferType == SECBUFFER_EXTRA && buffer.pvBuffer && buffer.cbBuffer) {
                    const auto* begin = static_cast<const uint8_t*>(buffer.pvBuffer);
                    ssl.encryptedExtra.assign(begin, begin + buffer.cbBuffer);
                }
            }
            break;
        }
    }

    const uint32_t copied = std::min<uint32_t>(size, static_cast<uint32_t>(ssl.decrypted.size()));
    std::memcpy(out, ssl.decrypted.data(), copied);
    ssl.decrypted.erase(ssl.decrypted.begin(), ssl.decrypted.begin() + copied);
    return copied == 0 ? SSL_ERR_ZERO : static_cast<int32_t>(copied);
}
#else
// Windows gets TLS for free from the OS (Schannel, above) - mbed TLS is this project's own
// vendored equivalent for everywhere else (runtime/third_party/mbedtls, see runtime/CMakeLists.txt
// for why mbed TLS specifically). The CA chain and RNG are expensive to set up (parsing ~150 root
// certificates, seeding entropy) and read-only once built, so they're shared process-wide instead
// of being redone per SSL session.
static bool g_mbedtlsCaLoaded = false;
static mbedtls_x509_crt g_mbedtlsCaChain;
static mbedtls_entropy_context g_mbedtlsEntropy;
static mbedtls_ctr_drbg_context g_mbedtlsCtrDrbg;

// Mirrors ax_mix.cpp's FindDspCoefficientRom exactly - same three places a bundled asset can live
// depending on platform and how the binary was launched (next to the desktop executable, the
// Android app's own data directory, or a source-tree checkout during development).
static std::optional<std::filesystem::path> FindCaCertificateBundle() {
    if (const auto executableDirectory = RuntimeConfigFile::ExecutableDirectory()) {
        const auto adjacent = *executableDirectory / "cacert.pem";
        if (std::filesystem::is_regular_file(adjacent)) {
            return adjacent;
        }
    }

#if defined(__ANDROID__)
    const auto androidAsset = RuntimeConfigFile::ApplicationDataDirectory() / "cacert.pem";
    if (std::filesystem::is_regular_file(androidAsset)) {
        return androidAsset;
    }
#endif

    for (auto base = std::filesystem::current_path(); !base.empty();) {
        const auto sourceTreeAsset = base / "runtime" / "assets" / "certs" / "cacert.pem";
        if (std::filesystem::is_regular_file(sourceTreeAsset)) {
            return sourceTreeAsset;
        }
        const auto parent = base.parent_path();
        if (parent == base) {
            break;
        }
        base = parent;
    }
    return std::nullopt;
}

// Lazy, once-per-process: the first real SSL use pays for parsing the CA bundle and seeding the
// RNG, every session after that reuses the result. Returns false (logging once) if the bundle is
// missing or unparseable - callers treat that as a normal handshake failure, not a crash, since a
// missing TLS root store shouldn't take down gameplay that never touches the network.
static bool EnsureMbedtlsGlobalsInitialized() {
    static const bool initialized = [] {
        mbedtls_x509_crt_init(&g_mbedtlsCaChain);
        mbedtls_entropy_init(&g_mbedtlsEntropy);
        mbedtls_ctr_drbg_init(&g_mbedtlsCtrDrbg);

        const char* personalization = "wiicompiled_ssl";
        if (mbedtls_ctr_drbg_seed(&g_mbedtlsCtrDrbg, mbedtls_entropy_func, &g_mbedtlsEntropy,
                                  reinterpret_cast<const unsigned char*>(personalization),
                                  std::strlen(personalization)) != 0) {
            NetFail("ssl: failed to seed TLS random number generator");
            return false;
        }

        const auto bundle = FindCaCertificateBundle();
        if (!bundle) {
            NetFail("ssl: missing TLS root CA bundle (cacert.pem) - HTTPS connections will fail");
            return false;
        }
        const int parseRet = mbedtls_x509_crt_parse_file(&g_mbedtlsCaChain, bundle->string().c_str());
        if (parseRet < 0) {
            char errorBuffer[256];
            mbedtls_strerror(parseRet, errorBuffer, sizeof(errorBuffer));
            NetFail("ssl: failed to parse CA bundle %s: %s", bundle->string().c_str(), errorBuffer);
            return false;
        }
        return true;
    }();
    g_mbedtlsCaLoaded = initialized;
    return initialized;
}

// Builds this session's mbed TLS handshake state exactly once - a second call (e.g. the handshake
// re-running after DOHANDSHAKE was already satisfied) is a no-op via ssl.haveSsl.
static int32_t EnsureMbedtlsSession(SslSession& ssl) {
    if (ssl.haveSsl) {
        return SSL_OK;
    }
    if (!EnsureMbedtlsGlobalsInitialized()) {
        return SSL_ERR_FAILED;
    }

    mbedtls_ssl_init(&ssl.sslContext);
    mbedtls_ssl_config_init(&ssl.sslConfig);
    if (mbedtls_ssl_config_defaults(&ssl.sslConfig, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                     MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
        return SSL_ERR_FAILED;
    }
    // Real certificate validation, matching Schannel's SCH_CRED_AUTO_CRED_VALIDATION on the
    // Windows side above - a self-signed or wrong-hostname certificate must fail the handshake,
    // not just get logged.
    mbedtls_ssl_conf_authmode(&ssl.sslConfig, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&ssl.sslConfig, &g_mbedtlsCaChain, nullptr);
    mbedtls_ssl_conf_rng(&ssl.sslConfig, mbedtls_ctr_drbg_random, &g_mbedtlsCtrDrbg);
    if (mbedtls_ssl_setup(&ssl.sslContext, &ssl.sslConfig) != 0) {
        return SSL_ERR_FAILED;
    }
    // The hostname drives both SNI (which certificate the server presents) and the CN/SAN check
    // mbedtls_ssl_conf_authmode enforces above - required, not optional, same reasoning as the
    // Windows path's own "refuse an empty hostname" check just above SslHandshakeImpl.
    mbedtls_ssl_set_hostname(&ssl.sslContext, ssl.hostname.c_str());

    // ssl.native is already a connected, blocking POSIX socket by the time DOHANDSHAKE runs (see
    // IOCTLV_NET_SSL_CONNECT) - wrapping it in mbed TLS's own net_context and using its own
    // send/recv callbacks reuses a well-tested implementation instead of hand-rolling one.
    ssl.netContext.fd = static_cast<int>(ssl.native);
    mbedtls_ssl_set_bio(&ssl.sslContext, &ssl.netContext, mbedtls_net_send, mbedtls_net_recv, nullptr);

    ssl.haveSsl = true;
    return SSL_OK;
}

static void ClearSslSession(SslSession& ssl) {
    if (ssl.haveSsl) {
        mbedtls_ssl_free(&ssl.sslContext);
        mbedtls_ssl_config_free(&ssl.sslConfig);
    }
    ssl = {};
}

static int32_t SslHandshakeImpl(SslSession& ssl) {
    if (ssl.plaintextWfc) {
        ssl.handshaked = true;
        return SSL_OK;
    }
    if (ssl.handshaked) {
        return SSL_OK;
    }
    if (ssl.native == kInvalidSocket) {
        return SSL_ERR_SYSCALL;
    }
    // mbed TLS can authenticate a certificate chain without authenticating a server identity when
    // no hostname is set - refuse that ambiguous mode, matching the Windows path's own check.
    if (ssl.hostname.empty()) {
        return SSL_ERR_VCOMMONNAME;
    }

    const int32_t setupRet = EnsureMbedtlsSession(ssl);
    if (setupRet != SSL_OK) {
        return setupRet;
    }

    int handshakeRet;
    while ((handshakeRet = mbedtls_ssl_handshake(&ssl.sslContext)) != 0) {
        if (handshakeRet == MBEDTLS_ERR_SSL_WANT_READ || handshakeRet == MBEDTLS_ERR_SSL_WANT_WRITE) {
            // The socket is set blocking before the handshake ever starts (IOCTLV_NET_SSL_CONNECT),
            // so mbed TLS's own send/recv callbacks either return real data or a real error - a
            // WANT_READ/WANT_WRITE here means retry the same call immediately, not "come back
            // later" the way it would for a nonblocking socket.
            continue;
        }
        char errorBuffer[256];
        mbedtls_strerror(handshakeRet, errorBuffer, sizeof(errorBuffer));
        NetFail("ssl handshake FAILED host=%s mbedtls_err=%s", ssl.hostname.c_str(), errorBuffer);
        return handshakeRet == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED ? SSL_ERR_VCOMMONNAME : SSL_ERR_FAILED;
    }
    ssl.handshaked = true;
    return SSL_OK;
}

static int32_t SslWrite(SslSession& ssl, const uint8_t* data, uint32_t size) {
    if (!data || size == 0) {
        return SSL_ERR_ZERO;
    }
    const int32_t handshakeRet = SslHandshake(ssl);
    if (handshakeRet != SSL_OK) {
        return handshakeRet;
    }

    if (ssl.plaintextWfc) {
        uint32_t total = 0;
        while (total < size) {
            const ssize_t sent = send(ssl.native, data + total, size - total, 0);
            if (sent <= 0) {
                return SSL_ERR_SYSCALL;
            }
            total += static_cast<uint32_t>(sent);
        }
        return static_cast<int32_t>(total);
    }

    for (;;) {
        const int ret = mbedtls_ssl_write(&ssl.sslContext, data, size);
        if (ret >= 0) {
            return ret;
        }
        if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
            continue;
        }
        return SSL_ERR_FAILED;
    }
}

static int32_t SslRead(SslSession& ssl, uint8_t* out, uint32_t size) {
    if (!out || size == 0) {
        return SSL_ERR_ZERO;
    }
    const int32_t handshakeRet = SslHandshake(ssl);
    if (handshakeRet != SSL_OK) {
        return handshakeRet;
    }

    if (ssl.plaintextWfc) {
        const ssize_t ret = recv(ssl.native, out, size, 0);
        if (ret == 0) {
            return SSL_ERR_ZERO;
        }
        if (ret < 0) {
            return SSL_ERR_RAGAIN;
        }
        return static_cast<int32_t>(ret);
    }

    const int ret = mbedtls_ssl_read(&ssl.sslContext, out, size);
    if (ret == 0 || ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
        return SSL_ERR_ZERO;
    }
    if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
        return SSL_ERR_RAGAIN;
    }
    if (ret < 0) {
        return SSL_ERR_FAILED;
    }
    return ret;
}
#endif

// The handshake runs on every SSL read/write, so a failure repeats for as long
// as the session lives; report only the first one.
static int32_t SslHandshake(SslSession& ssl) {
    const int32_t result = SslHandshakeImpl(ssl);
    if (result != SSL_OK && !ssl.loggedHandshakeFail) {
        ssl.loggedHandshakeFail = true;
        NetFail("ssl handshake FAILED host=%s ssl_err=%d",
                ssl.hostname.empty() ? "?" : ssl.hostname.c_str(), result);
    }
    return result;
}

void ClearSslSessionsForSocket(uint32_t fd) {
    for (SslSession& ssl : g_sslSessions) {
        if (ssl.active && ssl.socketFd == fd) {
            ClearSslSession(ssl);
        }
    }
}

int32_t HandleSslIoctlv(uint32_t cmd, const std::vector<IoVector>& in, const std::vector<IoVector>& out) {

    switch (cmd) {
    case IOCTLV_NET_SSL_NEW: {
        // out[0] carries the guest verify option. Host TLS verification is
        // always enforced by Schannel, but still read this value so a request
        // pointing outside guest memory faults here as it always has.
        if (!out.empty() && out[0].address && out[0].size >= 4) {
            (void)Memory::Read32(out[0].address);
        }
        std::string hostname = out.size() > 1 ? ReadGuestString(out[1].address, out[1].size) : "";

        for (int i = 0; i < kMaxSslSessions; ++i) {
            if (!g_sslSessions[i].active) {
                ClearSslSession(g_sslSessions[i]);
                g_sslSessions[i].active = true;
                g_sslSessions[i].hostname = hostname;
                WriteSslReturn(in, i + 1);
                return 0;
            }
        }
        WriteSslReturn(in, SSL_ERR_FAILED);
        NetFail("SSL_NEW host=%s FAILED: all %d session slots in use",
                hostname.empty() ? "?" : hostname.c_str(), kMaxSslSessions);
        return 0;
    }
    case IOCTLV_NET_SSL_CONNECT: {
        const int sslId = ReadSslId(out);
        if (!IsSslIdValid(sslId)) {
            WriteSslReturn(in, SSL_ERR_ID);
            return 0;
        }
        if (out.size() < 2 || !out[1].address || out[1].size < 4) {
            WriteSslReturn(in, SSL_ERR_FAILED);
            return 0;
        }
        const uint32_t socketFd = Memory::Read32(out[1].address);
        WiiSocket* socket = GetWiiSocket(socketFd);
        if (!socket) {
            WriteSslReturn(in, SSL_ERR_SYSCALL);
            return 0;
        }

        SslSession& ssl = g_sslSessions[sslId];
        ssl.socketFd = socketFd;
        ssl.native = socket->native;
        ssl.plaintextWfc = false;
        socket->nonblocking = false;
        SetNonBlocking(socket->native, false);
        if (IsRetroPlaintextSslHost(ssl.hostname) && socket->peerPort == 443) {
            const int32_t reroute = ReconnectWiiSocket(*socket, 80);
            if (reroute != 0) {
                WriteSslReturn(in, SSL_ERR_SYSCALL);
                NetFail("SSL_CONNECT host=%s 443->80 plaintext reroute FAILED wii=%d",
                        ssl.hostname.c_str(), reroute);
                return 0;
            }
            ssl.native = socket->native;
            ssl.plaintextWfc = true;
        }
#ifdef _WIN32
        const int timeoutMs = 15000;
        setsockopt(socket->native, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));
        setsockopt(socket->native, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));
#endif
        WriteSslReturn(in, SSL_OK);
        return 0;
    }
    case IOCTLV_NET_SSL_DOHANDSHAKE:
    case IOCTLV_NET_SSL_DOHANDSHAKEEX: {
        const int sslId = ReadSslId(out);
        WriteSslReturn(in, IsSslIdValid(sslId) ? SslHandshake(g_sslSessions[sslId]) : SSL_ERR_ID);
        return 0;
    }
    case IOCTLV_NET_SSL_WRITE: {
        const int sslId = ReadSslId(out);
        if (!IsSslIdValid(sslId)) {
            WriteSslReturn(in, SSL_ERR_ID);
            return 0;
        }
        if (out.size() < 2 || !out[1].address) {
            WriteSslReturn(in, SSL_ERR_FAILED);
            return 0;
        }
        const auto* data = Memory::GetPointer(out[1].address, out[1].size);
        std::vector<uint8_t> patched;
        const NasSslWriteAction nasAction = PrepareNasSslWrite(g_sslSessions[sslId], data, out[1].size, patched);
        if (nasAction == NasSslWriteAction::Buffered) {
            WriteSslReturn(in, static_cast<int32_t>(out[1].size));
            return 0;
        }

        const bool patchedSslWrite = nasAction == NasSslWriteAction::Ready;

        const uint8_t* writeData = patchedSslWrite ? patched.data() : data;
        const uint32_t writeSize = patchedSslWrite ? static_cast<uint32_t>(patched.size()) : out[1].size;
        SslSession& writeSession = g_sslSessions[sslId];
        int32_t result = SslWrite(writeSession, writeData, writeSize);
        if (patchedSslWrite && result == static_cast<int32_t>(writeSize)) {
            result = static_cast<int32_t>(out[1].size);
        }
        // SSL_ERR_WAGAIN is a retry, not a failure; the SDK re-issues the write,
        // so only a change of error is reported.
        if (result < 0 && result != SSL_ERR_WAGAIN && result != writeSession.lastLoggedWriteError) {
            writeSession.lastLoggedWriteError = result;
            NetFail("SSL_WRITE host=%s size=%u FAILED ssl_err=%d",
                    writeSession.hostname.empty() ? "?" : writeSession.hostname.c_str(), writeSize,
                    result);
        }
        WriteSslReturn(in, result);
        return 0;
    }
    case IOCTLV_NET_SSL_READ: {
        const int sslId = ReadSslId(out);
        if (!IsSslIdValid(sslId)) {
            WriteSslReturn(in, SSL_ERR_ID);
            return 0;
        }
        if (in.size() < 2 || !in[1].address) {
            WriteSslReturn(in, SSL_ERR_FAILED);
            return 0;
        }
        auto* data = Memory::GetPointer(in[1].address, in[1].size);
        SslSession& readSession = g_sslSessions[sslId];
        const int32_t result = SslRead(readSession, data, in[1].size);
        if (result < 0 && result != SSL_ERR_RAGAIN && result != readSession.lastLoggedReadError) {
            readSession.lastLoggedReadError = result;
            NetFail("SSL_READ host=%s FAILED ssl_err=%d%s",
                    readSession.hostname.empty() ? "?" : readSession.hostname.c_str(), result,
                    result == SSL_ERR_ZERO ? " (peer closed)" : "");
        }
        WriteSslReturn(in, result);
        return 0;
    }
    case IOCTLV_NET_SSL_SHUTDOWN: {
        const int sslId = ReadSslId(out);
        if (!IsSslIdValid(sslId)) {
            WriteSslReturn(in, SSL_ERR_ID);
            return 0;
        }
        ClearSslSession(g_sslSessions[sslId]);
        WriteSslReturn(in, SSL_OK);
        return 0;
    }
    case IOCTLV_NET_SSL_SETCLIENTCERT:
    case IOCTLV_NET_SSL_SETCLIENTCERTDEFAULT:
    case IOCTLV_NET_SSL_REMOVECLIENTCERT:
    case IOCTLV_NET_SSL_SETROOTCA:
    case IOCTLV_NET_SSL_SETROOTCADEFAULT:
    case IOCTLV_NET_SSL_SETBUILTINROOTCA:
    case IOCTLV_NET_SSL_SETBUILTINCLIENTCERT:
    case IOCTLV_NET_SSL_DISABLEVERIFYOPTIONFORDEBUG: {
        const int sslId = ReadSslId(out);
        WriteSslReturn(in, IsSslIdValid(sslId) ? SSL_OK : SSL_ERR_ID);
        return 0;
    }
    case IOCTLV_NET_SSL_DEBUGGETVERSION:
    case IOCTLV_NET_SSL_DEBUGGETTIME:
        WriteSslReturn(in, SSL_OK);
        return 0;
    default:
        WriteSslReturn(in, SSL_ERR_FAILED);
        return 0;
    }
}

}  // namespace NetworkHle
