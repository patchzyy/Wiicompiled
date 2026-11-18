#include "ax_mix_kernels.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(MKW_AX_MIX_REQUIRE_NEON) && !MKW_AX_MIX_NEON
#error The ARM kernel fixture must select NEON
#endif

namespace {

struct Backend {
    const char* name;
    decltype(&AxMixKernels::MixAddRampScalar) mix;
    decltype(&AxMixKernels::ScaleRampScalar) scale;
    decltype(&AxMixKernels::MixAccumRamp32Scalar) accum;
    decltype(&AxMixKernels::StoreBigEndian32Scalar) store;
    decltype(&AxMixKernels::LoadBigEndian32Scalar) load;
};

constexpr Backend kScalar = {"scalar", AxMixKernels::MixAddRampScalar,
    AxMixKernels::ScaleRampScalar, AxMixKernels::MixAccumRamp32Scalar,
    AxMixKernels::StoreBigEndian32Scalar, AxMixKernels::LoadBigEndian32Scalar};
#if MKW_AX_MIX_NEON
constexpr const char* kSelectedName = "NEON";
#elif MKW_AX_MIX_AVX2
constexpr const char* kSelectedName = "AVX2";
#else
constexpr const char* kSelectedName = "selected scalar";
#endif
constexpr Backend kSelected = {kSelectedName, AxMixKernels::MixAddRamp,
    AxMixKernels::ScaleRamp, AxMixKernels::MixAccumRamp32,
    AxMixKernels::StoreBigEndian32, AxMixKernels::LoadBigEndian32};

uint32_t g_random = 0x12345678u;

char g_case[192]{};

uint32_t NextRandom() {
    g_random ^= g_random << 13;
    g_random ^= g_random >> 17;
    g_random ^= g_random << 5;
    return g_random;
}

void Require(bool condition, const char* message) {
    if (condition) return;
    if (g_case[0]) std::fprintf(stderr, "%s: ", g_case);
    throw std::runtime_error(message);
}

int32_t Wrap32(int64_t value) {
    const uint32_t bits = static_cast<uint32_t>(value);
    return bits <= 0x7fffffffu ? static_cast<int32_t>(bits)
        : static_cast<int32_t>(static_cast<int64_t>(bits) - 0x100000000ll);
}

int64_t FloorQ15(int64_t product) {
    return product >= 0 ? product / 32768 : -((-product + 32767) / 32768);
}

int16_t ScaleSample(int16_t sample, uint16_t volume) {
    const int64_t scaled = FloorQ15(static_cast<int64_t>(sample) * volume);
    return static_cast<int16_t>(std::clamp<int64_t>(scaled, -32768, 32767));
}

constexpr int16_t kSamples[] = {-32768, 32767, -32767, 16384, -16385, 1, -1, 0};
constexpr int32_t kWords[] = {std::numeric_limits<int32_t>::min(),
    std::numeric_limits<int32_t>::max(), -2147483647, 2147450880,
    -65537, 65537, -1, 0, 1};
constexpr uint16_t kGains[] = {0, 1, 0x3fff, 0x4000, 0x7fff, 0x8000, 0xfffe, 0xffff};

std::vector<uint32_t> Counts() {
    std::vector<uint32_t> counts;
    for (uint32_t count = 0; count <= 40; ++count) counts.push_back(count);
    for (uint32_t count : {63u, 64u, 65u, 95u, 96u, 97u, 159u, 160u, 161u}) {
        counts.push_back(count);
    }
    return counts;
}

void TestRamps(const Backend& backend, uint32_t count, size_t offset,
               uint16_t volume, uint16_t delta, unsigned pattern) {
    std::snprintf(g_case, sizeof(g_case), "%s count=%u offset=%zu volume=%u delta=%u pattern=%u seed=%u",
                  backend.name, count, offset, unsigned(volume), unsigned(delta), pattern, g_random);
    // Source ends at the allocation boundary so ASan also catches tail overreads.
    std::vector<int16_t> input(offset + count, 12345);
    std::vector<int32_t> initial(offset + count + 1, 0x13579bdf);
    for (uint32_t i = 0; i < count; ++i) {
        input[offset + i] = pattern == 0 ? kSamples[i % std::size(kSamples)]
            : static_cast<int16_t>(static_cast<int32_t>(NextRandom() & 0xffffu) - 32768);
        initial[offset + i] = pattern == 0 ? kWords[i % std::size(kWords)]
            : Wrap32(NextRandom());
    }
    const auto original = input;
    auto expectedMix = initial;
    auto expectedScale = input;
    int16_t expectedDpop = -23456;
    uint16_t expectedVolume = volume;
    for (uint32_t i = 0; i < count; ++i) {
        const int16_t scaled = ScaleSample(input[offset + i], expectedVolume);
        expectedMix[offset + i] = Wrap32(static_cast<int64_t>(initial[offset + i]) + scaled);
        expectedScale[offset + i] = scaled;
        expectedDpop = scaled;
        expectedVolume = static_cast<uint16_t>(expectedVolume + delta);
    }
    auto mixed = initial;
    int16_t dpop = -23456;
    Require(backend.mix(mixed.data() + offset, input.data() + offset, count,
                        volume, delta, dpop) == expectedVolume, "mix final volume");
    Require(mixed == expectedMix, "mix output or sentinel");
    Require(dpop == expectedDpop, "mix de-pop sample");
    Require(input == original, "mix modified its source");

    auto scaled = input;
    Require(backend.scale(scaled.data() + offset, count, volume, delta) == expectedVolume,
            "scale final volume");
    Require(scaled == expectedScale, "scale output or prefix");
}

void TestAccum(const Backend& backend, uint32_t count, size_t offset, unsigned pattern) {
    std::snprintf(g_case, sizeof(g_case), "%s count=%u offset=%zu pattern=%u seed=%u",
                  backend.name, count, offset, pattern, g_random);
    std::vector<int32_t> source(offset + count, 0x2468ace);
    std::vector<uint16_t> ramp(offset + count, 12345);
    std::vector<int32_t> initial(offset + count + 1, 0x13579bdf);
    for (uint32_t i = 0; i < count; ++i) {
        source[offset + i] = pattern == 0 ? kWords[i % std::size(kWords)] : Wrap32(NextRandom());
        initial[offset + i] = pattern == 0 ? kWords[(i + 1) % std::size(kWords)]
            : Wrap32(NextRandom());
        ramp[offset + i] = pattern == 0 ? kGains[i % std::size(kGains)]
            : static_cast<uint16_t>(NextRandom());
    }
    const auto originalSource = source;
    const auto originalRamp = ramp;
    auto expected = initial;
    for (uint32_t i = 0; i < count; ++i) {
        expected[offset + i] = Wrap32(static_cast<int64_t>(initial[offset + i]) +
            FloorQ15(static_cast<int64_t>(source[offset + i]) * ramp[offset + i]));
    }
    auto actual = initial;
    backend.accum(actual.data() + offset, source.data() + offset, ramp.data() + offset, count);
    Require(actual == expected, "aux accumulation or sentinel");
    Require(source == originalSource && ramp == originalRamp, "aux modified its source");

    auto aliased = initial;
    expected = initial;
    for (uint32_t i = 0; i < count; ++i) {
        expected[offset + i] = Wrap32(static_cast<int64_t>(initial[offset + i]) +
            FloorQ15(static_cast<int64_t>(initial[offset + i]) * ramp[offset + i]));
    }
    backend.accum(aliased.data() + offset, aliased.data() + offset, ramp.data() + offset, count);
    Require(aliased == expected, "aux in-place accumulation");
}

void TestAccumEdges(const Backend& backend) {
    for (int32_t source : kWords) for (int32_t initial : kWords) for (uint16_t gain : kGains) {
        std::snprintf(g_case, sizeof(g_case), "%s source=%d initial=%d gain=%u",
                      backend.name, source, initial, unsigned(gain));
        std::array<int32_t, 17> src, dst;
        std::array<uint16_t, 17> ramp;
        src.fill(source);
        dst.fill(initial);
        ramp.fill(gain);
        const int32_t expected = Wrap32(static_cast<int64_t>(initial) +
            FloorQ15(static_cast<int64_t>(source) * gain));
        backend.accum(dst.data(), src.data(), ramp.data(), 17);
        Require(std::all_of(dst.begin(), dst.end(), [expected](int32_t v) { return v == expected; }),
                "aux full-width edge");
    }
}

void TestEndian(const Backend& backend, size_t count, size_t offset) {
    std::snprintf(g_case, sizeof(g_case), "%s count=%zu offset=%zu seed=%u",
                  backend.name, count, offset, g_random);
    std::vector<int32_t> source(offset + count, 0x13579bdf);
    for (size_t i = 0; i < count; ++i) {
        source[offset + i] = i % 2 == 0 ? kWords[(i / 2) % std::size(kWords)] : Wrap32(NextRandom());
    }
    const auto original = source;
    std::vector<uint8_t> expected(offset + count * 4 + 1, 0xa5);
    for (size_t i = 0; i < count; ++i) {
        const uint32_t word = static_cast<uint32_t>(source[offset + i]);
        for (unsigned byte = 0; byte < 4; ++byte) {
            expected[offset + i * 4 + byte] = static_cast<uint8_t>(word >> (24 - byte * 8));
        }
    }
    auto bytes = std::vector<uint8_t>(expected.size(), 0xa5);
    backend.store(bytes.data() + offset, source.data() + offset, count);
    Require(bytes == expected, "big-endian store or sentinel");
    Require(source == original, "store modified its source");

    // Drop the trailing sentinel before loading to expose overreads to ASan.
    std::vector<uint8_t> loadBytes(expected.begin(), expected.end() - 1);
    std::vector<int32_t> loaded(offset + count + 1, 0x13579bdf);
    auto expectedWords = loaded;
    std::copy(source.begin() + offset, source.end(), expectedWords.begin() + offset);
    backend.load(loaded.data() + offset, loadBytes.data() + offset, count);
    Require(loaded == expectedWords, "big-endian load or sentinel");
    Require(std::equal(loadBytes.begin(), loadBytes.end(), expected.begin()), "load modified source");
}

void TestBackend(const Backend& backend) {
    for (uint32_t count : Counts()) for (size_t offset : {1u, 2u, 3u}) {
        for (uint16_t volume : kGains) for (uint16_t delta : kGains) {
            TestRamps(backend, count, offset, volume, delta, 0);
        }
        for (unsigned pattern = 0; pattern < 8; ++pattern) TestAccum(backend, count, offset, pattern);
    }
    for (unsigned trial = 0; trial < 512; ++trial) {
        TestRamps(backend, NextRandom() % 162, 1 + NextRandom() % 3,
                  static_cast<uint16_t>(NextRandom()), static_cast<uint16_t>(NextRandom()), 1);
    }
    TestRamps(backend, 65539, 1, 0xffff, 0xffff, 0);
    TestAccumEdges(backend);
    for (size_t count = 0; count <= 161; ++count) for (size_t offset = 1; offset <= 15; ++offset) {
        TestEndian(backend, count, offset);
    }
}

uint64_t Checksum(uint64_t hash, uint32_t value) {
    return (hash ^ value) * 1099511628211ull;
}

#if defined(_MSC_VER)
#define AX_TEST_NOINLINE __declspec(noinline)
#else
#define AX_TEST_NOINLINE __attribute__((noinline))
#endif

AX_TEST_NOINLINE uint64_t Benchmark(const Backend& backend, const std::string& kernel,
                                   uint32_t count, uint32_t iterations) {
    std::vector<int16_t> samples(count);
    std::vector<int32_t> source(count), bus(count);
    std::vector<uint16_t> ramp(count);
    std::vector<uint8_t> bytes(count * 4 + 1);
    for (uint32_t i = 0; i < count; ++i) {
        samples[i] = kSamples[i % std::size(kSamples)];
        source[i] = kWords[i % std::size(kWords)];
        bus[i] = source[i];
        ramp[i] = kGains[i % std::size(kGains)];
    }
    AxMixKernels::StoreBigEndian32Scalar(bytes.data() + 1, source.data(), count);
    uint16_t volume = 0xfffe;
    int16_t dpop = -12345;
    const auto start = std::chrono::steady_clock::now();
    if (kernel == "mix") {
        for (uint32_t i = 0; i < iterations; ++i) {
            volume = backend.mix(bus.data(), samples.data(), count, volume, 0xffff, dpop);
        }
    } else if (kernel == "scale") {
        for (uint32_t i = 0; i < iterations; ++i) {
            samples[i % count] = kSamples[i % std::size(kSamples)];
            volume = backend.scale(samples.data(), count, volume, 0xffff);
        }
    } else if (kernel == "accum") {
        for (uint32_t i = 0; i < iterations; ++i) {
            backend.accum(bus.data(), source.data(), ramp.data(), count);
        }
    } else if (kernel == "store") {
        for (uint32_t i = 0; i < iterations; ++i) {
            source[i % count] = Wrap32(i);
            backend.store(bytes.data() + 1, source.data(), count);
        }
    } else if (kernel == "load") {
        for (uint32_t i = 0; i < iterations; ++i) {
            bytes[1 + i % (count * 4)] = static_cast<uint8_t>(i);
            backend.load(bus.data(), bytes.data() + 1, count);
        }
    }
    const auto elapsed = std::chrono::duration<double, std::nano>(
        std::chrono::steady_clock::now() - start).count();
    uint64_t hash = Checksum(Checksum(14695981039346656037ull, volume), static_cast<uint16_t>(dpop));
    for (int32_t value : bus) hash = Checksum(hash, static_cast<uint32_t>(value));
    for (int16_t value : samples) hash = Checksum(hash, static_cast<uint16_t>(value));
    for (uint8_t value : bytes) hash = Checksum(hash, value);
    std::printf("%s,%s,%u,%u,%.3f,%.3f,%016llx\n", backend.name, kernel.c_str(), count,
        iterations, elapsed / iterations, elapsed / (static_cast<double>(iterations) * count),
        static_cast<unsigned long long>(hash));
    return hash;
}

void RunBenchmarks(uint32_t iterations) {
    std::puts("backend,kernel,samples,iterations,ns_per_block,ns_per_sample,checksum");
    for (uint32_t count : {6u, 18u, 32u, 96u, 160u}) {
        for (const char* kernel : {"mix", "scale", "accum", "store", "load"}) {
            const uint64_t scalar = Benchmark(kScalar, kernel, count, iterations);
            const uint64_t selected = Benchmark(kSelected, kernel, count, iterations);
            Require(scalar == selected, "benchmark checksum differs");
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc >= 2 && std::string(argv[1]) == "--benchmark") {
            uint32_t iterations = 200000;
            if (argc == 3) {
                const std::string argument = argv[2];
                const auto result = std::from_chars(argument.data(), argument.data() + argument.size(), iterations);
                Require(result.ec == std::errc{} && result.ptr == argument.data() + argument.size() &&
                        iterations > 0 && iterations <= 100000000, "invalid benchmark iterations");
            }
            Require(argc <= 3, "usage: ax_mix_kernels_tests [--benchmark [iterations]]");
            RunBenchmarks(iterations);
        } else {
            Require(argc == 1, "usage: ax_mix_kernels_tests [--benchmark [iterations]]");
            g_random = 0x12345678u;
            TestBackend(kScalar);
            g_random = 0x12345678u;
            TestBackend(kSelected);
            std::printf("AX mix kernels (%s): passed\n", kSelected.name);
        }
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
