#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace opentm::tm_core {

struct ppc_insn {
    std::uint64_t address = 0;
    std::uint32_t opcode  = 0;
    std::string   mnemonic;
    std::string   operands;
    bool          valid   = false;

    std::optional<std::uint64_t> branch_target;
    bool is_call   = false;
    bool is_branch = false;
    bool is_return = false;
    bool is_conditional = false;

    enum class indirect { none, via_lr, via_ctr };
    indirect through = indirect::none;

    std::string text() const;// "mnemonic operands"
};

class ppc_disassembler {
public:
    ppc_disassembler();
    ~ppc_disassembler();

    ppc_disassembler(const ppc_disassembler&) = delete;
    ppc_disassembler& operator=(const ppc_disassembler&) = delete;

    bool is_open() const noexcept { return handle_ != 0; }

    static constexpr std::uint32_t insn_size = 4;

    std::vector<ppc_insn> disassemble(std::span<const std::byte> code, std::uint64_t address) const;
    std::optional<ppc_insn> disassemble_one(std::span<const std::byte> code, std::uint64_t address) const;

private:
    std::size_t handle_ = 0;// csh
};

} // namespace opentm::tm_core
