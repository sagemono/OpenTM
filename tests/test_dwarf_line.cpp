#include <catch2/catch_test_macros.hpp>

#include <tm_core/dwarf_line.h>
#include <tm_core/elf_image.h>

#include <cstdint>
#include <vector>
#include <cstdio>
#include <string>

using opentm::tm_core::elf_image;
using opentm::tm_core::line_table;

namespace {

struct line_unit {
    std::vector<std::uint8_t> prog;

    void u8(std::uint8_t v) { prog.push_back(v); }
    void uleb(std::uint64_t v) {
        do { auto b = static_cast<std::uint8_t>(v & 0x7f); v >>= 7; if (v) b |= 0x80; prog.push_back(b); }
        while (v);
    }
    void sleb(std::int64_t v) {
        bool more = true;
        while (more) {
            auto b = static_cast<std::uint8_t>(v & 0x7f);
            v >>= 7;
            if ((v == 0 && !(b & 0x40)) || (v == -1 && (b & 0x40))) more = false;
            else b |= 0x80;
            prog.push_back(b);
        }
    }
    void set_address(std::uint64_t a) {
        u8(0); uleb(9); u8(2);// DW_LNE_set_address
        for (int i = 7; i >= 0; --i) u8(static_cast<std::uint8_t>((a >> (i * 8)) & 0xff));
    }
    void advance_pc(std::uint64_t bytes) { u8(2); uleb(bytes / 4); }// min_insn_len = 4
    void advance_line(std::int64_t d) { u8(3); sleb(d); }
    void copy() { u8(1); }
    void end_sequence() { u8(0); uleb(1); u8(1); }

    std::vector<std::byte> build(const std::string& dir, const std::string& file) {
        std::vector<std::uint8_t> head;
        auto put = [&](std::initializer_list<std::uint8_t> b) { for (auto x : b) head.push_back(x); };
        auto str = [&](const std::string& s) { for (char c : s) head.push_back(static_cast<std::uint8_t>(c)); head.push_back(0); };

        head.push_back(4);    // min_insn_length
        head.push_back(1);    // default_is_stmt
        head.push_back(0xfb); // line_base = -5
        head.push_back(14);   // line_range
        head.push_back(13);   // opcode_base
        put({0,1,1,1,1,0,0,0,1,0,0,1});   // standard opcode lengths (12)
        str(dir);
        head.push_back(0);// end of directories
        str(file);
        head.push_back(1);// dir index
        head.push_back(0);// mtime
        head.push_back(0);// length
        head.push_back(0);// end of files

        std::vector<std::uint8_t> body;
        auto be16 = [&](std::uint16_t v) { body.push_back(v >> 8); body.push_back(v & 0xff); };
        auto be32 = [&](std::uint32_t v) { for (int i = 3; i >= 0; --i) body.push_back((v >> (i*8)) & 0xff); };

        be16(2);// version
        be32(static_cast<std::uint32_t>(head.size()));// header_length
        body.insert(body.end(), head.begin(), head.end());
        body.insert(body.end(), prog.begin(), prog.end());

        std::vector<std::uint8_t> unit;
        for (int i = 3; i >= 0; --i) unit.push_back(((body.size()) >> (i*8)) & 0xff);
        unit.insert(unit.end(), body.begin(), body.end());

        std::vector<std::byte> out;
        for (auto b : unit) out.push_back(std::byte{b});
        return out;
    }
};

std::vector<std::byte> elf_with_debug_line(const std::vector<std::byte>& line) {
    std::vector<std::uint8_t> b(0x40, 0);
    b[0] = 0x7f; b[1] = 'E'; b[2] = 'L'; b[3] = 'F';
    b[4] = 2; b[5] = 2;
    auto put = [&](std::size_t at, std::uint64_t v, int width) {
        if (b.size() < at + static_cast<std::size_t>(width)) b.resize(at + width, 0);
        for (int i = 0; i < width; ++i) b[at + static_cast<std::size_t>(i)] =
            static_cast<std::uint8_t>((v >> ((width - 1 - i) * 8)) & 0xff);
    };

    const std::size_t names_off = 0x100;
    const std::string names = std::string("\0.debug_line\0.shstrtab\0", 23);
    b.resize(names_off + names.size(), 0);
    for (std::size_t i = 0; i < names.size(); ++i) b[names_off + i] = static_cast<std::uint8_t>(names[i]);

    const std::size_t line_off = 0x200;
    b.resize(line_off + line.size(), 0);
    for (std::size_t i = 0; i < line.size(); ++i) {
        b[line_off + i] = std::to_integer<std::uint8_t>(line[i]);
    }

    const std::size_t shoff = b.size() + 16;
    b.resize(shoff + 3 * 64, 0);
    auto section = [&](int idx, std::uint32_t name, std::uint32_t type, std::size_t off, std::size_t size) {
        const auto o = shoff + static_cast<std::size_t>(idx) * 64;
        put(o, name, 4);
        put(o + 4, type, 4);
        put(o + 0x18, off, 8);
        put(o + 0x20, size, 8);
    };
    section(0, 0, 0, 0, 0);
    section(1, 1, 1, line_off, line.size());          // ".debug_line"
    section(2, 13, 3, names_off, names.size());       // ".shstrtab"

    put(0x28, shoff, 8);
    put(0x3a, 64, 2);
    put(0x3c, 3, 2);
    put(0x3e, 2, 2);

    std::vector<std::byte> out;
    for (auto x : b) out.push_back(std::byte{x});
    return out;
}

} // namespace

TEST_CASE("the line program maps addresses to source lines", "[dwarf]") {
    line_unit u;
    u.set_address(0x10200);
    u.advance_line(41);
    u.copy();
    u.advance_pc(8);
    u.advance_line(1);
    u.copy();
    u.advance_pc(4);
    u.advance_line(7);
    u.copy();
    u.advance_pc(4);
    u.end_sequence();

    std::string err;
    const auto section = u.build("D:/C++/cellmark/src", "main.cpp");

    line_table lines;
    const bool parsed = lines.parse_section(section, &err);
    INFO("parse error: " << err << "  bytes: " << section.size() << "  rows: " << lines.size());
    REQUIRE(parsed);
    REQUIRE(lines.size() == 4);

    SECTION("an address on a row is that line") {
        const auto at = lines.find(0x10200);
        REQUIRE(at.has_value());
        CHECK(at->line == 42);
        CHECK(at->file == "D:/C++/cellmark/src/main.cpp");
    }

    SECTION("an address inside a row belongs to the line that started it") {
        const auto at = lines.find(0x10204);
        REQUIRE(at.has_value());
        CHECK(at->line == 42);
        CHECK(at->address == 0x10200);
    }

    SECTION("later rows win as the address advances") {
        CHECK(lines.find(0x10208)->line == 43);
        CHECK(lines.find(0x1020c)->line == 50);
    }

    SECTION("an address below the table is not attributed to anything") {
        CHECK_FALSE(lines.find(0x10000).has_value());
    }

    SECTION("a line can be looked up to put a breakpoint on it") {
        const auto at = lines.addresses_for("main.cpp", 43);
        REQUIRE(at.size() == 1);
        CHECK(at[0] == 0x10208);
    }
}

TEST_CASE("an image built without -g says so", "[dwarf]") {
    const auto image = elf_with_debug_line({});
    elf_image elf;
    std::string err;
    REQUIRE(elf.parse(image, &err));

    line_table lines;
    CHECK_FALSE(lines.parse(elf, &err));
    CHECK_FALSE(err.empty());
}

TEST_CASE("a line table can be read through an ELF as well", "[dwarf]") {
    line_unit u;
    u.set_address(0x10200);
    u.advance_line(41);
    u.copy();
    u.advance_pc(4);
    u.end_sequence();

    // named, so the bytes outlive the elf_image that points at them
    const auto image = elf_with_debug_line(u.build("D:/C++/cellmark/src", "main.cpp"));
    elf_image elf;
    std::string err;
    REQUIRE(elf.parse(image, &err));

    line_table lines;
    REQUIRE(lines.parse(elf, &err));
    const auto at = lines.find(0x10200);
    REQUIRE(at.has_value());
    CHECK(at->line == 42);
    CHECK(at->file == "D:/C++/cellmark/src/main.cpp");
}