//
//   req header (16 bytes):
//     +0x00 4B  cmd          (request command id)
//     +0x04 4B  req_id       (echoed back in reply)
//     +0x08 4B  data_len     (size of payload in bytes)
//     +0x0C 4B  process_id   (target PID is zero for whole system queries)
//   res header (20 bytes):
//     +0x00 4B  cmd          (0x80000000 | request cmd)
//     +0x04 4B  req_id       (echoed)
//     +0x08 4B  data_len     (size of payload that follows)
//     +0x0C 4B  process_id
//     +0x10 4B  result_code  (0 = OK)
//

#pragma once

#include "dbgshl_cmd.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace opentm::tm_core::dbgp {

inline constexpr std::size_t request_header_size  = 16;
inline constexpr std::size_t response_header_size = 20;

//framing

struct request {
    std::uint32_t cmd        = 0;
    std::uint32_t req_id     = 0;
    std::uint32_t process_id = 0;
    std::vector<std::byte> payload;
};

struct response {
    std::uint32_t cmd         = 0;
    std::uint32_t req_id      = 0;
    std::uint32_t data_len    = 0;
    std::uint32_t process_id  = 0;
    std::uint32_t result_code = 0;
    std::vector<std::byte> payload; // = exactly data_len bytes
};

std::vector<std::byte> encode_request(const request& r);
std::optional<response> decode_response(std::span<const std::byte> bytes);

std::vector<std::uint32_t> parse_process_list(const response& r);

// res payload:
//   +0x00 u32  PPU thread count (n)
//   +0x04 u32  SPU thread group count (m)
//   +0x08 ...  N x u64 ppu thread ids
//   ...        M x u32 spu thread group ids
struct thread_list {
    std::vector<std::uint64_t> ppu_thread_ids;
    std::vector<std::uint32_t> spu_thread_group_ids;
};
std::optional<thread_list> parse_thread_list(const response& r);

// res payload:
//   +0x00 u64  ppu thread id
//   +0x08 u32  priority
//   +0x0C u32  state (0=IDLE 1=RUNNABLE 2=ONPROC 3=SLEEP 6=STOP 7=ZOMBIE 8=DEAD)
//   +0x10 u64  stack addr
//   +0x18 u64  stack size
//   +0x20 u32  base priority
//   +0x24 ...  nul terminated thread name (0 - 128 bytes)
struct ppu_thread_info {
    std::uint64_t thread_id     = 0;
    std::uint32_t priority      = 0;
    std::uint32_t state         = 0;
    std::uint64_t stack_addr    = 0;
    std::uint64_t stack_size    = 0;
    std::uint32_t base_priority = 0;
    std::string   name;
};
std::vector<std::byte> build_ppu_thread_info_request_body(std::uint64_t tid);
std::optional<ppu_thread_info> parse_ppu_thread_info(const response& r);

const char* ppu_thread_state_name(std::uint32_t state) noexcept;

// req payload empty, header PID=<target>.
// res payload
//   +0x00 u32  status
//   +0x04 u32  ppu thread count
//   +0x08 u32  spu thread count
//   +0x0C u32  raw spu count
//   +0x10 u32  parent pid
//   +0x14 u64  max physical memory size
//   +0x1C ...  nul terminated self path (0-512 bytes)
//   variable 32B ppu guid
//   variable 32B reserved
//   variable 4B  flags about following info (0x00000001 = valid)
//   variable 4B  debug flags
//   variable 136B debug param

struct process_info {
    std::uint32_t status                 = 0;
    std::uint32_t ppu_thread_count       = 0;
    std::uint32_t spu_thread_count       = 0;
    std::uint32_t raw_spu_count          = 0;
    std::uint32_t parent_pid             = 0;
    std::uint64_t max_physical_mem_size  = 0;
    std::string   self_path;
    std::array<std::uint8_t, 32> guid    = {};
    std::uint32_t flags                  = 0;
    std::uint32_t debug_flags            = 0;
};
std::optional<process_info> parse_process_info_ex2(const response& r);

// req payload empty, header PID=<target>.
// res payload (28 bytes):
//   +0x00 u32  created shared memory size
//   +0x04 u32  attached shared memory size
//   +0x08 u32  process local memory size
//   +0x0C u32  process local text size
//   +0x10 u32  text size of prx
//   +0x14 u32  data size of prx
//   +0x18 u32  remaining memory size
struct user_memory_stat {
    std::uint32_t shared_created   = 0;
    std::uint32_t shared_attached  = 0;
    std::uint32_t local_memory     = 0;
    std::uint32_t local_text       = 0;
    std::uint32_t prx_text         = 0;
    std::uint32_t prx_data         = 0;
    std::uint32_t remain_memory    = 0;
};
std::optional<user_memory_stat> parse_user_memory_stat(const response& r);

// req payload: u32 module handle at +0x00.
// res layout
//   +0x000  8B   sizeof module
//   +0x008 30B   name (nul pad ascii)
//   +0x026  2B   ver (high byte = major, low = minor)
//   +0x028  4B   attr
//   +0x02C  4B   start entry
//   +0x030  4B   stop entry
//   +0x034 512B  program#m filename
//   +0x234  4B   no of segments
//   +0x238  ...  segmenets, each 56 bytes = 7 x u64: base, file_size, mem_size, seg_num, seg_type, attr_flags, align
struct prx_segment {
    std::uint64_t base        = 0;
    std::uint64_t file_size   = 0;
    std::uint64_t mem_size    = 0;
    std::uint64_t segment_num = 0; // 0=text, 1=data/bss, 2&3=rodata
    std::uint64_t segment_type = 0;
    std::uint64_t attr_flags  = 0;
    std::uint64_t align       = 0;
};
struct prx_info {
    std::uint32_t handle      = 0;
    std::uint64_t mem_size    = 0;
    std::uint8_t  version_major = 0;
    std::uint8_t  version_minor = 0;
    std::uint32_t attribute   = 0;
    std::uint32_t start_entry = 0;
    std::uint32_t stop_entry  = 0;
    std::string   name;
    std::string   path;
    std::vector<prx_segment> segments;
};
std::vector<std::byte> build_prx_info_request_body(std::uint32_t handle);
std::optional<prx_info> parse_prx_info(const response& r);

// res: u32 count, then count * u32 handles.
std::vector<std::uint32_t> parse_prx_list(const response& r);

std::vector<std::uint32_t> parse_handle_list(const response& r);
std::vector<std::byte> build_handle_request_body(std::uint32_t handle);

struct mutex_info {
    std::uint32_t handle             = 0;
    std::uint32_t attr_protocol      = 0; // 1=FIFO, 2=PRIORITY, etc
    std::uint32_t attr_recursive     = 0; // 0x10=RECURSIVE 0x20=NOT_RECURSIVE
    std::uint32_t attr_shared        = 0;
    std::uint32_t attr_adaptive      = 0;
    std::uint64_t key                = 0;
    std::uint32_t flags              = 0;
    std::string   name;              // 8 bytes per spec but ascii / nul pad
    std::uint64_t owner_thread_id    = 0;
    std::uint32_t lock_counter       = 0;
    std::uint32_t cond_ref_counter   = 0;
    std::uint32_t cond_var_id        = 0;
    std::uint32_t wait_thread_count  = 0;
    std::uint32_t all_wait_count     = 0;
    std::vector<std::uint64_t> wait_thread_ids;
};
std::optional<mutex_info> parse_mutex_info(const response& r);

struct lwmutex_info {
    std::uint32_t handle          = 0;
    std::uint32_t attr_protocol   = 0;
    std::uint32_t attr_recursive  = 0;
    std::string   name;
    std::uint32_t owner_thread_id = 0;
    std::uint32_t lock_counter    = 0;
    std::vector<std::uint32_t> wait_thread_ids;
};
std::optional<lwmutex_info> parse_lwmutex_info(const response& r);

struct cond_info {
    std::uint32_t handle       = 0;
    std::uint32_t attr_shared  = 0;
    std::uint64_t key          = 0;
    std::uint32_t flags        = 0;
    std::string   name;
    std::uint32_t mutex_id     = 0;
    std::vector<std::uint64_t> wait_thread_ids;
};
std::optional<cond_info> parse_cond_info(const response& r);

struct event_queue_info {
    std::uint32_t handle        = 0;
    std::uint32_t attr_protocol = 0;
    std::uint32_t type          = 0; // SYS_PPU_QUEUE / SYS_SPU_QUEUE
    std::uint64_t key           = 0;
    std::uint32_t queue_size    = 0;
    std::string   name;
    std::uint32_t queued_count  = 0;
    std::vector<std::uint64_t> wait_thread_ids;
};
std::optional<event_queue_info> parse_event_queue_info(const response& r);

struct container_info {
    std::uint32_t id        = 0;
    std::uint32_t parent_id = 0; // 0xFFFFFFFF / sentinel = none
    std::uint32_t total     = 0; // bytes
    std::uint32_t available = 0;
};
std::optional<std::vector<container_info>> parse_container_info(const response& r);

// debugger primitives

// read_memory (0x300): address then length, both u64. The reply echoes the
// address before the bytes, so payload = 8 + length.
std::vector<std::byte> build_read_memory_request_body(std::uint64_t address, std::uint64_t length);

struct memory_block {
    std::uint64_t          address = 0;
    std::vector<std::byte> data;
};
std::optional<memory_block> parse_read_memory(const response& r);

// read_ppu_registers (0x400): thread id then four per-class selectors, whose
// widths say what each covers - 32 GPRs, 32 FPRs, the special registers, 32
// vector registers. Note the u16 in the middle; the body is 22 bytes, not 24.
struct ppu_register_select {
    std::uint32_t gpr = 0xffffffffu;
    std::uint32_t fpr = 0xffffffffu;
    std::uint16_t spr = 0x00ffu;
    std::uint32_t vmx = 0xffffffffu;
};

std::vector<std::byte> build_read_ppu_registers_request_body(
    std::uint64_t thread_id, const ppu_register_select& select = {});

inline constexpr std::size_t ppu_gpr_count = 32;
inline constexpr std::size_t ppu_fpr_count = 32;
// the reply repeats the selectors before the register file
inline constexpr std::size_t ppu_registers_prologue = 8 + 28;
// the special registers sit between the FPRs and the vector file, not at the end
inline constexpr std::size_t ppu_special_offset =
    ppu_registers_prologue + (ppu_gpr_count + ppu_fpr_count) * 8;

struct ppu_registers {
    std::uint64_t thread_id = 0;
    std::array<std::uint64_t, ppu_gpr_count> gpr{};
    std::array<std::uint64_t, ppu_fpr_count> fpr{};
    std::uint64_t pc  = 0;
    std::uint32_t cr  = 0;
    std::uint64_t lr  = 0;
    std::uint64_t ctr = 0;
};
std::optional<ppu_registers> parse_ppu_registers(const response& r);

// stop_ppu_thread (0x204) and continue_ppu_thread (0x200): the process id rides
// in the frame header, so the body carries only the thread ids.
std::vector<std::byte> build_thread_id_list_body(std::span<const std::uint64_t> thread_ids);

// set_ppu_breakpoint (0x502) and clear_ppu_breakpoint (0x503) both take a bare u64 address and echo it back
std::vector<std::byte> build_breakpoint_body(std::uint64_t address);
std::optional<std::uint64_t> parse_breakpoint_reply(const response& r);

// step_ppu_thread (0x200012): a temporary breakpoint at the address the thread should stop on next.
std::vector<std::byte> build_step_body(std::uint64_t address, std::uint64_t thread_id);

// stop_event (0x80000b00): unsolicited, sent when a thread stops. req_id is 0
struct stop_event {
    std::uint32_t reason    = 0;
    std::uint64_t thread_id = 0;
    std::uint64_t address   = 0;
    std::uint64_t stack_pointer = 0;
};
inline constexpr std::uint32_t stop_reason_breakpoint = 0x10u;
std::optional<stop_event> parse_stop_event(const response& r);

} // namespace opentm::tm_core::dbgp
