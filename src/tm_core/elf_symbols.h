#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace opentm::tm_core {

struct elf_symbol {
    std::uint64_t address = 0;
    std::uint64_t size    = 0;
    std::string   name;

    bool covers(std::uint64_t a) const noexcept {
        return size == 0 ? a == address : (a >= address && a < address + size);
    }
};

class symbol_table {
public:
    bool load(const std::string& path, std::string* error = nullptr);
    bool parse(std::span<const std::byte> image, std::string* error = nullptr);

    bool empty() const noexcept { return functions_.empty(); }
    std::size_t size() const noexcept { return functions_.size(); }
    const std::vector<elf_symbol>& functions() const noexcept { return functions_; }

    const elf_symbol* find(std::uint64_t address) const;
    std::optional<std::string> describe(std::uint64_t address) const;

    const std::string& source_path() const noexcept { return path_; }

private:
    std::vector<elf_symbol> functions_;
    std::string             path_;
};

} // namespace opentm::tm_core