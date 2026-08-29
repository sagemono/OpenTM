#include <catch2/catch_test_macros.hpp>

#include <tm_core/ppc_disasm.h>

#include <cstdint>
#include <vector>

using opentm::tm_core::ppc_disassembler;

namespace {

std::vector<std::byte> words(std::initializer_list<std::uint32_t> ws) {
    std::vector<std::byte> out;
    for (auto w : ws) {
        out.push_back(std::byte{static_cast<std::uint8_t>(w >> 24)});
        out.push_back(std::byte{static_cast<std::uint8_t>(w >> 16)});
        out.push_back(std::byte{static_cast<std::uint8_t>(w >> 8)});
        out.push_back(std::byte{static_cast<std::uint8_t>(w)});
    }
    return out;
}

} // namespace

TEST_CASE("PPU code disassembles as 64bit big endian", "[disasm]") {
    ppc_disassembler d;
    REQUIRE(d.is_open());

    SECTION("the prologue the kit returned for main()") {
        // read_memory at 0x10700 answered with these bytes in the captures
        const auto code = words({0xf821fe61});
        const auto one  = d.disassemble_one(code, 0x10700);
        REQUIRE(one.has_value());
        CHECK(one->valid);
        CHECK(one->address == 0x10700);
        CHECK(one->opcode  == 0xf821fe61);
        CHECK(one->mnemonic == "stdu");
        CHECK_FALSE(one->is_branch);
    }

    SECTION("a call names its target and is flagged for stepover") {
        const auto code = words({0x48000005});   // bl .+4
        const auto one  = d.disassemble_one(code, 0x10000);
        REQUIRE(one.has_value());
        CHECK(one->is_call);
        CHECK(one->is_branch);
        REQUIRE(one->branch_target.has_value());
        CHECK(*one->branch_target == 0x10004);
    }

    SECTION("a return is recognised") {
        const auto code = words({0x4e800020});   // blr
        const auto one  = d.disassemble_one(code, 0x10000);
        REQUIRE(one.has_value());
        CHECK(one->is_return);
        // nothing in the instruction says where it goes
        CHECK_FALSE(one->branch_target.has_value());
    }
}

TEST_CASE("a word that will not decode costs four bytes, not the buffer", "[disasm]") {
    ppc_disassembler d;
    REQUIRE(d.is_open());

    // a bad word between two good ones must not swallow what follows ( ͡° ͜ʖ ͡°)
    const auto code = words({0x38600001, 0x00000000, 0x4e800020});
    const auto all  = d.disassemble(code, 0x20000);

    REQUIRE(all.size() == 3);
    CHECK(all[0].valid);
    CHECK(all[0].address == 0x20000);
    CHECK(all[1].address == 0x20004);
    CHECK(all[2].address == 0x20008);
    CHECK(all[2].valid);
    CHECK(all[2].is_return);
}

TEST_CASE("branch forms are told apart by their encoding", "[disasm]") {
    ppc_disassembler d;
    REQUIRE(d.is_open());

    struct row { std::uint32_t word; bool branch, call, ret; const char* what; };
    const row rows[] = {
        {0x48000005, true,  true,  false, "bl, a relative call"},
        {0x48000004, true,  false, false, "b, a plain relative branch"},
        {0x4e800020, true,  false, true,  "blr, the ordinary return"},
        {0x4e800021, true,  true,  false, "blrl, which links"},
        {0x4e800420, true,  false, false, "bctr, through the count register"},
        {0x4e800421, true,  true,  false, "bctrl, an indirect call"},
        {0x41820010, true,  false, false, "beq, a conditional branch"},
        {0xf821fe61, false, false, false, "stdu, not a branch at all"},
        {0x38600001, false, false, false, "li, not a branch at all"},
    };

    for (const auto& r : rows) {
        CAPTURE(r.what);
        const auto code = words({r.word});
        const auto one  = d.disassemble_one(code, 0x10000);
        REQUIRE(one.has_value());
        CHECK(one->is_branch == r.branch);
        CHECK(one->is_call   == r.call);
        CHECK(one->is_return == r.ret);
    }
}
