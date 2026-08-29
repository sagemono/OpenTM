#include "kernel_explorer_controller.h"

#include "session_controller.h"

#include <tm_core/tcp_connection.h>

#include <QStringLiteral>

namespace opentm::tm_ui {

namespace {

constexpr std::uint16_t kCatDbgp     = 0x0200;
constexpr std::uint32_t kEnvelopeSb  = 0x02100000;

} // namespace

kernel_explorer_controller::kernel_explorer_controller(
    opentm::tm_core::tcp_connection* conn,
    session_controller* session,
    QObject* parent)
    : QObject(parent), connection_(conn), session_(session)
{}

kernel_explorer_controller::~kernel_explorer_controller() = default;

std::uint32_t kernel_explorer_controller::send_dbgp(
    std::uint32_t cmd, std::uint32_t pid,
    const std::vector<std::byte>& body, std::uint32_t handle, std::uint32_t round)
{
    using namespace opentm::tm_core;
    if (!connection_ || !session_) return 0;

    const auto seq = session_->next_dbgshl_seq();
    pending_[seq] = pending{cmd, pid, handle, round};

    
    dbgp::request req;
    req.cmd        = cmd & ~dbgshl::reply_bit; 
    req.req_id     = seq;
    req.process_id = pid;
    req.payload    = body;
    const auto bytes = dbgp::encode_request(req);

    deci3_frame f;
    f.direction = deci3_direction::host_to_target;
    f.category  = kCatDbgp;
    
    
    f.session_b = session_->dbgshl_sb();
    f.payload.assign(bytes.begin(), bytes.end());

    if (!connection_->send_frame(f)) {
        pending_.remove(seq);
        return 0;
    }
    return seq;
}

void kernel_explorer_controller::refresh_process_list() {
    
    process_state_.summaries.clear();
    process_state_.remaining_pids.clear();
    process_state_.got_list = false;

    const auto seq = send_dbgp(opentm::tm_core::dbgshl::cmd::get_process_list, 0, {});
    if (seq == 0) {
        emit log_message(QStringLiteral("    !! kernel_explorer: process_list send failed"));
        return;
    }
    emit log_message(QStringLiteral("    <- KE: GET_PROCESS_LIST seq=0x%1").arg(seq, 8, 16, QChar('0')));
}

void kernel_explorer_controller::refresh_threads(std::uint32_t pid) {
    threads_state_.pid = pid;
    threads_state_.expected = 0;
    threads_state_.info.clear();
    send_dbgp(opentm::tm_core::dbgshl::cmd::get_thread_list, pid, {});
}

void kernel_explorer_controller::refresh_modules(std::uint32_t pid) {
    modules_state_.pid = pid;
    modules_state_.expected = 0;
    modules_state_.received = 0;
    modules_state_.info.clear();
    send_dbgp(opentm::tm_core::dbgshl::cmd::get_module_list, pid, {});
}

template <class Info>
void kernel_explorer_controller::refresh_sync(sync_accum<Info>& st, std::uint32_t pid, std::uint32_t list_cmd) {
    st.pid = pid;
    st.handles_remaining.clear();
    st.info.clear();
    send_dbgp(list_cmd, pid, {});
}

template <class Info>
void kernel_explorer_controller::on_sync_info(sync_accum<Info>& st, std::optional<Info> parsed, std::uint32_t handle, void (kernel_explorer_controller::*ready)(std::uint32_t, QList<Info>)) {
    if (parsed) st.info.push_back(*parsed);
    st.handles_remaining.removeAll(handle);
    if (st.handles_remaining.isEmpty()) {
        emit (this->*ready)(st.pid, st.info);
    }
}

void kernel_explorer_controller::refresh_mutexes(std::uint32_t pid) {
    refresh_sync(mutexes_state_, pid, opentm::tm_core::dbgshl::cmd::get_mutex_list);
}

void kernel_explorer_controller::refresh_lwmutexes(std::uint32_t pid) {
    refresh_sync(lwmutexes_state_, pid, opentm::tm_core::dbgshl::cmd::get_lwmutex_list);
}

void kernel_explorer_controller::refresh_conds(std::uint32_t pid) {
    refresh_sync(conds_state_, pid, opentm::tm_core::dbgshl::cmd::get_cond_list);
}

void kernel_explorer_controller::refresh_event_queues(std::uint32_t pid) {
    refresh_sync(evq_state_, pid, opentm::tm_core::dbgshl::cmd::get_event_queue_list);
}

void kernel_explorer_controller::refresh_containers(std::uint32_t pid) {
    send_dbgp(opentm::tm_core::dbgshl::cmd::get_container_info, pid, {});
}

void kernel_explorer_controller::trigger_core_dump(std::uint32_t pid) {
    if (pid == 0) return;
    send_dbgp(opentm::tm_core::dbgshl::cmd::trigger_core_dump, pid, {});
    emit log_message(QStringLiteral("    <- KE: TRIGGER_CORE_DUMP pid=0x%1").arg(pid, 8, 16, QChar('0')));
}

void kernel_explorer_controller::resume_process(std::uint32_t pid) {
    if (pid == 0) return;
    send_dbgp(opentm::tm_core::dbgshl::cmd::continue_process, pid, {});
    emit log_message(QStringLiteral("    <- KE: CONTINUE_PROCESS pid=0x%1").arg(pid, 8, 16, QChar('0')));
}

void kernel_explorer_controller::pause_process(std::uint32_t pid) {
    if (pid == 0) return;
    send_dbgp(opentm::tm_core::dbgshl::cmd::stop_process, pid, {});
    emit log_message(QStringLiteral("    <- KE: STOP_PROCESS pid=0x%1").arg(pid, 8, 16, QChar('0')));
}

void kernel_explorer_controller::terminate_process(std::uint32_t pid) {
    if (pid == 0) return;
    send_dbgp(opentm::tm_core::dbgshl::cmd::terminate_game_process, pid, {});
    emit log_message(QStringLiteral("    <- KE: TERMINATE_GAME_PROCESS pid=0x%1").arg(pid, 8, 16, QChar('0')));
}

void kernel_explorer_controller::on_frame_received(opentm::tm_core::deci3_frame f) {
    using namespace opentm::tm_core;
    if (f.category != kCatDbgp) return;
    if (f.session_b != kEnvelopeSb) return;
    if (f.payload.size() < dbgp::response_header_size) return;
    
    if ((std::to_integer<std::uint8_t>(f.payload[0]) & 0x80) == 0) return;

    const std::span<const std::byte> view(f.payload.data(), f.payload.size());
    const auto r = dbgp::decode_response(view);
    if (!r) return;

    const auto it = pending_.find(r->req_id);
    if (it == pending_.end()) {
        return;
    }
    const auto p = it.value();
    pending_.erase(it);

    using namespace opentm::tm_core::dbgshl;
    switch (p.cmd) {
    case cmd::get_process_list:           on_process_list_reply(*r); break;
    case cmd::get_process_info:  on_process_info_reply(*r, p.pid); break;
    case cmd::get_user_memory_stat: on_user_memory_stat_reply(*r, p.pid); break;
    case cmd::get_thread_list:            on_thread_list_reply(*r, p.pid); break;
    case cmd::get_ppu_thread_info:
        on_thread_info_reply(*r, p.pid, p.handle, p.round);
        break;
    case cmd::get_module_list: {
        const auto v = dbgp::parse_handle_list(*r);
        modules_state_.pid = p.pid;
        modules_state_.info.clear();
        modules_state_.expected = static_cast<int>(v.size());
        modules_state_.received = 0;
        if (modules_state_.expected == 0) {
            emit modules_ready(p.pid, {});
            break;
        }
        
        for (const auto h : v) {
            auto body = dbgp::build_prx_info_request_body(h);
            send_dbgp(cmd::get_module_info, p.pid, body, h);
        }
        break;
    }
    case cmd::get_module_info: {
        auto info = dbgp::parse_prx_info(*r);
        if (info) {
            info->handle = p.handle;
            modules_state_.info.push_back(*info);
        }
        ++modules_state_.received;
        if (modules_state_.received >= modules_state_.expected) {
            emit modules_ready(modules_state_.pid, modules_state_.info);
        }
        break;
    }
    case cmd::get_mutex_list:
        on_info_list_reply(*r, p.pid,
            cmd::get_mutex_info,
            mutexes_state_.handles_remaining, mutexes_state_.info);
        break;
    case cmd::get_mutex_info:
        on_sync_info(mutexes_state_, dbgp::parse_mutex_info(*r), p.handle, &kernel_explorer_controller::mutexes_ready);
        break;
    case cmd::get_lwmutex_list:
        on_info_list_reply(*r, p.pid,
            cmd::get_lwmutex_info,
            lwmutexes_state_.handles_remaining, lwmutexes_state_.info);
        break;
    case cmd::get_lwmutex_info:
        on_sync_info(lwmutexes_state_, dbgp::parse_lwmutex_info(*r), p.handle, &kernel_explorer_controller::lwmutexes_ready);
        break;
    case cmd::get_cond_list:
        on_info_list_reply(*r, p.pid,
            cmd::get_cond_info,
            conds_state_.handles_remaining, conds_state_.info);
        break;
    case cmd::get_cond_info:
        on_sync_info(conds_state_, dbgp::parse_cond_info(*r), p.handle, &kernel_explorer_controller::conds_ready);
        break;
    case cmd::get_event_queue_list:
        on_info_list_reply(*r, p.pid,
            cmd::get_event_queue_info,
            evq_state_.handles_remaining, evq_state_.info);
        break;
    case cmd::get_event_queue_info:
        on_sync_info(evq_state_, dbgp::parse_event_queue_info(*r), p.handle, &kernel_explorer_controller::event_queues_ready);
        break;
    case cmd::get_container_info:
        if (auto cs = dbgp::parse_container_info(*r)) {
            QList<dbgp::container_info> out;
            for (const auto& c : *cs) out.push_back(c);
            emit containers_ready(p.pid, out);
        }
        break;
    default:
        
        break;
    }
}

void kernel_explorer_controller::on_process_list_reply(
    const opentm::tm_core::dbgp::response& r)
{
    const auto pids = opentm::tm_core::dbgp::parse_process_list(r);
    process_state_.got_list = true;
    if (pids.empty()) {
        emit log_message(QStringLiteral("    -- KE: no running processes"));
        emit process_list_ready({});
        return;
    }
    emit log_message(QStringLiteral("    -- KE: %1 process(es) - fetching info").arg(pids.size()));
    for (const auto pid : pids) {
        process_summary entry;
        entry.pid = pid;
        process_state_.summaries.push_back(entry);
        process_state_.remaining_pids.push_back(pid);

        using namespace opentm::tm_core::dbgshl;
        send_dbgp(cmd::get_process_info,    pid, {});
        send_dbgp(cmd::get_user_memory_stat, pid, {});
    }
}

void kernel_explorer_controller::on_process_info_reply(
    const opentm::tm_core::dbgp::response& r, std::uint32_t pid)
{
    auto info = opentm::tm_core::dbgp::parse_process_info_ex2(r);
    if (!info) return;
    for (auto& s : process_state_.summaries) {
        if (s.pid == pid) { s.info = *info; break; }
    }
    maybe_emit_process_list();
}

void kernel_explorer_controller::on_user_memory_stat_reply(
    const opentm::tm_core::dbgp::response& r, std::uint32_t pid)
{
    if (auto stat = opentm::tm_core::dbgp::parse_user_memory_stat(r)) {
        for (auto& s : process_state_.summaries) {
            if (s.pid == pid) { s.memory = *stat; break; }
        }
    }
    
    process_state_.remaining_pids.removeAll(pid);
    maybe_emit_process_list();
}

void kernel_explorer_controller::maybe_emit_process_list() {
    if (!process_state_.got_list) return;
    if (!process_state_.remaining_pids.isEmpty()) return;
    emit process_list_ready(process_state_.summaries);
}

void kernel_explorer_controller::on_thread_list_reply(
    const opentm::tm_core::dbgp::response& r, std::uint32_t pid)
{
    const auto tl = opentm::tm_core::dbgp::parse_thread_list(r);
    if (!tl) {
        emit threads_ready(pid, {});
        return;
    }
    ++threads_state_.round;
    threads_state_.pid = pid;
    threads_state_.info.clear();
    threads_state_.expected = static_cast<int>(tl->ppu_thread_ids.size());
    threads_state_.received = 0;
    if (threads_state_.expected == 0) {
        emit threads_ready(pid, {});
        return;
    }
    for (const auto tid : tl->ppu_thread_ids) {
        auto body = opentm::tm_core::dbgp::build_ppu_thread_info_request_body(tid);
        send_dbgp(opentm::tm_core::dbgshl::cmd::get_ppu_thread_info, pid, body,
                  static_cast<std::uint32_t>(tid & 0xffffffffu), threads_state_.round);
    }
}

void kernel_explorer_controller::on_thread_info_reply(
    const opentm::tm_core::dbgp::response& r,
    std::uint32_t pid, std::uint64_t, std::uint32_t round)
{
    if (round != threads_state_.round) return;

    auto info = opentm::tm_core::dbgp::parse_ppu_thread_info(r);
    if (info) threads_state_.info.push_back(*info);
    ++threads_state_.received;

    if (threads_state_.received >= threads_state_.expected) {
        emit threads_ready(pid, threads_state_.info);
        ++threads_state_.round;
    }
}

template <class Info>
void kernel_explorer_controller::on_info_list_reply(
    const opentm::tm_core::dbgp::response& r,
    std::uint32_t pid, std::uint32_t info_cmd,
    QList<std::uint32_t>& expected_set, QList<Info>& accumulator)
{
    const auto handles = opentm::tm_core::dbgp::parse_handle_list(r);
    accumulator.clear();
    expected_set.clear();
    if (handles.empty()) {
        return;
    }
    for (const auto h : handles) expected_set.push_back(h);
    for (const auto h : handles) {
        auto body = opentm::tm_core::dbgp::build_handle_request_body(h);
        send_dbgp(info_cmd, pid, body, h);
    }
}

} // namespace opentm::tm_ui
