#include "ppc_disasm.h"

#include <capstone/capstone.h>

#include <cstdio>

namespace opentm::tm_core {

std::string ppc_insn::text() const {
    if (operands.empty()) return mnemonic;
    return mnemonic + " " + operands;
}

namespace {

struct branch_kind {
    bool is_branch = false;
    bool is_call   = false;
    bool is_return = false;
    bool is_conditional = false;
    ppc_insn::indirect through = ppc_insn::indirect::none;
};

bool always_taken(std::uint32_t opcode) {
    const auto bo = (opcode >> 21) & 0x1f;
    return (bo & 0x14) == 0x14;
}

branch_kind classify(std::uint32_t opcode) {
    const auto primary = (opcode >> 26) & 0x3f;
    const bool lk      = (opcode & 1) != 0;

    branch_kind k;
    switch (primary) {
    case 16:
        k.is_branch     = true;
        k.is_call       = lk;
        k.is_conditional = !always_taken(opcode);
        break;
    case 18:
        k.is_branch = true;
        k.is_call   = lk;
        break;
    case 19: {
        const auto xo = (opcode >> 1) & 0x3ff;
        if (xo == 16) {
            k.is_branch      = true;
            k.is_call        = lk;
            k.is_return      = !lk;
            k.is_conditional = !always_taken(opcode);
            k.through        = ppc_insn::indirect::via_lr;
        } else if (xo == 528) {
            k.is_branch      = true;
            k.is_call        = lk;
            k.is_conditional = !always_taken(opcode);
            k.through        = ppc_insn::indirect::via_ctr;
        }
        break;
    }
    default:
        break;
    }
    return k;
}

std::optional<std::uint64_t> immediate_target(const cs_insn& insn) {
    if (insn.detail == nullptr) return std::nullopt;
    const auto& ppc = insn.detail->ppc;
    for (std::uint8_t i = 0; i < ppc.op_count; ++i) {
        if (ppc.operands[i].type == PPC_OP_IMM) {
            return static_cast<std::uint64_t>(ppc.operands[i].imm);
        }
    }
    return std::nullopt;
}

} // namespace

ppc_disassembler::ppc_disassembler() {
    csh h = 0;
    const auto mode = static_cast<cs_mode>(CS_MODE_64 | CS_MODE_BIG_ENDIAN);
    if (cs_open(CS_ARCH_PPC, mode, &h) != CS_ERR_OK) {
        handle_ = 0;
        return;
    }
    cs_option(h, CS_OPT_DETAIL, CS_OPT_ON);
    handle_ = static_cast<std::size_t>(h);
}

ppc_disassembler::~ppc_disassembler() {
    if (handle_ != 0) {
        auto h = static_cast<csh>(handle_);
        cs_close(&h);
    }
}

std::vector<ppc_insn> ppc_disassembler::disassemble(std::span<const std::byte> code, std::uint64_t address) const
{
    std::vector<ppc_insn> out;
    if (code.empty()) return out;

    const auto words = code.size() / insn_size;
    out.reserve(words);

    for (std::size_t i = 0; i < words; ++i) {
        const auto at = address + i * insn_size;
        const auto one = disassemble_one(code.subspan(i * insn_size, insn_size), at);
        if (one) {
            out.push_back(*one);
            continue;
        }
        ppc_insn bad;
        bad.address  = at;
        bad.mnemonic = ".long";
        const auto* p = reinterpret_cast<const std::uint8_t*>(code.data()) + i * insn_size;
        bad.opcode = (static_cast<std::uint32_t>(p[0]) << 24) |
                     (static_cast<std::uint32_t>(p[1]) << 16) |
                     (static_cast<std::uint32_t>(p[2]) << 8)  |
                      static_cast<std::uint32_t>(p[3]);
        char buf[16];
        std::snprintf(buf, sizeof buf, "0x%08x", bad.opcode);
        bad.operands = buf;
        bad.valid = false;
        out.push_back(bad);
    }
    return out;
}

std::optional<ppc_insn> ppc_disassembler::disassemble_one(std::span<const std::byte> code, std::uint64_t address) const
{
    if (handle_ == 0 || code.size() < insn_size) return std::nullopt;
    const auto h = static_cast<csh>(handle_);

    cs_insn* insn = nullptr;
    const auto n = cs_disasm(h, reinterpret_cast<const std::uint8_t*>(code.data()), insn_size, address, 1, &insn);
    if (n == 0) {
        if (insn) cs_free(insn, n);
        return std::nullopt;
    }

    ppc_insn out;
    out.address  = insn->address;
    out.mnemonic = insn->mnemonic;
    out.operands = insn->op_str;
    out.valid    = true;

    const auto* p = reinterpret_cast<const std::uint8_t*>(code.data());
    out.opcode = (static_cast<std::uint32_t>(p[0]) << 24) |
                 (static_cast<std::uint32_t>(p[1]) << 16) |
                 (static_cast<std::uint32_t>(p[2]) << 8)  |
                  static_cast<std::uint32_t>(p[3]);

    const auto kind = classify(out.opcode);
    out.is_branch      = kind.is_branch;
    out.is_call        = kind.is_call;
    out.is_return      = kind.is_return;
    out.is_conditional = kind.is_conditional;
    out.through        = kind.through;
    if (out.is_branch) out.branch_target = immediate_target(*insn);

    cs_free(insn, n);
    return out;
}

} // namespace opentm::tm_core