#pragma once

#include <cstddef>
#include <cstdint>

namespace opentm::tm_core::dbgshl {

inline constexpr std::uint32_t reply_bit = 0x80000000u;

constexpr std::uint32_t reply_of(std::uint32_t request) noexcept {
    return request | reply_bit;
}

constexpr std::uint32_t request_of(std::uint32_t reply) noexcept {
    return reply & ~reply_bit;
}

namespace cmd {

// thread control
inline constexpr std::uint32_t load                   = 0x80000000u;
inline constexpr std::uint32_t load_ext               = 0x80000004u;
inline constexpr std::uint32_t get_process_list       = 0x80000102u;
inline constexpr std::uint32_t get_thread_list        = 0x80000103u;
inline constexpr std::uint32_t continue_process       = 0x80000202u;
inline constexpr std::uint32_t stop_process           = 0x80000203u;
inline constexpr std::uint32_t terminate_game_process = 0x80000209u;
inline constexpr std::uint32_t trigger_core_dump      = 0x8000020au;

// lv2 kernel queries
inline constexpr std::uint32_t get_module_list        = 0x80000108u;
inline constexpr std::uint32_t get_module_info        = 0x80000109u;
inline constexpr std::uint32_t get_mutex_list         = 0x80000110u;
inline constexpr std::uint32_t get_mutex_info         = 0x80000111u;
inline constexpr std::uint32_t get_cond_list          = 0x80000112u;
inline constexpr std::uint32_t get_cond_info          = 0x80000113u;
inline constexpr std::uint32_t get_lwmutex_list       = 0x80000116u;
inline constexpr std::uint32_t get_lwmutex_info       = 0x80000117u;
inline constexpr std::uint32_t get_event_queue_list   = 0x80000118u;
inline constexpr std::uint32_t get_event_queue_info   = 0x80000119u;
inline constexpr std::uint32_t get_container_info     = 0x8000011eu;
inline constexpr std::uint32_t get_ppu_thread_info    = 0x8000011fu;
inline constexpr std::uint32_t get_process_info       = 0x80000132u;

// sync primitives
inline constexpr std::uint32_t get_sync_primitives    = 0x8020000du;
inline constexpr std::uint32_t get_user_memory_stat   = 0x80200008u;

// storage / fs
inline constexpr std::uint32_t install_package        = 0x80000802u;
inline constexpr std::uint32_t format_hdd             = 0x80000804u;
inline constexpr std::uint32_t create_settings_file   = 0x80000810u;

// these are not currently implemented, TODO: wire up to opentm debugger

// memory and registers
inline constexpr std::uint32_t read_memory            = 0x80000300u;
inline constexpr std::uint32_t write_memory           = 0x80000301u;
inline constexpr std::uint32_t read_spu_local_store   = 0x80000302u;
inline constexpr std::uint32_t write_spu_local_store  = 0x80000303u;
inline constexpr std::uint32_t write_memory_scattered = 0x80000304u;
inline constexpr std::uint32_t read_ppu_registers     = 0x80000400u;
inline constexpr std::uint32_t write_ppu_registers    = 0x80000401u;
inline constexpr std::uint32_t read_spu_registers     = 0x80000402u;
inline constexpr std::uint32_t write_spu_registers    = 0x80000403u;
inline constexpr std::uint32_t read_memory_compressed   = 0x8020000eu;
inline constexpr std::uint32_t read_memory64_compressed = 0x80200011u;

// bp / wp
inline constexpr std::uint32_t list_ppu_breakpoints   = 0x80000500u;
inline constexpr std::uint32_t list_spu_breakpoints   = 0x80000501u;
inline constexpr std::uint32_t set_ppu_breakpoint     = 0x80000502u;
inline constexpr std::uint32_t clear_ppu_breakpoint   = 0x80000503u;
inline constexpr std::uint32_t set_spu_breakpoint     = 0x80000504u;
inline constexpr std::uint32_t clear_spu_breakpoint   = 0x80000505u;
inline constexpr std::uint32_t set_spu_loop           = 0x80000509u;
inline constexpr std::uint32_t clear_spu_loop         = 0x8000050au;
inline constexpr std::uint32_t get_data_watchpoint    = 0x80200009u;
inline constexpr std::uint32_t set_data_watchpoint    = 0x8020000au;
// stepping is a temporary breakpoint the agent clears when it fires
inline constexpr std::uint32_t step_ppu_thread        = 0x80200012u;
inline constexpr std::uint32_t stop_event             = 0x80000b00u;

// extra thread control
inline constexpr std::uint32_t continue_ppu_thread    = 0x80000200u;
inline constexpr std::uint32_t continue_spu_group     = 0x80000201u;
inline constexpr std::uint32_t stop_ppu_thread        = 0x80000204u;
inline constexpr std::uint32_t stop_spu_group         = 0x80000205u;
inline constexpr std::uint32_t kill_process           = 0x80000206u;
inline constexpr std::uint32_t suspend_ppu_thread     = 0x8000020cu;
inline constexpr std::uint32_t resume_ppu_thread      = 0x8000020du;
inline constexpr std::uint32_t clear_exception_thread = 0x8000020bu;
inline constexpr std::uint32_t set_debug_thread_control = 0x80000207u;
inline constexpr std::uint32_t get_debug_thread_control = 0x80000208u;

// obj queries
inline constexpr std::uint32_t attach_process          = 0x80000002u;
inline constexpr std::uint32_t get_agent_version       = 0x800000ffu;
inline constexpr std::uint32_t get_sdk_version         = 0x80002000u;
inline constexpr std::uint32_t get_spu_group_info      = 0x80000105u;
inline constexpr std::uint32_t get_spu_thread_info     = 0x80000107u;
inline constexpr std::uint32_t get_overlay_list        = 0x8000010au;
inline constexpr std::uint32_t get_overlay_info        = 0x8000010bu;
inline constexpr std::uint32_t get_vm_info             = 0x8000010cu;
inline constexpr std::uint32_t get_prx_info            = 0x8000010eu;
inline constexpr std::uint32_t get_vram_info           = 0x8000010fu;
inline constexpr std::uint32_t get_rwlock_list         = 0x80000114u;
inline constexpr std::uint32_t get_rwlock_info         = 0x80000115u;
inline constexpr std::uint32_t get_semaphore_list      = 0x8000011au;
inline constexpr std::uint32_t get_semaphore_info      = 0x8000011bu;
inline constexpr std::uint32_t get_lwcond_list         = 0x8000011cu;
inline constexpr std::uint32_t get_lwcond_info         = 0x8000011du;
inline constexpr std::uint32_t get_eventflag_list      = 0x80000120u;
inline constexpr std::uint32_t get_eventflag_info      = 0x80000121u;
inline constexpr std::uint32_t get_kernel_thread_list  = 0x80000130u;
inline constexpr std::uint32_t get_kernel_thread_info  = 0x80000131u;
inline constexpr std::uint32_t get_sync_primitive_counts = 0x8020000cu;
inline constexpr std::uint32_t get_status_tree         = 0x80200000u;
inline constexpr std::uint32_t get_system_info         = 0x80200005u;
inline constexpr std::uint32_t get_ip_info             = 0x80200010u;

// spu / modules
inline constexpr std::uint32_t set_raw_spu_data        = 0x80200001u;
inline constexpr std::uint32_t raw_spu_command         = 0x80200002u;
inline constexpr std::uint32_t raw_spu_notify          = 0x80200003u;
inline constexpr std::uint32_t scan_spu_thread_modules = 0x80200004u;
inline constexpr std::uint32_t scan_spurs_modules      = 0x8020000bu;
inline constexpr std::uint32_t init_scratch_area       = 0x80200006u;
inline constexpr std::uint32_t term_scratch_area       = 0x80200007u;

//prof
inline constexpr std::uint32_t trace_create            = 0x80200100u;
inline constexpr std::uint32_t trace_get_buffer_info   = 0x80200101u;
inline constexpr std::uint32_t trace_read_buffer       = 0x80200102u;
inline constexpr std::uint32_t trace_destroy_all       = 0x80200103u;

// fs
inline constexpr std::uint32_t create_game_dir         = 0x80000800u;
inline constexpr std::uint32_t remove_game_dir         = 0x80000801u;
inline constexpr std::uint32_t filetrace_start         = 0x80000710u;

} // namespace cmd

//unsolicited target->host notifications.
//
//   header:  [ucmd][seq 0][data_len][source]
//   data:    [status][event][event specific payload...]
namespace notify {

inline constexpr std::uint32_t ucmd = 0x80000b00u;
inline constexpr std::uint32_t source_agent   = 0x00000000u;
inline constexpr std::uint32_t source_install = 0x01000300u;
inline constexpr std::uint32_t source_process = 0x01010200u;
inline constexpr std::uint32_t event_agent_up        = 0x00000060u;
inline constexpr std::uint32_t event_install_percent = 0x00000080u;
inline constexpr std::uint32_t event_install_done    = 0x00000081u;

// bytes offsets into the payload
inline constexpr std::size_t source_offset = 12;
inline constexpr std::size_t event_offset  = 20;
inline constexpr std::size_t data_offset   = 24;

} // namespace notify
} // namespace opentm::tm_core::dbgshl
