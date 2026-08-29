#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace opentm::tm_core {

class elf_image {
public:
    struct section {
        std::string   name;
        std::uint64_t addr = 0;
        std::size_t   offset = 0;
        std::size_t   size = 0;
        std::uint32_t type = 0;
        std::uint32_t link = 0;
        std::size_t   entry_size = 0;
    };

    bool load(const std::string& path, std::string* error = nullptr);
    bool parse(std::span<const std::byte> file, std::string* error = nullptr);

    bool is_64bit() const noexcept { return is64_; }
    bool valid() const noexcept { return !sections_.empty(); }
    const std::vector<section>& sections() const noexcept { return sections_; }

    std::span<const std::byte> image() const noexcept { return image_; }
    std::span<const std::byte> section_data(const std::string& name) const;
    const section* find_section(const std::string& name) const;

private:
    std::vector<std::byte>     owned_; // set when we read the file ourselves
    std::span<const std::byte> image_;
    std::vector<section>       sections_;
    bool                       is64_ = true;
};

} // namespace opentm::tm_core
