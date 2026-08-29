#include "elf_symbols.h"

#include <algorithm>
#include <cstring>
#include <fstream>

namespace opentm::tm_core {

namespace {

constexpr std::size_t sce_header_len_offset = 0x10; //elf start

std::uint16_t be16(std::span<const std::byte> d, std::size_t o) {
    return static_cast<std::uint16_t>((std::to_integer<std::uint8_t>(d[o]) << 8) | std::to_integer<std::uint8_t>(d[o + 1]));
}
std::uint32_t be32(std::span<const std::byte> d, std::size_t o) {
    std::uint32_t v = 0;
    for (std::size_t i = 0; i < 4; ++i) v = (v << 8) | std::to_integer<std::uint8_t>(d[o + i]);
    return v;
}
std::uint64_t be64(std::span<const std::byte> d, std::size_t o) {
    std::uint64_t v = 0;
    for (std::size_t i = 0; i < 8; ++i) v = (v << 8) | std::to_integer<std::uint8_t>(d[o + i]);
    return v;
}

bool is_elf(std::span<const std::byte> d, std::size_t at) {
    return d.size() >= at + 4 &&
           std::to_integer<std::uint8_t>(d[at])     == 0x7f &&
           std::to_integer<std::uint8_t>(d[at + 1]) == 'E' &&
           std::to_integer<std::uint8_t>(d[at + 2]) == 'L' &&
           std::to_integer<std::uint8_t>(d[at + 3]) == 'F';
}

bool is_sce(std::span<const std::byte> d) {
    return d.size() >= 4 &&
           std::to_integer<std::uint8_t>(d[0]) == 'S' &&
           std::to_integer<std::uint8_t>(d[1]) == 'C' &&
           std::to_integer<std::uint8_t>(d[2]) == 'E' &&
           std::to_integer<std::uint8_t>(d[3]) == 0;
}

std::size_t elf_offset_in(std::span<const std::byte> d) {
    if (is_elf(d, 0)) return 0;
    if (is_sce(d) && d.size() > sce_header_len_offset + 8) {
        const auto at = static_cast<std::size_t>(be64(d, sce_header_len_offset));
        if (is_elf(d, at)) return at;
    }
    for (std::size_t at = 0; at + 4 < d.size() && at < 0x10000; ++at) {
        if (is_elf(d, at)) return at;
    }
    return std::string::npos;
}

std::string c_string(std::span<const std::byte> d, std::size_t start, std::size_t limit) {
    std::string out;
    for (std::size_t i = start; i < limit && i < d.size(); ++i) {
        const auto c = std::to_integer<char>(d[i]);
        if (c == '\0') break;
        out.push_back(c);
    }
    return out;
}

} // namespace

bool symbol_table::load(const std::string& path, std::string* error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (error) *error = "cannot open " + path;
        return false;
    }
    std::vector<char> raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (raw.empty()) {
        if (error) *error = path + " is empty";
        return false;
    }
    const std::span<const std::byte> image(reinterpret_cast<const std::byte*>(raw.data()), raw.size());
    if (!parse(image, error)) return false;
    path_ = path;
    return true;
}

bool symbol_table::parse(std::span<const std::byte> image, std::string* error) {
    functions_.clear();

    const auto base = elf_offset_in(image);
    if (base == std::string::npos) {
        if (error) *error = "no ELF image here (not an ELF and not a debug SELF)";
        return false;
    }
    const auto d = image.subspan(base);
    if (d.size() < 0x40) {
        if (error) *error = "ELF header is truncated";
        return false;
    }

    const bool is64 = std::to_integer<std::uint8_t>(d[4]) == 2;
    if (std::to_integer<std::uint8_t>(d[5]) != 2) {
        if (error) *error = "only big-endian images are supported";
        return false;
    }

    const auto shoff = is64 ? static_cast<std::size_t>(be64(d, 0x28)) : static_cast<std::size_t>(be32(d, 0x20));
    const auto shentsize = is64 ? be16(d, 0x3a) : be16(d, 0x2e);
    const auto shnum     = is64 ? be16(d, 0x3c) : be16(d, 0x30);
    if (shoff == 0 || shnum == 0 || shoff + std::size_t(shnum) * shentsize > d.size()) {
        if (error) *error = "this image has no section headers, so it carries no symbols";
        return false;
    }

    struct section { std::uint32_t type; std::size_t off, size, entsize; std::uint32_t link; };
    auto read_section = [&](std::uint16_t i) {
        const std::size_t o = shoff + std::size_t(i) * shentsize;
        section s{};
        s.type = be32(d, o + 4);
        if (is64) {
            s.off     = static_cast<std::size_t>(be64(d, o + 0x18));
            s.size    = static_cast<std::size_t>(be64(d, o + 0x20));
            s.link    = be32(d, o + 0x28);
            s.entsize = static_cast<std::size_t>(be64(d, o + 0x38));
        } else {
            s.off     = be32(d, o + 0x10);
            s.size    = be32(d, o + 0x14);
            s.link    = be32(d, o + 0x18);
            s.entsize = be32(d, o + 0x24);
        }
        return s;
    };

    constexpr std::uint32_t sht_symtab = 2;
    constexpr std::uint8_t  stt_func   = 2;

    for (std::uint16_t i = 0; i < shnum; ++i) {
        const auto sym = read_section(i);
        if (sym.type != sht_symtab || sym.entsize == 0) continue;
        if (sym.off + sym.size > d.size()) continue;
        if (sym.link >= shnum) continue;

        const auto str = read_section(static_cast<std::uint16_t>(sym.link));
        if (str.off + str.size > d.size()) continue;

        const auto count = sym.size / sym.entsize;
        functions_.reserve(functions_.size() + count);
        for (std::size_t k = 0; k < count; ++k) {
            const std::size_t o = sym.off + k * sym.entsize;
            std::uint32_t name_off = 0;
            std::uint8_t  info     = 0;
            std::uint64_t value    = 0, size = 0;
            if (is64) {
                name_off = be32(d, o);
                info     = std::to_integer<std::uint8_t>(d[o + 4]);
                value    = be64(d, o + 8);
                size     = be64(d, o + 16);
            } else {
                name_off = be32(d, o);
                value    = be32(d, o + 4);
                size     = be32(d, o + 8);
                info     = std::to_integer<std::uint8_t>(d[o + 12]);
            }
            if ((info & 0xf) != stt_func || value == 0) continue;

            auto name = c_string(d, str.off + name_off, str.off + str.size);
            if (name.empty()) continue;
            functions_.push_back(elf_symbol{value, size, std::move(name)});
        }
    }

    if (functions_.empty()) {
        if (error) *error = "no function symbols here (a stripped image?)";
        return false;
    }

    std::sort(functions_.begin(), functions_.end(), [](const elf_symbol& a, const elf_symbol& b) { return a.address < b.address; });
    return true;
}

const elf_symbol* symbol_table::find(std::uint64_t address) const {
    if (functions_.empty()) return nullptr;
    // the last symbol starting at or below the address
    auto it = std::upper_bound(functions_.begin(), functions_.end(), address, [](std::uint64_t a, const elf_symbol& s) { return a < s.address; });
    if (it == functions_.begin()) return nullptr;
    --it;
    return it->covers(address) ? &*it : nullptr;
}

std::optional<std::string> symbol_table::describe(std::uint64_t address) const {
    const auto* sym = find(address);
    if (sym == nullptr) return std::nullopt;
    if (address == sym->address) return sym->name;
    char buf[32];
    std::snprintf(buf, sizeof buf, "+0x%llx", static_cast<unsigned long long>(address - sym->address));
    return sym->name + buf;
}

} // namespace opentm::tm_core