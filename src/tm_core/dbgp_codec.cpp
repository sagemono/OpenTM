#include "dbgp_codec.h"

#include "be_io.h"

#include <algorithm>
#include <cstring>

namespace opentm::tm_core::dbgp {

namespace {

std::span<const std::byte> payload_view(const response& r) noexcept {
    return std::span<const std::byte>(r.payload.data(), r.payload.size());
}

} // namespace


// framing

std::vector<std::byte> encode_request(const request& r) {
    std::vector<std::byte> out;
    out.reserve(request_header_size + r.payload.size() + 1);
    append_be_u32(out, r.cmd);
    append_be_u32(out, r.req_id);
    append_be_u32(out, static_cast<std::uint32_t>(r.payload.size()));
    append_be_u32(out, r.process_id);
    out.insert(out.end(), r.payload.begin(), r.payload.end());

    std::uint32_t s = 0;
    for (auto b : out) s += std::to_integer<std::uint8_t>(b);
    out.push_back(std::byte{static_cast<std::uint8_t>((s - 0x10u) & 0xffu)});
    return out;
}

std::optional<response> decode_response(std::span<const std::byte> bytes) {
    if (bytes.size() < response_header_size + 1) return std::nullopt;
    response r;
    r.cmd         = read_be_u32(bytes, 0);
    r.req_id      = read_be_u32(bytes, 4);
    r.data_len    = read_be_u32(bytes, 8);
    r.process_id  = read_be_u32(bytes, 12);
    r.result_code = read_be_u32(bytes, 16);

    const auto avail = bytes.size() - response_header_size;
    const auto take  = std::min<std::size_t>(r.data_len, avail > 0 ? avail - 1 : 0);
    r.payload.assign(bytes.begin() + response_header_size, bytes.begin() + response_header_size + take);
    return r;
}


// per cmd parsers

std::vector<std::uint32_t> parse_process_list(const response& r) {
    std::vector<std::uint32_t> out;
    const auto p = payload_view(r);
    for (std::size_t off = 0; off + 4 <= p.size(); off += 4) {
        out.push_back(read_be_u32(p, off));
    }
    return out;
}

std::optional<thread_list> parse_thread_list(const response& r) {
    const auto p = payload_view(r);
    if (p.size() < 8) return std::nullopt;
    thread_list tl;
    const auto ppu_n = read_be_u32(p, 0);
    const auto spu_n = read_be_u32(p, 4);
    std::size_t off = 8;
    tl.ppu_thread_ids.reserve(ppu_n);
    for (std::uint32_t i = 0; i < ppu_n; ++i) {
        if (off + 8 > p.size()) return std::nullopt;
        tl.ppu_thread_ids.push_back(read_be_u64(p, off));
        off += 8;
    }
    tl.spu_thread_group_ids.reserve(spu_n);
    for (std::uint32_t i = 0; i < spu_n; ++i) {
        if (off + 4 > p.size()) return std::nullopt;
        tl.spu_thread_group_ids.push_back(read_be_u32(p, off));
        off += 4;
    }
    return tl;
}

std::vector<std::byte> build_ppu_thread_info_request_body(std::uint64_t tid) {
    std::vector<std::byte> body;
    body.reserve(8);
    append_be_u64(body, tid);
    return body;
}

std::optional<ppu_thread_info> parse_ppu_thread_info(const response& r) {
    const auto p = payload_view(r);
    if (p.size() < 36) return std::nullopt;
    ppu_thread_info info;
    info.thread_id     = read_be_u64(p, 0);
    info.priority      = read_be_u32(p, 8);
    info.state         = read_be_u32(p, 12);
    info.stack_addr    = read_be_u64(p, 16);
    info.stack_size    = read_be_u64(p, 24);
    info.base_priority = read_be_u32(p, 32);
    if (p.size() > 36) info.name = read_cstr(p, 36);
    return info;
}

const char* ppu_thread_state_name(std::uint32_t state) noexcept {
    switch (state) {
    case 0: return "IDLE";
    case 1: return "RUNNABLE";
    case 2: return "ONPROC";
    case 3: return "SLEEP";
    case 6: return "STOP";
    case 7: return "ZOMBIE";
    case 8: return "DEAD";
    default: return "?";
    }
}

std::optional<process_info> parse_process_info_ex2(const response& r) {
    const auto p = payload_view(r);
    if (p.size() < 28) return std::nullopt;
    process_info info;
    info.status                = read_be_u32(p, 0);
    info.ppu_thread_count      = read_be_u32(p, 4);
    info.spu_thread_count      = read_be_u32(p, 8);
    info.raw_spu_count         = read_be_u32(p, 12);
    info.parent_pid            = read_be_u32(p, 16);
    info.max_physical_mem_size = read_be_u64(p, 20);
    info.self_path             = read_cstr(p, 28);

    constexpr std::size_t path_slot      = 512;
    constexpr std::size_t guid_offset    = 28 + path_slot;
    constexpr std::size_t reserved_size  = 32;
    constexpr std::size_t flags_offset   = guid_offset + 32 + reserved_size;
    if (p.size() >= guid_offset + 32) {
        for (std::size_t i = 0; i < 32; ++i) {
            info.guid[i] = std::to_integer<std::uint8_t>(p[guid_offset + i]);
        }
    }
    if (p.size() >= flags_offset + 8) {
        info.flags       = read_be_u32(p, flags_offset);
        info.debug_flags = read_be_u32(p, flags_offset + 4);
    }
    return info;
}

std::optional<user_memory_stat> parse_user_memory_stat(const response& r) {
    const auto p = payload_view(r);
    if (p.size() < 28) return std::nullopt;
    user_memory_stat s;
    s.shared_created   = read_be_u32(p, 0);
    s.shared_attached  = read_be_u32(p, 4);
    s.local_memory     = read_be_u32(p, 8);
    s.local_text       = read_be_u32(p, 12);
    s.prx_text         = read_be_u32(p, 16);
    s.prx_data         = read_be_u32(p, 20);
    s.remain_memory    = read_be_u32(p, 24);
    return s;
}

std::vector<std::byte> build_prx_info_request_body(std::uint32_t handle) {
    std::vector<std::byte> body;
    body.reserve(4);
    append_be_u32(body, handle);
    return body;
}

std::optional<prx_info> parse_prx_info(const response& r) {
    const auto p = payload_view(r);

    constexpr std::size_t fixed_prefix = 0x238; // anything shorter is malformed, fixed prefix is 568 bytes
    if (p.size() < fixed_prefix) return std::nullopt;

    prx_info info;
    info.name = read_cstr(p, 0x008, 30);
    // high byte = major, low = minor
    info.version_major = std::to_integer<std::uint8_t>(p[0x026]);
    info.version_minor = std::to_integer<std::uint8_t>(p[0x027]);
    info.attribute     = read_be_u32(p, 0x028);
    info.start_entry   = read_be_u32(p, 0x02C);
    info.stop_entry    = read_be_u32(p, 0x030);
    info.path          = read_cstr(p, 0x034, 512);
    const auto seg_count = read_be_u32(p, 0x234);

    // segments live @ offset 0x238, 56 bytes each (7 x u64be)
    constexpr std::size_t seg_size = 56;
    for (std::uint32_t s = 0; s < seg_count; ++s) {
        const std::size_t off = 0x238 + s * seg_size;
        if (off + seg_size > p.size()) break;
        prx_segment seg;
        seg.base         = read_be_u64(p, off + 0);
        seg.file_size    = read_be_u64(p, off + 8);
        seg.mem_size     = read_be_u64(p, off + 16);
        seg.segment_num  = read_be_u64(p, off + 24);
        seg.segment_type = read_be_u64(p, off + 32);
        seg.attr_flags   = read_be_u64(p, off + 40);
        seg.align        = read_be_u64(p, off + 48);
        info.mem_size   += seg.mem_size;
        info.segments.push_back(seg);
    }
    return info;
}

std::vector<std::uint32_t> parse_prx_list(const response& r) {
    return parse_handle_list(r);
}

std::vector<std::uint32_t> parse_handle_list(const response& r) {
    std::vector<std::uint32_t> out;
    const auto p = payload_view(r);
    if (p.size() < 4) return out;
    const auto count = read_be_u32(p, 0);
    out.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::size_t off = 4 + i * 4;
        if (off + 4 > p.size()) break;
        out.push_back(read_be_u32(p, off));
    }
    return out;
}

std::vector<std::byte> build_handle_request_body(std::uint32_t handle) {
    std::vector<std::byte> body;
    body.reserve(4);
    append_be_u32(body, handle);
    return body;
}

std::optional<mutex_info> parse_mutex_info(const response& r) {
    const auto p = payload_view(r);
    // says the fixed prefix is 0x58 - 0x14 = 0x44 = 68 bytes after reponse header 
    
    // mtx name is at +0x34 (=0x20 into payload), 8 bytes ASCII nul padded
    if (p.size() < 0x44) return std::nullopt;
    mutex_info info;
    info.handle           = read_be_u32(p, 0x00);
    info.attr_protocol    = read_be_u32(p, 0x04);
    info.attr_recursive   = read_be_u32(p, 0x08);
    info.attr_shared      = read_be_u32(p, 0x0c);
    info.attr_adaptive    = read_be_u32(p, 0x10);
    info.key              = read_be_u64(p, 0x14);
    info.flags            = read_be_u32(p, 0x1c);
    info.name             = read_cstr(p, 0x20, 8);
    info.owner_thread_id    = read_be_u64(p, 0x28);
    info.lock_counter       = read_be_u32(p, 0x30);
    info.cond_ref_counter   = read_be_u32(p, 0x34);
    info.cond_var_id        = read_be_u32(p, 0x38);
    info.wait_thread_count  = read_be_u32(p, 0x3c);
    info.all_wait_count     = read_be_u32(p, 0x40);
    // List of wait PPU thread IDs follows as u64s.
    for (std::uint32_t i = 0; i < info.wait_thread_count; ++i) {
        const std::size_t off = 0x44 + i * 8;
        if (off + 8 > p.size()) break;
        info.wait_thread_ids.push_back(read_be_u64(p, off));
    }
    return info;
}

std::optional<lwmutex_info> parse_lwmutex_info(const response& r) {
    const auto p = payload_view(r);
    // layout of the abcd0001 records, offsets given relative to the handle (the 8 byte tag+length precedes it)
    //
    //   +0x00 u32 handle
    //   +0x04 u32 attr_protocol
    //   +0x08 u32 attr_recursive
    //   +0x0c 8B  lv2 sync names are 8 bytes, not 16
    //   +0x14 u32 reserved
    //   +0x18 u32 owner thread id  0xFFFFFFFF = never locked
    //   +0x1c u32 lock counter
    //   +0x20 u32 waiter count
    //   +0x24 ... u32 waiter ids
    //
    if (p.size() < 0x24) return std::nullopt;
    lwmutex_info info;
    info.handle          = read_be_u32(p, 0x00);
    info.attr_protocol   = read_be_u32(p, 0x04);
    info.attr_recursive  = read_be_u32(p, 0x08);
    info.name            = read_cstr(p, 0x0c, 8);
    info.owner_thread_id = read_be_u32(p, 0x18);
    info.lock_counter    = read_be_u32(p, 0x1c);
    const auto waiters   = read_be_u32(p, 0x20);
    for (std::uint32_t i = 0; i < waiters; ++i) {
        const std::size_t off = 0x24 + i * 4;
        if (off + 4 > p.size()) break;
        info.wait_thread_ids.push_back(read_be_u32(p, off));
    }
    return info;
}

std::optional<cond_info> parse_cond_info(const response& r) {
    const auto p = payload_view(r);
    // layout:
    //   +0x00 u32 handle
    //   +0x04 u32 attr (shared bit)
    //   +0x08 u64 key
    //   +0x10 u32 flags
    //   +0x14 8B  name (NUL-padded)
    //   +0x1c u32 mutex id
    //   +0x20 u32 wait count
    //   +0x24 ...  u64 wait thread ids
    if (p.size() < 0x24) return std::nullopt;
    cond_info info;
    info.handle      = read_be_u32(p, 0x00);
    info.attr_shared = read_be_u32(p, 0x04);
    info.key         = read_be_u64(p, 0x08);
    info.flags       = read_be_u32(p, 0x10);
    info.name        = read_cstr(p, 0x14, 8);
    info.mutex_id    = read_be_u32(p, 0x1c);
    const auto waiters = read_be_u32(p, 0x20);
    for (std::uint32_t i = 0; i < waiters; ++i) {
        const std::size_t off = 0x24 + i * 8;
        if (off + 8 > p.size()) break;
        info.wait_thread_ids.push_back(read_be_u64(p, off));
    }
    return info;
}

std::optional<event_queue_info> parse_event_queue_info(const response& r) {
    const auto p = payload_view(r);
    // abcd0003 / get_sync_primitives. The name comes before the key, not after it
    //   +0x00 u32 handle
    //   +0x04 u32 attr_protocol
    //   +0x08 u32 type
    //   +0x0c 8B  name (NUL-padded)
    //   +0x14 u64 key
    //   +0x1c u32 queue size
    //   +0x20 u32 queued count
    //   +0x24 u32 wait count
    //   +0x28 ...  u64 wait thread ids
    //
    if (p.size() < 0x28) return std::nullopt;
    event_queue_info info;
    info.handle        = read_be_u32(p, 0x00);
    info.attr_protocol = read_be_u32(p, 0x04);
    info.type          = read_be_u32(p, 0x08);
    info.name          = read_cstr(p, 0x0c, 8);
    info.key           = read_be_u64(p, 0x14);
    info.queue_size    = read_be_u32(p, 0x1c);
    info.queued_count  = read_be_u32(p, 0x20);
    const auto waiters = read_be_u32(p, 0x24);
    for (std::uint32_t i = 0; i < waiters; ++i) {
        const std::size_t off = 0x28 + i * 8;
        if (off + 8 > p.size()) break;
        info.wait_thread_ids.push_back(read_be_u64(p, off));
    }
    return info;
}

std::optional<std::vector<container_info>> parse_container_info(const response& r) {
    const auto p = payload_view(r);
    if (p.size() < 4) return std::nullopt;
    const auto count = read_be_u32(p, 0);
    std::vector<container_info> out;
    out.reserve(count);
    // percontainer 16-byte layout:
    //   +0x00 u32 total size of container? (bytes)
    //   +0x04 u32 available size in container (bytes)
    //   +0x08 u32 id
    //   +0x0C u32 parent container ID (0xFFFFFFFF if no parent)
    constexpr std::size_t entry_size = 16;
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::size_t off = 4 + i * entry_size;
        if (off + entry_size > p.size()) break;
        container_info ci;
        ci.total     = read_be_u32(p, off);
        ci.available = read_be_u32(p, off + 4);
        ci.id        = read_be_u32(p, off + 8);
        ci.parent_id = read_be_u32(p, off + 12);
        out.push_back(ci);
    }
    return out;
}

// debugger primitives

std::vector<std::byte> build_read_memory_request_body(std::uint64_t address, std::uint64_t length) {
    std::vector<std::byte> body;
    body.reserve(16);
    append_be_u64(body, address);
    append_be_u64(body, length);
    return body;
}

std::optional<memory_block> parse_read_memory(const response& r) {
    const auto p = payload_view(r);
    if (p.size() < 8) return std::nullopt;
    memory_block out;
    out.address = read_be_u64(p, 0);
    out.data.assign(p.begin() + 8, p.end());
    return out;
}

std::vector<std::byte> build_read_ppu_registers_request_body(
    std::uint64_t thread_id, const ppu_register_select& select)
{
    std::vector<std::byte> body;
    body.reserve(22);
    append_be_u64(body, thread_id);
    append_be_u32(body, select.gpr);
    append_be_u32(body, select.fpr);
    body.push_back(std::byte{static_cast<std::uint8_t>((select.spr >> 8) & 0xff)});
    body.push_back(std::byte{static_cast<std::uint8_t>(select.spr & 0xff)});
    append_be_u32(body, select.vmx);
    return body;
}

std::optional<ppu_registers> parse_ppu_registers(const response& r) {
    const auto p = payload_view(r);
    if (p.size() < ppu_registers_prologue + ppu_gpr_count * 8) return std::nullopt;
    ppu_registers out;
    out.thread_id = read_be_u64(p, 0);
    for (std::size_t i = 0; i < ppu_gpr_count; ++i) {
        out.gpr[i] = read_be_u64(p, ppu_registers_prologue + i * 8);
    }
    return out;
}

std::vector<std::byte> build_thread_id_list_body(std::span<const std::uint64_t> thread_ids) {
    std::vector<std::byte> body;
    body.reserve(thread_ids.size() * 8);
    for (auto id : thread_ids) append_be_u64(body, id);
    return body;
}

} // namespace opentm::tm_core::dbgp
