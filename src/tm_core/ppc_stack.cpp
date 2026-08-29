#include "ppc_stack.h"

namespace opentm::tm_core {

namespace {

bool holds(std::uint64_t base, std::size_t size, std::uint64_t address) {
    if (address < base) return false;
    const auto offset = address - base;
    return offset + 8 <= size;
}

std::uint64_t read_be_u64_at(std::span<const std::byte> block, std::uint64_t base, std::uint64_t address)
{
    const auto offset = static_cast<std::size_t>(address - base);
    std::uint64_t v = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        v = (v << 8) | static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(block[offset + i]));
    }
    return v;
}

} // namespace

std::vector<stack_frame> walk_stack(std::span<const std::byte> stack, std::uint64_t stack_base, std::uint64_t pc, std::uint64_t sp, std::size_t max_frames)
{
    std::vector<stack_frame> out;
    if (max_frames == 0) return out;

    out.push_back(stack_frame{pc, sp});

    auto current = sp;
    while (out.size() < max_frames) {
        if (!holds(stack_base, stack.size(), current + stack_back_chain_offset)) break;

        const auto caller = read_be_u64_at(stack, stack_base, current + stack_back_chain_offset);
        if (caller <= current) break;
        if (!holds(stack_base, stack.size(), caller + stack_lr_save_offset)) break;

        const auto return_address = read_be_u64_at(stack, stack_base, caller + stack_lr_save_offset);
        if (return_address == 0) break;
        if (return_address > 0xffffffffull) break; //check later??

        out.push_back(stack_frame{return_address, caller});
        current = caller;
    }
    return out;
}

} // namespace opentm::tm_core
