#include <catch2/catch_test_macros.hpp>

#include <tm_core/ppc_stack.h>

#include <cstdint>
#include <vector>

using opentm::tm_core::walk_stack;

namespace {

// a block of stack with frames linked the way PPC64 lays them out
struct fake_stack {
    std::uint64_t base = 0xd0100000;
    std::vector<std::byte> bytes;

    explicit fake_stack(std::size_t size) : bytes(size, std::byte{0}) {}

    void put(std::uint64_t address, std::uint64_t value) {
        auto offset = static_cast<std::size_t>(address - base);
        for (int i = 7; i >= 0; --i) {
            bytes[offset + static_cast<std::size_t>(7 - i)] =
                std::byte{static_cast<std::uint8_t>((value >> (i * 8)) & 0xff)};
        }
    }

    // link `frame` to `caller`, with the return address into the caller
    void frame(std::uint64_t frame_sp, std::uint64_t caller_sp, std::uint64_t return_to) {
        put(frame_sp, caller_sp);
        put(caller_sp + 16, return_to);
    }

    std::span<const std::byte> view() const { return {bytes.data(), bytes.size()}; }
};

} // namespace

TEST_CASE("a call stack is the back chain plus the saved LRs", "[stack]") {
    fake_stack s(0x400);
    // three frames: 0xd0100000 -> 0xd0100100 -> 0xd0100200
    s.frame(0xd0100000, 0xd0100100, 0x3b094);
    s.frame(0xd0100100, 0xd0100200, 0x390f4);
    s.put(0xd0100200, 0);   // chain ends

    const auto frames = walk_stack(s.view(), s.base, 0x48530, 0xd0100000);

    REQUIRE(frames.size() == 3);
    CHECK(frames[0].address == 0x48530);    // the pc we stopped on
    CHECK(frames[1].address == 0x3b094);
    CHECK(frames[2].address == 0x390f4);
    CHECK(frames[0].frame == 0xd0100000);
    CHECK(frames[1].frame == 0xd0100100);
}

TEST_CASE("a walk stops rather than running off the end", "[stack]") {
    SECTION("a chain that does not climb is not a chain") {
        fake_stack s(0x400);
        s.put(0xd0100000, 0xd0100000);   // points at itself
        const auto frames = walk_stack(s.view(), s.base, 0x48530, 0xd0100000);
        CHECK(frames.size() == 1);
    }

    SECTION("a chain pointing outside the block we read") {
        fake_stack s(0x400);
        s.put(0xd0100000, 0xd0900000);   // far above what we hold
        const auto frames = walk_stack(s.view(), s.base, 0x48530, 0xd0100000);
        CHECK(frames.size() == 1);
    }

    SECTION("a null return address ends it") {
        fake_stack s(0x400);
        s.frame(0xd0100000, 0xd0100100, 0);
        const auto frames = walk_stack(s.view(), s.base, 0x48530, 0xd0100000);
        CHECK(frames.size() == 1);
    }

    SECTION("a loop cannot outlast the frame limit") {
        fake_stack s(0x400);
        for (std::uint64_t at = 0xd0100000; at < 0xd01003c0; at += 0x40) {
            s.frame(at, at + 0x40, 0x1000);
        }
        const auto frames = walk_stack(s.view(), s.base, 0x48530, 0xd0100000, 4);
        CHECK(frames.size() == 4);
    }

    SECTION("an sp outside the block yields just the pc") {
        fake_stack s(0x400);
        const auto frames = walk_stack(s.view(), s.base, 0x48530, 0xd0200000);
        REQUIRE(frames.size() == 1);
        CHECK(frames[0].address == 0x48530);
    }
}
