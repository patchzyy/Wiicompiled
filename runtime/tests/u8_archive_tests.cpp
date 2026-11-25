#include "u8_archive.h"

#include <algorithm>
#include <iostream>
#include <iterator>
#include <stdexcept>

namespace {
void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void Word(std::vector<uint8_t>& data, size_t offset, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) data.at(offset + i) = static_cast<uint8_t>(value >> ((3 - i) * 8));
}
void Node(std::vector<uint8_t>& data, unsigned index, uint32_t typeAndName, uint32_t offset, uint32_t size) {
    const size_t at = 0x20 + index * 12;
    Word(data, at, typeAndName); Word(data, at + 4, offset); Word(data, at + 8, size);
}
std::vector<uint8_t> Archive() {
    std::vector<uint8_t> data(0x110);
    Word(data, 0, 0x55AA382D); Word(data, 4, 0x20); Word(data, 8, 6 * 12 + 30); Word(data, 12, 0x100);
    Node(data, 0, 0x01000000, 0, 6);
    Node(data, 1, 0x01000001, 0, 5);
    Node(data, 2, 0x01000005, 1, 4);
    Node(data, 3, 9, 0x100, 4);
    Node(data, 4, 0x01000015, 1, 5);
    Node(data, 5, 27, 0x110, 0);
    const char names[] = "\0dir\0sub\0RFL_Res.dat\0empty\0ok";
    std::copy(std::begin(names), std::end(names), data.begin() + 0x20 + 6 * 12);
    data[0x100] = 1; data[0x101] = 2; data[0x102] = 3; data[0x103] = 4;
    return data;
}
void Reject(const std::vector<uint8_t>& data, const char* message) {
    std::vector<uint8_t> out{99};
    Require(!RuntimeU8::Extract(data, "RFL_Res.dat", out) && out == std::vector<uint8_t>{99}, message);
}
} // namespace

int main() {
    try {
        const auto valid = Archive();
        std::vector<uint8_t> out;
        Require(RuntimeU8::Extract(valid, "dir/sub/RFL_Res.dat", out) && out == std::vector<uint8_t>({1,2,3,4}), "nested full-path extraction failed");
        Require(RuntimeU8::Extract(valid, "RFL_Res.dat", out), "basename extraction failed");
        Require(RuntimeU8::Extract(valid, "ok", out) && out.empty(), "zero-length EOF payload was rejected");
        Require(!RuntimeU8::Extract(valid, "missing", out), "missing target was found");
        auto data = valid; Word(data, 4, 0xfffffff8); Reject(data, "wrapped root was accepted");
        data = valid; Word(data, 4, static_cast<uint32_t>(data.size() - 11)); Reject(data, "truncated root was accepted");
        data = valid; Word(data, 8, 0xffffffff); Reject(data, "wrapped metadata was accepted");
        data = valid; Word(data, 8, 6 * 12 - 1); Reject(data, "truncated node table was accepted");
        data = valid; Word(data, 0x28, 0xffffffff); Reject(data, "huge node count was accepted");
        data = valid; Word(data, 0x28, 0); Reject(data, "zero node count was accepted");
        data = valid; Word(data, 0x20, 0); Reject(data, "file root was accepted");
        data = valid; Word(data, 0x24, 1); Reject(data, "invalid root parent was accepted");
        data = valid; Word(data, 0x20 + 3 * 12, 0x00ffffff); Reject(data, "out-of-range name offset was accepted");
        data = valid; std::fill(data.begin() + 0x20 + 6 * 12 + 9, data.begin() + 0x20 + 6 * 12 + 30, 'x');
        Reject(data, "name terminated only in padding was accepted");
        data = valid; Word(data, 0x20 + 2 * 12 + 8, 6); Reject(data, "child directory escaped its parent");
        data = valid; Word(data, 0x20 + 2 * 12 + 8, 2); Reject(data, "backward directory range was accepted");
        data = valid; Word(data, 0x20 + 2 * 12 + 4, 0); Reject(data, "wrong nested parent was accepted");
        data = valid; Word(data, 0x20 + 5 * 12, 0x0200001b); Reject(data, "unknown node type was accepted");
        data = valid; Word(data, 0x20 + 3 * 12 + 4, 0xfffffffe); Word(data, 0x20 + 3 * 12 + 8, 4);
        Reject(data, "wrapped matching payload was accepted");
        data = valid; Word(data, 0x20 + 5 * 12 + 4, 0xffffffff); Reject(data, "out-of-range empty payload was accepted");
        data = valid; Word(data, 0x20 + 5 * 12 + 4, 0x110); Word(data, 0x20 + 5 * 12 + 8, 1);
        Reject(data, "invalid later unselected payload was ignored");
        data = valid; Word(data, 0x20 + 5 * 12, 0x00ffffff); Reject(data, "invalid later name was ignored");
        data = valid; Word(data, 12, 0xffffffff);
        Require(RuntimeU8::Extract(data, "RFL_Res.dat", out), "unused header data offset constrained absolute file offsets");
        data = valid; Word(data, 0x20 + 3 * 12 + 4, 0x103); Word(data, 0x20 + 3 * 12 + 8, 1);
        Require(RuntimeU8::Extract(data, "RFL_Res.dat", out) && out == std::vector<uint8_t>{4}, "unaligned payload was rejected");
        for (size_t size = 0; size < 0x20; ++size) Reject(std::vector<uint8_t>(size), "short header was accepted");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
