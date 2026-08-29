#include <catch2/catch_test_macros.hpp>

#include <tm_core/dbgshl_cmd.h>
#include <tm_session/debug_controller.h>
#include <tm_session/session_controller.h>

#include <QByteArray>
#include <QList>

#include <cstdint>

using opentm::tm_ui::debug_controller;
using opentm::tm_ui::session_controller;

namespace {

opentm::tm_core::deci3_frame dbgp_reply(std::uint32_t cmd, std::uint32_t req_id, const std::vector<std::uint8_t>& payload, std::uint32_t session_b = 0x02100000)
{
    auto be32 = [](std::vector<std::uint8_t>& v, std::uint32_t x) {
        v.push_back(static_cast<std::uint8_t>(x >> 24));
        v.push_back(static_cast<std::uint8_t>(x >> 16));
        v.push_back(static_cast<std::uint8_t>(x >> 8));
        v.push_back(static_cast<std::uint8_t>(x));
    };
    std::vector<std::uint8_t> raw;
    be32(raw, cmd);
    be32(raw, req_id);
    be32(raw, static_cast<std::uint32_t>(payload.size()));
    be32(raw, 0x01010200); // proc id
    be32(raw, 0); // result
    raw.insert(raw.end(), payload.begin(), payload.end());
    raw.push_back(0); // csum, not verified on recv

    opentm::tm_core::deci3_frame f;
    f.direction = opentm::tm_core::deci3_direction::target_to_host;
    f.category  = 0x0200;
    f.session_b = session_b;
    for (auto b : raw) f.payload.push_back(std::byte{b});
    return f;
}

//add_then_resume_and_hit_bp.pcapng
std::vector<std::uint8_t> stop_body() {
    return {0x00,0x00,0x00,0x10, // reason
            0x00,0x00,0x00,0x00,0x01,0x00,0x00,0xb4,   // thread id
            0x00,0x00,0x00,0x00,
            0x00,0x00,0x00,0x00,0x00,0x03,0x91,0x10,   // addy
            0x00,0x00,0x00,0x00,0xd0,0x10,0x09,0xb0}; // sp
}

} // namespace

TEST_CASE("a memory window becomes aligned chunk reads", "[debug_controller]") {
    // the kit is only ever asked for 0x100 bytes on a 0x100 boundary
    CHECK(debug_controller::chunk_base(0x13337) == 0x13300);
    CHECK(debug_controller::chunk_base(0x13300) == 0x13300);
    CHECK(debug_controller::chunk_base(0x133ff) == 0x13300);

    SECTION("a window inside one chunk is one read") {
        CHECK(debug_controller::chunk_count(0x13300, 0x100) == 1);
        CHECK(debug_controller::chunk_count(0x13310, 0x10) == 1);
    }

    SECTION("an unaligned window spills into the next chunk") {
        // pcap shows viewing 0x13337 reads two chunks
        CHECK(debug_controller::chunk_count(0x13337, 0x100) == 2);
        CHECK(debug_controller::chunk_count(0x133ff, 2) == 2);
    }

    SECTION("an aligned window ending on a boundary does not overrun") {
        CHECK(debug_controller::chunk_count(0x13300, 0x200) == 2);
        CHECK(debug_controller::chunk_count(0x13300, 0x201) == 3);
    }

    SECTION("an empty window reads nothing") {
        CHECK(debug_controller::chunk_count(0x13337, 0) == 0);
    }
}

TEST_CASE("the stop event arrives without a request to match it", "[debug_controller]") {
    session_controller session(nullptr);
    debug_controller   dbg(nullptr, &session);

    quint64 got_thread = 0, got_address = 0;
    quint32 got_reason = 0;
    int     stops = 0;
    QObject::connect(&dbg, &debug_controller::thread_stopped, [&](quint64 t, quint64 a, quint32 r) { got_thread = t; got_address = a; got_reason = r; ++stops;});

    dbg.on_frame_received(dbgp_reply(opentm::tm_core::dbgshl::cmd::stop_event, 0, stop_body()));

    REQUIRE(stops == 1);
    CHECK(got_thread  == 0x010000b4);
    CHECK(got_address == 0x00039110);
    CHECK(got_reason  == opentm::tm_core::dbgp::stop_reason_breakpoint);
    CHECK_FALSE(dbg.is_running());
}

TEST_CASE("frames that are not ours are left alone", "[debug_controller]") {
    session_controller session(nullptr);
    debug_controller   dbg(nullptr, &session);

    int stops = 0;
    QObject::connect(&dbg, &debug_controller::thread_stopped, [&](quint64, quint64, quint32) { ++stops; });

    SECTION("another category") {
        auto f = dbgp_reply(opentm::tm_core::dbgshl::cmd::stop_event, 0, stop_body());
        f.category = 0x0110;   // DRFP
        dbg.on_frame_received(f);
        CHECK(stops == 0);
    }

    SECTION("another envelope") {
        auto f = dbgp_reply(opentm::tm_core::dbgshl::cmd::stop_event, 0, stop_body(), 0x02000000);
        dbg.on_frame_received(f);
        CHECK(stops == 0);
    }

    SECTION("a request rather than a reply") {
        auto f = dbgp_reply(0x00000b00, 0, stop_body());
        dbg.on_frame_received(f);
        CHECK(stops == 0);
    }

    SECTION("a reply to a request we never sent") {
        dbg.on_frame_received(dbgp_reply(opentm::tm_core::dbgshl::cmd::read_memory, 0x1234, {}));
        CHECK(stops == 0);
    }
}

TEST_CASE("breakpoints are only remembered once the kit confirms", "[debug_controller]") {
    session_controller session(nullptr);
    debug_controller   dbg(nullptr, &session);

    // with no connection nothing can be sent, so nothing is ever confirmed
    dbg.set_breakpoint(0x39110);
    CHECK_FALSE(dbg.has_breakpoint(0x39110));
    CHECK(dbg.breakpoints().isEmpty());
}
