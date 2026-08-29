#pragma once

#include "elf_image.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace opentm::tm_core {

struct source_location {
    std::string   file;
    std::uint32_t line = 0;
    std::uint64_t address = 0;
    bool          end_of_sequence = false;
};

class line_table {
public:
    bool load(const std::string& path, std::string* error = nullptr);
    bool parse(const elf_image& elf, std::string* error = nullptr);
    bool parse_section(std::span<const std::byte> debug_line, std::string* error = nullptr);

    bool empty() const noexcept { return rows_.empty(); }
    std::size_t size() const noexcept { return rows_.size(); }

    std::optional<source_location> find(std::uint64_t address) const;
    std::vector<std::uint64_t> addresses_for(const std::string& file, std::uint32_t line) const;

    const std::vector<source_location>& rows() const noexcept { return rows_; }

private:
    bool run_unit(std::span<const std::byte> unit, std::string* error);

    std::vector<source_location> rows_;// sorted by addy
};

} // namespace opentm::tm_core