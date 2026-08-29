#include "elf_image.h"

#include <fstream>

namespace opentm::tm_core {

namespace {

constexpr std::size_t sce_header_len_offset = 0x10;

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

bool elf_image::load(const std::string& path, std::string* error) {
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
    owned_.resize(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        owned_[i] = static_cast<std::byte>(static_cast<unsigned char>(raw[i]));
    }
    return parse(owned_, error);
}

bool elf_image::parse(std::span<const std::byte> file, std::string* error) {
    sections_.clear();

    const auto base = elf_offset_in(file);
    if (base == std::string::npos) {
        if (error) *error = "no ELF image here (not an ELF and not a debug SELF)";
        return false;
    }
    image_ = file.subspan(base);
    if (image_.size() < 0x40) {
        if (error) *error = "ELF header is truncated";
        return false;
    }

    is64_ = std::to_integer<std::uint8_t>(image_[4]) == 2;
    if (std::to_integer<std::uint8_t>(image_[5]) != 2) {
        if (error) *error = "only big-endian images are supported";
        return false;
    }

    const auto shoff = is64_ ? static_cast<std::size_t>(be64(image_, 0x28)) : static_cast<std::size_t>(be32(image_, 0x20));
    const auto shentsize = is64_ ? be16(image_, 0x3a) : be16(image_, 0x2e);
    const auto shnum     = is64_ ? be16(image_, 0x3c) : be16(image_, 0x30);
    const auto shstrndx  = is64_ ? be16(image_, 0x3e) : be16(image_, 0x32);
    if (shoff == 0 || shnum == 0 ||
        shoff + std::size_t(shnum) * shentsize > image_.size()) {
        if (error) *error = "this image has no section headers";
        return false;
    }

    auto raw_section = [&](std::uint16_t i) {
        const std::size_t o = shoff + std::size_t(i) * shentsize;
        section s{};
        s.type = be32(image_, o + 4);
        if (is64_) {
            s.addr       = be64(image_, o + 0x10);
            s.offset     = static_cast<std::size_t>(be64(image_, o + 0x18));
            s.size       = static_cast<std::size_t>(be64(image_, o + 0x20));
            s.link       = be32(image_, o + 0x28);
            s.entry_size = static_cast<std::size_t>(be64(image_, o + 0x38));
        } else {
            s.addr       = be32(image_, o + 0x0c);
            s.offset     = be32(image_, o + 0x10);
            s.size       = be32(image_, o + 0x14);
            s.link       = be32(image_, o + 0x18);
            s.entry_size = be32(image_, o + 0x24);
        }
        return s;
    };

    const auto names = (shstrndx < shnum) ? raw_section(shstrndx) : section{};
    for (std::uint16_t i = 0; i < shnum; ++i) {
        auto s = raw_section(i);
        const std::size_t o = shoff + std::size_t(i) * shentsize;
        const auto name_off = be32(image_, o);
        if (names.size != 0 && names.offset + name_off < image_.size()) {
            s.name = c_string(image_, names.offset + name_off, names.offset + names.size);
        }
        if (s.offset + s.size > image_.size()) s.size = 0;// dont hand out a bad span
        sections_.push_back(std::move(s));
    }
    return true;
}

const elf_image::section* elf_image::find_section(const std::string& name) const {
    for (const auto& s : sections_) {
        if (s.name == name) return &s;
    }
    return nullptr;
}

std::span<const std::byte> elf_image::section_data(const std::string& name) const {
    const auto* s = find_section(name);
    if (s == nullptr || s->size == 0) return {};
    return image_.subspan(s->offset, s->size);
}

} // namespace opentm::tm_core