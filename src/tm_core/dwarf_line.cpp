#include "dwarf_line.h"

#include <algorithm>

namespace opentm::tm_core {

namespace {

enum : std::uint8_t {
    DW_LNS_copy = 1,
    DW_LNS_advance_pc = 2,
    DW_LNS_advance_line = 3,
    DW_LNS_set_file = 4,
    DW_LNS_set_column = 5,
    DW_LNS_negate_stmt = 6,
    DW_LNS_set_basic_block = 7,
    DW_LNS_const_add_pc = 8,
    DW_LNS_fixed_advance_pc = 9,
};
enum : std::uint8_t {
    DW_LNE_end_sequence = 1,
    DW_LNE_set_address = 2,
    DW_LNE_define_file = 3,
};

struct reader {
    std::span<const std::byte> d;
    std::size_t at = 0;

    bool ok(std::size_t n = 1) const { return at + n <= d.size(); }
    std::uint8_t u8() { return ok() ? std::to_integer<std::uint8_t>(d[at++]) : 0; }

    std::uint16_t be16() {
        std::uint16_t v = 0;
        for (int i = 0; i < 2; ++i) v = static_cast<std::uint16_t>((v << 8) | u8());
        return v;
    }
    std::uint32_t be32() {
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v = (v << 8) | u8();
        return v;
    }
    std::uint64_t be64() {
        std::uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v = (v << 8) | u8();
        return v;
    }

    std::uint64_t uleb() {
        std::uint64_t v = 0;
        int shift = 0;
        while (ok()) {
            const auto b = u8();
            if (shift < 64) v |= static_cast<std::uint64_t>(b & 0x7f) << shift;
            shift += 7;
            if ((b & 0x80) == 0) break;
        }
        return v;
    }
    std::int64_t sleb() {
        std::int64_t v = 0;
        int shift = 0;
        std::uint8_t b = 0;
        while (ok()) {
            b = u8();
            if (shift < 64) v |= static_cast<std::int64_t>(b & 0x7f) << shift;
            shift += 7;
            if ((b & 0x80) == 0) break;
        }
        if (shift < 64 && (b & 0x40)) v |= -(static_cast<std::int64_t>(1) << shift);
        return v;
    }
    std::string cstr() {
        std::string s;
        while (ok()) {
            const auto c = static_cast<char>(u8());
            if (c == '\0') break;
            s.push_back(c);
        }
        return s;
    }
};

std::string join(const std::string& dir, const std::string& name) {
    if (dir.empty() || name.empty()) return name;
    if (name.front() == '/' || name.front() == '\\') return name;
    if (name.size() > 1 && name[1] == ':') return name; // already abs
    return dir + "/" + name;
}

} // namespace

bool line_table::load(const std::string& path, std::string* error) {
    elf_image elf;
    if (!elf.load(path, error)) return false;
    return parse(elf, error);
}

bool line_table::parse(const elf_image& elf, std::string* error) {
    const auto section = elf.section_data(".debug_line");
    if (section.empty()) {
        if (error) *error = "this image has no .debug_line, so it was built without -g";
        rows_.clear();
        return false;
    }
    return parse_section(section, error);
}

bool line_table::parse_section(std::span<const std::byte> section, std::string* error) {
    rows_.clear();
    if (section.empty()) {
        if (error) *error = "no .debug_line data";
        return false;
    }

    std::size_t at = 0;
    while (at + 10 <= section.size()) {
        reader head{section, at};
        const auto unit_length = head.be32();
        if (unit_length == 0 || unit_length == 0xffffffffu) break;
        const auto unit_end = at + 4 + unit_length;
        if (unit_end > section.size()) break;

        if (!run_unit(section.subspan(at, unit_end - at), error)) {
            // .
        }
        at = unit_end;
    }

    const bool any_real = std::any_of(rows_.begin(), rows_.end(),
        [](const source_location& r) { return !r.end_of_sequence; });
    if (!any_real) {
        if (error && error->empty()) *error = "no line rows in .debug_line";
        return false;
    }

    std::sort(rows_.begin(), rows_.end(),
              [](const source_location& a, const source_location& b) {
                  if (a.address != b.address) return a.address < b.address;
                  return !a.end_of_sequence && b.end_of_sequence;
              });
    return true;
}

bool line_table::run_unit(std::span<const std::byte> unit, std::string* error) {
    reader r{unit, 0};
    r.be32();
    const auto version = r.be16();
    if (version > 4) {
        if (error) *error = "DWARF 5 line tables are not supported";
        return false;
    }
    const auto header_length = r.be32();
    const auto program_start = r.at + header_length;

    const auto min_insn_length = r.u8();
    const std::uint8_t max_ops = (version >= 4) ? r.u8() : 1;
    (void)max_ops;
    const auto default_is_stmt = r.u8();
    (void)default_is_stmt;
    const auto line_base  = static_cast<std::int8_t>(r.u8());
    const auto line_range = r.u8();
    const auto opcode_base = r.u8();

    std::vector<std::uint8_t> std_opcode_lengths(opcode_base > 0 ? opcode_base - 1 : 0);
    for (auto& n : std_opcode_lengths) n = r.u8();

    std::vector<std::string> dirs{std::string()};
    while (r.ok()) {
        auto dir = r.cstr();
        if (dir.empty()) break;
        dirs.push_back(std::move(dir));
    }

    struct file_entry { std::string name; std::uint64_t dir = 0; };
    std::vector<file_entry> files{file_entry{}};
    while (r.ok()) {
        auto name = r.cstr();
        if (name.empty()) break;
        file_entry f;
        f.name = std::move(name);
        f.dir  = r.uleb();
        r.uleb();   // mtime
        r.uleb();   // length
        files.push_back(std::move(f));
    }

    auto file_name = [&](std::uint64_t index) -> std::string {
        if (index >= files.size()) return {};
        const auto& f = files[index];
        const auto dir = (f.dir < dirs.size()) ? dirs[f.dir] : std::string();
        return join(dir, f.name);
    };

    struct state {
        std::uint64_t address = 0;
        std::uint64_t file = 1;
        std::int64_t  line = 1;
    };
    state s;
    auto emit_row = [&] {
        if (s.line > 0 && s.address != 0) {
            rows_.push_back(source_location{file_name(s.file), static_cast<std::uint32_t>(s.line), s.address});
        }
    };

    r.at = program_start;
    while (r.at < unit.size()) {
        const auto op = r.u8();

        if (op >= opcode_base) {
            const auto adjusted = op - opcode_base;
            if (line_range != 0) {
                s.address += static_cast<std::uint64_t>(adjusted / line_range) * min_insn_length;
                s.line    += line_base + (adjusted % line_range);
            }
            emit_row();
            continue;
        }

        if (op == 0) {
            const auto length = r.uleb();
            const auto next = r.at + static_cast<std::size_t>(length);
            if (length == 0) continue;
            const auto sub = r.u8();
            if (sub == DW_LNE_end_sequence) {
                if (s.address != 0) {
                    rows_.push_back(source_location{std::string(), 0, s.address, true});
                }
                s = state{};
            } else if (sub == DW_LNE_set_address) {
                s.address = (length >= 9) ? r.be64() : r.be32();
            } else if (sub == DW_LNE_define_file) {
                file_entry f;
                f.name = r.cstr();
                f.dir  = r.uleb();
                r.uleb();
                r.uleb();
                files.push_back(std::move(f));
            }
            r.at = next;
            continue;
        }

        switch (op) {
        case DW_LNS_copy:              emit_row(); break;
        case DW_LNS_advance_pc:        s.address += r.uleb() * min_insn_length; break;
        case DW_LNS_advance_line:      s.line += r.sleb(); break;
        case DW_LNS_set_file:          s.file = r.uleb(); break;
        case DW_LNS_set_column:        r.uleb(); break;
        case DW_LNS_negate_stmt:       break;
        case DW_LNS_set_basic_block:   break;
        case DW_LNS_const_add_pc:
            if (line_range != 0) {
                s.address += static_cast<std::uint64_t>((255 - opcode_base) / line_range) * min_insn_length;
            }
            break;
        case DW_LNS_fixed_advance_pc:  s.address += r.be16(); break;
        default:
            if (op - 1 < std_opcode_lengths.size()) {
                for (std::uint8_t i = 0; i < std_opcode_lengths[op - 1]; ++i) r.uleb();
            }
            break;
        }
    }
    return true;
}

std::optional<source_location> line_table::find(std::uint64_t address) const {
    if (rows_.empty()) return std::nullopt;
    auto it = std::upper_bound(rows_.begin(), rows_.end(), address,
                               [](std::uint64_t a, const source_location& r) {
                                   return a < r.address;
                               });
    if (it == rows_.begin()) return std::nullopt;
    --it;
    if (it->end_of_sequence) return std::nullopt; // in a gap between sequences
    return *it;
}

std::vector<std::uint64_t> line_table::addresses_for(const std::string& file, std::uint32_t line) const
{
    std::vector<std::uint64_t> out;
    for (const auto& row : rows_) {
        if (row.end_of_sequence || row.line != line) continue;
        if (!file.empty() && row.file.find(file) == std::string::npos) continue;
        out.push_back(row.address);
    }
    return out;
}

} // namespace opentm::tm_core