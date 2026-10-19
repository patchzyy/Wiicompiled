// SPDX-License-Identifier: GPL-2.0-or-later
//
// Copyright 2010 Dolphin Emulator Project
// Copyright 2007,2008 Segher Boessenkool
//
// Console identity constants and certificate layout details are derived from
// the Dolphin Emulator
// (https://github.com/dolphin-emu/dolphin):
//
//   * Source/Core/Core/IOS/IOSC.cpp - the default console identity constants
//     (device id, CA/MS ids, NG key id, default ECC private key and signature).
//   * Source/Core/Common/Crypto/ec.cpp - the sect233r1 public-key and signature
//     encodings used by IOS. Arithmetic, hashing, randomness and ECDSA are now
//     provided by Crypto++.
//
// This file is GPL-2.0-or-later as a consequence; see THIRD-PARTY-NOTICES.md.

#pragma once

#include "isa/big_endian.h"
#include "nand_path.h"

#include <cryptopp/eccrypto.h>
#include <cryptopp/ec2n.h>
#include <cryptopp/oids.h>
#include <cryptopp/osrng.h>
#include <cryptopp/sha.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>

namespace WiiEsCrypto {

using EcSignature = std::array<uint8_t, 60>;
using EcPublicKey = std::array<uint8_t, 60>;
using EccCert = std::array<uint8_t, 0x180>;

constexpr uint32_t kDeviceId = 0x0403AC68;
constexpr uint32_t kCaId = 1;
constexpr uint32_t kMsId = 2;
constexpr uint32_t kNgKeyId = 0x6AAB8C59;

constexpr std::array<uint8_t, 30> kDefaultPrivateKey{{
    0x00, 0xAB, 0xEE, 0xC1, 0xDD, 0xB4, 0xA6, 0x16, 0x6B, 0x70, 0xFD, 0x7E, 0x56, 0x67, 0x70,
    0x57, 0x55, 0x27, 0x38, 0xA3, 0x26, 0xC5, 0x46, 0x16, 0xF7, 0x62, 0xC9, 0xED, 0x73, 0xF2,
}};

constexpr EcSignature kDefaultSignature{{
    0x00, 0xD8, 0x81, 0x63, 0xB2, 0x00, 0x6B, 0x0B, 0x54, 0x82, 0x88, 0x63, 0x81, 0x1C, 0x00,
    0x71, 0x12, 0xED, 0xB7, 0xFD, 0x21, 0xAB, 0x0E, 0x50, 0x0E, 0x1F, 0xBF, 0x78, 0xAD, 0x37,
    0x00, 0x71, 0x8D, 0x82, 0x41, 0xEE, 0x45, 0x11, 0xC7, 0x3B, 0xAC, 0x08, 0xB6, 0x83, 0xDC,
    0x05, 0xB8, 0xA8, 0x90, 0x1F, 0xA8, 0x2A, 0x0E, 0x4E, 0x76, 0xEF, 0x44, 0x72, 0x99, 0xF8,
}};

struct Identity {
    uint32_t deviceId = kDeviceId;
    uint32_t caId = kCaId;
    uint32_t msId = kMsId;
    uint32_t ngKeyId = kNgKeyId;
    std::array<uint8_t, 30> privateKey = kDefaultPrivateKey;
    EcSignature signature = kDefaultSignature;
    bool fromNand = false;
};

inline std::optional<Identity> LoadIdentityFromKeysBin(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        return std::nullopt;
    }

    const std::streamoff fileSize = file.tellg();
    if (fileSize < 0x400) {
        return std::nullopt;
    }
    file.seekg(0, std::ios::beg);
    std::array<uint8_t, 0x400> dump{};
    file.read(reinterpret_cast<char*>(dump.data()), static_cast<std::streamsize>(dump.size()));
    if (!file) {
        return std::nullopt;
    }

    Identity identity{};
    identity.deviceId = BigEndian::Read32(dump.data() + 0x124);
    identity.msId = BigEndian::Read32(dump.data() + 0x200);
    identity.caId = BigEndian::Read32(dump.data() + 0x204);
    identity.ngKeyId = BigEndian::Read32(dump.data() + 0x208);
    std::copy_n(dump.data() + 0x128, identity.privateKey.size(), identity.privateKey.begin());
    std::copy_n(dump.data() + 0x20C, identity.signature.size(), identity.signature.begin());
    identity.fromNand = true;

    const bool privateKeyEmpty = std::all_of(identity.privateKey.begin(), identity.privateKey.end(),
                                            [](uint8_t b) { return b == 0; });
    if (identity.deviceId == 0 || identity.caId == 0 || identity.msId == 0 ||
        identity.ngKeyId == 0 || privateKeyEmpty) {
        return std::nullopt;
    }
    return identity;
}

inline std::optional<Identity> LoadIdentityFromNand(const std::filesystem::path& nandBase) {
    if (nandBase.empty()) {
        return std::nullopt;
    }
    return LoadIdentityFromKeysBin(nandBase / "keys.bin");
}

inline const Identity& CurrentIdentity() {
    static const Identity identity = [] {
        if (auto loaded = LoadIdentityFromNand(RuntimeNandPath::DiscoverNandRootPath())) {
            return *loaded;
        }
        return Identity{};
    }();
    return identity;
}

inline std::array<uint8_t, 20> Sha1(const uint8_t* data, size_t size) {
    std::array<uint8_t, 20> digest{};
    CryptoPP::SHA1 hash;
    hash.CalculateDigest(digest.data(), data, size);
    return digest;
}

using CryptoEcdsa = CryptoPP::ECDSA<CryptoPP::EC2N, CryptoPP::SHA1>;

inline CryptoEcdsa::PrivateKey MakePrivateKey(const uint8_t* key) {
    CryptoPP::DL_GroupParameters_EC<CryptoPP::EC2N> parameters(CryptoPP::ASN1::sect233r1());
    // Wii keys need not be canonical scalars; reduction preserves their public key.
    const CryptoPP::Integer exponent = CryptoPP::Integer(key, 30) % parameters.GetSubgroupOrder();
    if (exponent == CryptoPP::Integer::Zero()) {
        throw std::invalid_argument("Wii ES private key is outside the sect233r1 subgroup");
    }

    CryptoEcdsa::PrivateKey privateKey;
    privateKey.Initialize(parameters, exponent);
    return privateKey;
}

inline EcSignature SignMessage(const uint8_t* key, const uint8_t* data, size_t size) {
    const CryptoEcdsa::PrivateKey privateKey = MakePrivateKey(key);
    const CryptoEcdsa::Signer signer(privateKey);
    if (signer.SignatureLength() != EcSignature{}.size()) {
        throw std::runtime_error("Crypto++ returned an unexpected sect233r1 signature size");
    }

    thread_local CryptoPP::AutoSeededRandomPool random;
    EcSignature signature{};
    const size_t written = signer.SignMessage(random, data, size, signature.data());
    if (written != signature.size()) {
        throw std::runtime_error("Crypto++ produced a truncated Wii ES signature");
    }
    return signature;
}

inline EcPublicKey PrivToPub(const uint8_t* key) {
    const CryptoEcdsa::PrivateKey privateKey = MakePrivateKey(key);
    CryptoEcdsa::PublicKey publicKey;
    privateKey.MakePublicKey(publicKey);
    const auto& point = publicKey.GetPublicElement();
    EcPublicKey out{};
    point.x.Encode(out.data(), 30);
    point.y.Encode(out.data() + 30, 30);
    return out;
}

inline void WriteString(uint8_t* dst, size_t dstSize, const std::string& text) {
    std::memset(dst, 0, dstSize);
    std::memcpy(dst, text.data(), std::min(dstSize, text.size()));
}

inline std::string Hex8(uint32_t value) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out(8, '0');
    for (int i = 7; i >= 0; --i) {
        out[static_cast<size_t>(i)] = kHex[value & 0xFu];
        value >>= 4;
    }
    return out;
}

inline std::string Hex16(uint64_t value) {
    return Hex8(static_cast<uint32_t>(value >> 32)) + Hex8(static_cast<uint32_t>(value));
}

inline EccCert MakeBlankEccCert(const std::string& issuer, const std::string& name,
                                const uint8_t* privateKey, uint32_t keyId) {
    EccCert cert{};
    BigEndian::Write32(cert.data() + 0x00, 0x00010002u);
    WriteString(cert.data() + 0x80, 0x40, issuer);
    BigEndian::Write32(cert.data() + 0xC0, 2);
    WriteString(cert.data() + 0xC4, 0x40, name);
    BigEndian::Write32(cert.data() + 0x104, keyId);
    const EcPublicKey pub = PrivToPub(privateKey);
    std::copy(pub.begin(), pub.end(), cert.begin() + 0x108);
    return cert;
}

inline EccCert GetDeviceCertificate(const Identity& identity = CurrentIdentity()) {
    const std::string issuer = "Root-CA" + Hex8(identity.caId) + "-MS" + Hex8(identity.msId);
    const std::string name = "NG" + Hex8(identity.deviceId);
    EccCert cert = MakeBlankEccCert(issuer, name, identity.privateKey.data(), identity.ngKeyId);
    std::copy(identity.signature.begin(), identity.signature.end(), cert.begin() + 0x04);
    return cert;
}

inline void Sign(uint64_t titleId, const uint8_t* data, size_t dataSize,
                 const Identity& identity,
                 EcSignature& sigOut, EccCert& apCertOut) {
    std::array<uint8_t, 30> apPrivate{};
    apPrivate[0x1D] = 1;

    const std::string signer = "Root-CA" + Hex8(identity.caId) + "-MS" + Hex8(identity.msId) +
                               "-NG" + Hex8(identity.deviceId);
    const std::string name = "AP" + Hex16(titleId);
    apCertOut = MakeBlankEccCert(signer, name, apPrivate.data(), 0);

    const EcSignature apCertSig = SignMessage(
        identity.privateKey.data(), apCertOut.data() + 0x80, apCertOut.size() - 0x80);
    std::copy(apCertSig.begin(), apCertSig.end(), apCertOut.begin() + 0x04);

    sigOut = SignMessage(apPrivate.data(), data, dataSize);
}

inline void Sign(uint64_t titleId, const uint8_t* data, size_t dataSize,
                 EcSignature& sigOut, EccCert& apCertOut) {
    Sign(titleId, data, dataSize, CurrentIdentity(), sigOut, apCertOut);
}


} // namespace WiiEsCrypto
