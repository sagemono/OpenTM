#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace opentm::tm_core {

struct stack_frame {
    std::uint64_t address = 0;
    std::uint64_t frame   = 0;
};

std::vector<stack_frame> walk_stack(std::span<const std::byte> stack, std::uint64_t stack_base, std::uint64_t pc, std::uint64_t sp, std::size_t max_frames = 64);

inline constexpr std::size_t stack_back_chain_offset = 0;
inline constexpr std::size_t stack_lr_save_offset    = 16;

} // namespace opentm::tm_core
