#include <tm_core/dbgp_codec.h>

#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace opentm::tm_core;

namespace {

std::vector<std::byte> from_hex(const char* hex) {
    std::vector<std::byte> out;
    auto val = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (; *hex; ) {
        while (*hex && *hex == ' ') ++hex;
        if (!*hex) break;
        const int hi = val(*hex++);
        if (!*hex) break;
        const int lo = val(*hex++);
        if (hi < 0 || lo < 0) break;
        out.push_back(std::byte{static_cast<std::uint8_t>((hi << 4) | lo)});
    }
    return out;
}

} // namespace

TEST_CASE("DBGP: encode_request round-trips a process_list request", "[dbgp]") {
    dbgp::request req;
    req.cmd        = 0x00000102;
    req.req_id     = 0x00000018;
    req.process_id = 0;

    const auto bytes = dbgp::encode_request(req);
    REQUIRE(bytes.size() == 17); // 16B header + 0B payload + 1B trailer
    // First 16 bytes should match the captured request header.
    const auto expected_header = from_hex("00000102 00000018 00000000 00000000");
    for (std::size_t i = 0; i < 16; ++i) {
        REQUIRE(static_cast<std::uint8_t>(bytes[i]) == static_cast<std::uint8_t>(expected_header[i]));
    }
}

TEST_CASE("DBGP: decode process_list reply with one process", "[dbgp]") {
    const auto bytes = from_hex(
        "80000102 00000018 00000004 00000000 00000000"  // header
        "01000500"                                       // payload (1 PID)
        "a5");                                           // trailer
    const auto r = dbgp::decode_response(bytes);
    REQUIRE(r.has_value());
    REQUIRE(r->cmd         == 0x80000102u);
    REQUIRE(r->req_id      == 0x00000018u);
    REQUIRE(r->data_len    == 4u);
    REQUIRE(r->process_id  == 0u);
    REQUIRE(r->result_code == 0u);

    const auto pids = dbgp::parse_process_list(*r);
    REQUIRE(pids.size() == 1);
    REQUIRE(pids[0] == 0x01000500u);
}

TEST_CASE("DBGP: decode empty process_list", "[dbgp]") {
    const auto bytes = from_hex(
        "80000102 00000005 00000000 00000000 00000000 88");
    const auto r = dbgp::decode_response(bytes);
    REQUIRE(r.has_value());
    REQUIRE(r->data_len == 0u);
    REQUIRE(dbgp::parse_process_list(*r).empty());
}

TEST_CASE("DBGP: decode user_memory_stat reply", "[dbgp]") {
    const auto bytes = from_hex(
        "80200008 00000019 0000001c 01000500 00000000"  // header
        "00000000"          // shared_created
        "00010000"          // shared_attached
        "186b0000"          // local_memory   (390.7 MB)
        "00060000"          // local_text     (384 KB)
        "000c0000"          // prx_text       (768 KB)
        "00050000"          // prx_data       (320 KB)
        "003a6000"          // remain         (3.6 MB)
        "18");
    const auto r = dbgp::decode_response(bytes);
    REQUIRE(r.has_value());
    REQUIRE(r->cmd == 0x80200008u);
    const auto s = dbgp::parse_user_memory_stat(*r);
    REQUIRE(s.has_value());
    REQUIRE(s->shared_created  == 0x00000000u);
    REQUIRE(s->shared_attached == 0x00010000u);
    REQUIRE(s->local_memory    == 0x186b0000u);
    REQUIRE(s->local_text      == 0x00060000u);
    REQUIRE(s->prx_text        == 0x000c0000u);
    REQUIRE(s->prx_data        == 0x00050000u);
    REQUIRE(s->remain_memory   == 0x003a6000u);
}

TEST_CASE("DBGP: decode thread_list reply", "[dbgp]") {
    const auto bytes = from_hex(
        "80000103 0000001b 00000024 01000500 00000000"
        "00000000"                              // ppu count = 0
        "00000003"                              // spu count = 3??
        "00000001 00000000"                     // u64
        "01000086 00000000"                     // u64
        "01000085 00000000"                     // u64
        "01000083 04000100"                     // u64
        "63");
    const auto r = dbgp::decode_response(bytes);
    REQUIRE(r.has_value());
    REQUIRE(r->data_len == 36u);
    REQUIRE(r->process_id  == 0x01000500u);
    REQUIRE(r->result_code == 0u);
}

// Bytes below are lifted from pcaps/debugger/stop_program_execution.pcapng,
// ProDG stopped at a TRAP in cellmark_decr.self.
TEST_CASE("debugger primitives match the captured frames", "[dbgp]") {
    using namespace opentm::tm_core::dbgp;

    SECTION("read_memory asks for an address and a length") {
        const auto body = build_read_memory_request_body(0x10700, 0x100);
        REQUIRE(body.size() == 16);
        const std::vector<std::uint8_t> expected{
            0x00,0x00,0x00,0x00,0x00,0x01,0x07,0x00,
            0x00,0x00,0x00,0x00,0x00,0x00,0x01,0x00};
        for (std::size_t i = 0; i < expected.size(); ++i) {
            CHECK(std::to_integer<std::uint8_t>(body[i]) == expected[i]);
        }
    }

    SECTION("the reply echoes the address, then the bytes") {
        // ProDG read 0x10700 and the console answered with the prologue of
        // main() - the same instruction its disassembly pane shows there.
        response r;
        r.result_code = 0;
        for (auto b : {0x00,0x00,0x00,0x00,0x00,0x01,0x07,0x00,   // address
                       0xf8,0x21,0xfe,0x61,                        // stdu r1,-0x1a0(r1)
                       0xfb,0xe1,0x01,0x98}) {
            r.payload.push_back(std::byte{static_cast<std::uint8_t>(b)});
        }
        const auto block = parse_read_memory(r);
        REQUIRE(block.has_value());
        CHECK(block->address == 0x10700);
        REQUIRE(block->data.size() == 8);
        CHECK(std::to_integer<std::uint8_t>(block->data[0]) == 0xf8);
        CHECK(std::to_integer<std::uint8_t>(block->data[3]) == 0x61);
    }

    SECTION("read_ppu_registers is 22 bytes, with a u16 selector in the middle") {
        const auto body = build_read_ppu_registers_request_body(0x010000c3);
        REQUIRE(body.size() == 22);
        const std::vector<std::uint8_t> expected{
            0x00,0x00,0x00,0x00,0x01,0x00,0x00,0xc3,   // thread id
            0xff,0xff,0xff,0xff,                        // gpr select
            0xff,0xff,0xff,0xff,                        // fpr select
            0x00,0xff,                                  // spr select, u16
            0xff,0xff,0xff,0xff};                       // vmx select
        for (std::size_t i = 0; i < expected.size(); ++i) {
            CHECK(std::to_integer<std::uint8_t>(body[i]) == expected[i]);
        }
    }

    SECTION("registers land after the echoed selectors") {
        response r;
        std::vector<std::uint8_t> raw{
            0x00,0x00,0x00,0x00,0x01,0x00,0x00,0xc3,   // thread id
            0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,   // 28 bytes of echoed
            0xff,0xff,0xff,0xff,0x00,0xff,0x00,0x7f,   // selectors
            0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
            0x00,0x00,0x00,0x00};
        raw.resize(ppu_registers_prologue, 0);
        // r0 = 0, r1 = the stack pointer the console reported
        for (auto b : {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
                       0x00,0x00,0x00,0x00,0xd0,0x10,0x08,0xb0,
                       0x00,0x00,0x00,0x00,0x00,0x07,0xb0,0xc0}) {
            raw.push_back(static_cast<std::uint8_t>(b));
        }
        raw.resize(ppu_registers_prologue + 32 * 8, 0);
        for (auto b : raw) r.payload.push_back(std::byte{b});

        const auto regs = parse_ppu_registers(r);
        REQUIRE(regs.has_value());
        CHECK(regs->thread_id == 0x010000c3);
        CHECK(regs->gpr[0] == 0);
        CHECK(regs->gpr[1] == 0xd01008b0);   // stack
        CHECK(regs->gpr[2] == 0x0007b0c0);   // TOC
    }

    SECTION("a thread id list is just the ids") {
        const std::uint64_t ids[] = {0x010000c3, 0x010000c5, 0x010000c6};
        const auto body = build_thread_id_list_body(ids);
        REQUIRE(body.size() == 24);
        CHECK(std::to_integer<std::uint8_t>(body[7])  == 0xc3);
        CHECK(std::to_integer<std::uint8_t>(body[15]) == 0xc5);
        CHECK(std::to_integer<std::uint8_t>(body[23]) == 0xc6);
    }
}

TEST_CASE("breakpoints, stepping and the stop event", "[dbgp]") {
    using namespace opentm::tm_core::dbgp;

    SECTION("setting a breakpoint sends the bare address") {
        const auto body = build_breakpoint_body(0x0002e118);
        REQUIRE(body.size() == 8);
        const std::vector<std::uint8_t> expected{
            0x00,0x00,0x00,0x00,0x00,0x02,0xe1,0x18};
        for (std::size_t i = 0; i < expected.size(); ++i) {
            CHECK(std::to_integer<std::uint8_t>(body[i]) == expected[i]);
        }
    }

    SECTION("the reply echoes the address back") {
        response r;
        r.result_code = 0;
        for (auto b : {0x00,0x00,0x00,0x00,0x00,0x02,0xe1,0x18}) {
            r.payload.push_back(std::byte{static_cast<std::uint8_t>(b)});
        }
        const auto addr = parse_breakpoint_reply(r);
        REQUIRE(addr.has_value());
        CHECK(*addr == 0x0002e118);
    }

    SECTION("a step is an address and the thread it applies to") {
        const auto body = build_step_body(0x000106fc, 0x010000df);
        REQUIRE(body.size() == 16);
        const std::vector<std::uint8_t> expected{
            0x00,0x00,0x00,0x00,0x00,0x01,0x06,0xfc,
            0x00,0x00,0x00,0x00,0x01,0x00,0x00,0xdf};
        for (std::size_t i = 0; i < expected.size(); ++i) {
            CHECK(std::to_integer<std::uint8_t>(body[i]) == expected[i]);
        }
    }

    SECTION("the stop event names the thread, the address and the stack") {
        response r;
        r.result_code = 0;
        for (auto b : {0x00,0x00,0x00,0x10, // reason
                       0x00,0x00,0x00,0x00,0x01,0x00,0x00,0xb4, // thread id
                       0x00,0x00,0x00,0x00,
                       0x00,0x00,0x00,0x00,0x00,0x03,0x91,0x10, // address
                       0x00,0x00,0x00,0x00,0xd0,0x10,0x09,0xb0}) { // stack
            r.payload.push_back(std::byte{static_cast<std::uint8_t>(b)});
        }
        const auto ev = parse_stop_event(r);
        REQUIRE(ev.has_value());
        CHECK(ev->reason == stop_reason_breakpoint);
        CHECK(ev->thread_id == 0x010000b4);
        CHECK(ev->address == 0x00039110);
        CHECK(ev->stack_pointer == 0xd01009b0);
    }

    SECTION("pc, cr, lr and ctr follow the FPRs") {
        response r;
        std::vector<std::uint8_t> raw;
        raw.resize(ppu_special_offset, 0);
        for (auto b : {0x00,0x00,0x00,0x00,0x00,0x01,0x06,0xfc,//pc
                       0x22,0x00,0x00,0x22, //cr
                       0x00,0x00,0x00,0x00, //hole
                       0x00,0x00,0x00,0x00,0x00,0x01,0x04,0xd8,// lr
                       0x00,0x00,0x00,0x00,0x00,0x19,0x0e,0x7c}) { // ctr
            raw.push_back(static_cast<std::uint8_t>(b));
        }
        for (auto b : raw) r.payload.push_back(std::byte{b});

        const auto regs = parse_ppu_registers(r);
        REQUIRE(regs.has_value());
        CHECK(regs->pc  == 0x000106fc);
        CHECK(regs->cr  == 0x22000022);
        CHECK(regs->lr  == 0x000104d8);
        CHECK(regs->ctr == 0x00190e7c);
    }

    SECTION("a short reply still yields the GPRs it did carry") {
        response r;
        r.payload.resize(ppu_registers_prologue + 32 * 8, std::byte{0});
        const auto regs = parse_ppu_registers(r);
        REQUIRE(regs.has_value());
        CHECK(regs->pc == 0);
    }
}

TEST_CASE("writing memory and registers", "[dbgp]") {
    using namespace opentm::tm_core::dbgp;

    SECTION("write_memory is the address then the bytes, with no count") {
        const std::byte data[] = {std::byte{0xff}};
        const auto body = build_write_memory_body(0x13337, data);
        REQUIRE(body.size() == 9);
        const std::vector<std::uint8_t> expected{
            0x00,0x00,0x00,0x00,0x00,0x01,0x33,0x37,
            0xff};
        for (std::size_t i = 0; i < expected.size(); ++i) {
            CHECK(std::to_integer<std::uint8_t>(body[i]) == expected[i]);
        }
    }

    SECTION("a longer write just carries more bytes") {
        const std::byte data[] = {std::byte{0xde}, std::byte{0xad}, std::byte{0xbe}, std::byte{0xef}};
        const auto body = build_write_memory_body(0x10700, data);
        CHECK(body.size() == 12);
        CHECK(std::to_integer<std::uint8_t>(body[8])  == 0xde);
        CHECK(std::to_integer<std::uint8_t>(body[11]) == 0xef);
    }

    SECTION("writing r3 selects bit 3 and nothing else") {
        const auto body = build_write_ppu_gpr_body(0x010000b4, 3, 0xffffffffffffffffull);
        REQUIRE(body.size() == 30);
        const std::vector<std::uint8_t> expected{
            0x00,0x00,0x00,0x00,0x01,0x00,0x00,0xb4, // thread id
            0x00,0x00,0x00,0x08, // gpr select, bit 3
            0x00,0x00,0x00,0x00, // fpr select
            0x00,0x00,// spr select, u16
            0x00,0x00,0x00,0x00,// vmx select
            0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff};// the valuee
        for (std::size_t i = 0; i < expected.size(); ++i) {
            CHECK(std::to_integer<std::uint8_t>(body[i]) == expected[i]);
        }
    }

    SECTION("selecting several registers appends one value each") {
        auto select = ppu_register_select::none();
        select.gpr = 0b1100;   // r2 and r3
        const std::uint64_t values[] = {0x1111, 0x2222};
        const auto body = build_write_ppu_registers_body(0x010000b4, select, values);
        CHECK(body.size() == 22 + 16);
    }

    SECTION("the echo reports what the agent actually filled in") {
        // the console accepts seven of the eight special registers asked for
        response r;
        std::vector<std::uint8_t> raw{
            0x00,0x00,0x00,0x00,0x01,0x00,0x00,0xb4, // thread id
            0xff,0xff,0xff,0xff, // gpr requested
            0xff,0xff,0xff,0xff, // gpr accepted
            0xff,0xff,0xff,0xff, // fpr requested
            0xff,0xff,0xff,0xff, // fpr accepted
            0x00,0xff,0x00,0x7f, // spr requested, then accepted
            0xff,0xff,0xff,0xff, // vmx requested
            0xff,0xff,0xff,0xff};// vmx accepted
        for (auto b : raw) r.payload.push_back(std::byte{b});

        const auto ack = parse_ppu_register_ack(r);
        REQUIRE(ack.has_value());
        CHECK(ack->gpr_requested == 0xffffffff);
        CHECK(ack->gpr_accepted  == 0xffffffff);
        CHECK(ack->spr_requested == 0x00ff);
        CHECK(ack->spr_accepted  == 0x007f);
        CHECK(ack->vmx_accepted  == 0xffffffff);
    }
}
