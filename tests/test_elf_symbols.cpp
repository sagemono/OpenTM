#include <catch2/catch_test_macros.hpp>

#include <tm_core/elf_symbols.h>

#include <cstdint>
#include <vector>

using opentm::tm_core::elf_symbol;
using opentm::tm_core::symbol_table;

namespace {

struct fake_elf {
    std::vector<std::uint8_t> b;

    void put(std::size_t at, std::uint64_t v, int width) {
        if (b.size() < at + static_cast<std::size_t>(width)) b.resize(at + width, 0);
        for (int i = 0; i < width; ++i) {
            b[at + static_cast<std::size_t>(i)] =
                static_cast<std::uint8_t>((v >> ((width - 1 - i) * 8)) & 0xff);
        }
    }

    std::vector<std::byte> build(const std::vector<elf_symbol>& syms) {
        b.assign(0x40, 0);
        b[0] = 0x7f; b[1] = 'E'; b[2] = 'L'; b[3] = 'F';
        b[4] = 2;    // 64-bit
        b[5] = 2;    // big endian

        const std::size_t strtab_off = 0x100;
        std::vector<std::uint8_t> strs{0};
        std::vector<std::size_t> name_offs;
        for (const auto& s : syms) {
            name_offs.push_back(strs.size());
            for (char c : s.name) strs.push_back(static_cast<std::uint8_t>(c));
            strs.push_back(0);
        }

        const std::size_t symtab_off = strtab_off + strs.size() + 8;
        b.resize(symtab_off + syms.size() * 24, 0);
        for (std::size_t i = 0; i < strs.size(); ++i) b[strtab_off + i] = strs[i];

        for (std::size_t i = 0; i < syms.size(); ++i) {
            const auto o = symtab_off + i * 24;
            put(o, name_offs[i], 4);
            b[o + 4] = 2;// STT_FUNC
            put(o + 8, syms[i].address, 8);
            put(o + 16, syms[i].size, 8);
        }

        const std::size_t shoff = symtab_off + syms.size() * 24 + 16;
        b.resize(shoff + 3 * 64, 0);
        auto section = [&](int idx, std::uint32_t type, std::size_t off, std::size_t size, std::uint32_t link, std::size_t entsize) {
            const auto o = shoff + static_cast<std::size_t>(idx) * 64;
            put(o + 4, type, 4);
            put(o + 0x18, off, 8);
            put(o + 0x20, size, 8);
            put(o + 0x28, link, 4);
            put(o + 0x38, entsize, 8);
        };
        section(0, 0, 0, 0, 0, 0);
        section(1, 2, symtab_off, syms.size() * 24, 2, 24);   // SHT_SYMTAB -> strtab
        section(2, 3, strtab_off, strs.size(), 0, 0);

        put(0x28, shoff, 8);// e_shoff
        put(0x3a, 64, 2);   // e_shentsize
        put(0x3c, 3, 2);// e_shnum
        put(0x3e, 2, 2);// e_shstrndx

        std::vector<std::byte> out;
        out.reserve(b.size());
        for (auto x : b) out.push_back(std::byte{x});
        return out;
    }
};

// wrap an image the way a debug SELF does: SCE header, ELF at header_len
std::vector<std::byte> as_self(const std::vector<std::byte>& elf, std::size_t header_len) {
    std::vector<std::byte> out(header_len, std::byte{0});
    out[0] = std::byte{'S'}; out[1] = std::byte{'C'}; out[2] = std::byte{'E'}; out[3] = std::byte{0};
    for (int i = 0; i < 8; ++i) {
        out[0x10 + static_cast<std::size_t>(i)] = std::byte{static_cast<std::uint8_t>((header_len >> ((7 - i) * 8)) & 0xff)};
    }
    out.insert(out.end(), elf.begin(), elf.end());
    return out;
}

const std::vector<elf_symbol> cellmark_like{
    {0x00010230, 0x24,  "._start"},
    {0x00010354, 0x184, "._initialize"},
    {0x000106f8, 0x600, ".main"},
    {0x00048504, 0x80,  ".cellGcmFinish"},
};

} // namespace

TEST_CASE("function symbols come out of a plain ELF", "[symbols]") {
    fake_elf f;
    symbol_table table;
    std::string err;
    REQUIRE(table.parse(f.build(cellmark_like), &err));
    CHECK(table.size() == 4);

    SECTION("an address inside a function is named with its offset") {
        CHECK(*table.describe(0x48558) == ".cellGcmFinish+0x54");
        CHECK(*table.describe(0x10250) == "._start+0x20");
    }

    SECTION("an entry point is named without an offset") {
        CHECK(*table.describe(0x000106f8) == ".main");
    }

    SECTION("an address in no function is not guessed at") {
        CHECK_FALSE(table.describe(0x1000).has_value()); // below everything
        CHECK_FALSE(table.describe(0x00010300).has_value()); // in a gap
        CHECK_FALSE(table.describe(0x00900000).has_value()); // past the end
    }
}

TEST_CASE("symbols are read straight out of a debug SELF", "[symbols]") {
    fake_elf f;
    const auto elf = f.build(cellmark_like);

    symbol_table table;
    std::string err;
    REQUIRE(table.parse(as_self(elf, 0x980), &err));
    CHECK(table.size() == 4);
    CHECK(*table.describe(0x48558) == ".cellGcmFinish+0x54");
}

TEST_CASE("images without symbols are rejected, not half-read", "[symbols]") {
    symbol_table table;
    std::string err;

    SECTION("something that is not an image at all") {
        std::vector<std::byte> junk(64, std::byte{0xab});
        CHECK_FALSE(table.parse(junk, &err));
        CHECK_FALSE(err.empty());
    }

    SECTION("a stripped ELF") {
        fake_elf f;
        CHECK_FALSE(table.parse(f.build({}), &err));
        CHECK(table.empty());
    }
}
