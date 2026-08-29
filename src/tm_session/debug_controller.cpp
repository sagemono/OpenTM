#include "debug_controller.h"

#include "session_controller.h"

#include <QStringList>

#include <tm_core/dbgshl_cmd.h>
#include <tm_core/tcp_connection.h>

#include <algorithm>

namespace opentm::tm_ui {

namespace {

constexpr std::uint16_t kCatDbgp = 0x0200;

} // namespace

debug_controller::debug_controller(opentm::tm_core::tcp_connection* conn, session_controller* session, QObject* parent)
    : QObject(parent), connection_(conn), session_(session)
{}

debug_controller::~debug_controller() = default;

QList<quint64> debug_controller::breakpoints() const {
    QList<quint64> out(breakpoints_.begin(), breakpoints_.end());
    std::sort(out.begin(), out.end());
    return out;
}

bool debug_controller::has_breakpoint(std::uint64_t address) const {
    return breakpoints_.contains(address);
}

std::uint32_t debug_controller::send_dbgp(std::uint32_t cmd, const std::vector<std::byte>& body, const pending& p)
{
    using namespace opentm::tm_core;
    if (!connection_ || !session_) return 0;

    const auto seq = session_->next_dbgshl_seq();

    dbgp::request req;
    req.cmd        = cmd & ~dbgshl::reply_bit;
    req.req_id     = seq;
    req.process_id = pid_;
    req.payload    = body;
    const auto bytes = dbgp::encode_request(req);

    deci3_frame f;
    f.direction = deci3_direction::host_to_target;
    f.category  = kCatDbgp;
    f.session_b = session_->dbgshl_sb();
    f.payload.assign(bytes.begin(), bytes.end());

    if (!connection_->send_frame(f)) return 0;
    pending_[seq] = p;
    return seq;
}

void debug_controller::set_process(std::uint32_t pid) {
    if (pid_ == pid) return;
    const auto was = pid_;
    pid_ = pid;
    forget_state();
    if (was != 0) {
        emit log_message(QStringLiteral("    -- process changed 0x%1 -> 0x%2, dropping breakpoints and thread state").arg(was, 0, 16).arg(pid, 0, 16));
    }
    emit process_changed(pid);
    attach_process();
}

void debug_controller::attach_process() {
    using namespace opentm::tm_core;
    if (pid_ == 0) return;
    // shaped like stop_process, which also carries only the pid in the header
    emit log_message(QStringLiteral("    <- attach process 0x%1").arg(pid_, 0, 16));
    send_dbgp(dbgshl::cmd::attach_process, {}, pending{kind::attach, 0, 0, 0});
}

void debug_controller::forget_state() {
    pending_.clear();
    reads_.clear();
    breakpoints_.clear();
    step_breakpoints_.clear();
    halted_threads_.clear();
    set_running(false);
}

void debug_controller::set_running(bool on) {
    if (running_ == on) return;
    running_ = on;
    emit running_changed(running_);
}

void debug_controller::read_memory(quint64 address, quint32 length) {
    using namespace opentm::tm_core;
    if (length == 0) return;

    const quint64 first = chunk_base(address);
    const quint64 last  = chunk_base(address + length - 1);

    const auto group = next_group_++;
    read_group g;
    g.address     = address;
    g.length      = length;
    g.outstanding = chunk_count(address, length);
    reads_[group] = g;

    for (quint64 at = first; at <= last; at += memory_chunk) {
        const auto body = dbgp::build_read_memory_request_body(at, memory_chunk);
        if (send_dbgp(dbgshl::cmd::read_memory, body, pending{kind::read_memory, at, 0, group}) == 0) {
            const auto it = reads_.find(group);
            if (it == reads_.end()) return;
            it.value().failed = true;  // the chunks that never went out will never answer
            it.value().outstanding = 0;
            finish_read_group(group);
            return;
        }
    }
}

void debug_controller::write_memory(quint64 address, QByteArray data) {
    using namespace opentm::tm_core;
    if (data.isEmpty()) return;
    const std::span<const std::byte> view(reinterpret_cast<const std::byte*>(data.constData()), static_cast<std::size_t>(data.size()));
    const auto body = dbgp::build_write_memory_body(address, view);
    send_dbgp(dbgshl::cmd::write_memory, body, pending{kind::write_memory, address, 0, 0});
}

void debug_controller::read_registers(quint64 thread_id) {
    using namespace opentm::tm_core;
    const auto body = dbgp::build_read_ppu_registers_request_body(thread_id);
    send_dbgp(dbgshl::cmd::read_ppu_registers, body, pending{kind::read_registers, 0, thread_id, 0});
}

void debug_controller::write_gpr(quint64 thread_id, unsigned index, quint64 value) {
    using namespace opentm::tm_core;
    if (index >= dbgp::ppu_gpr_count) return;
    const auto body = dbgp::build_write_ppu_gpr_body(thread_id, index, value);
    send_dbgp(dbgshl::cmd::write_ppu_registers, body, pending{kind::write_registers, 0, thread_id, 0});
}

void debug_controller::set_breakpoint(quint64 address) {
    using namespace opentm::tm_core;
    const auto body = dbgp::build_breakpoint_body(address);
    send_dbgp(dbgshl::cmd::set_ppu_breakpoint, body, pending{kind::set_breakpoint, address, 0, 0});
}

void debug_controller::clear_breakpoint(quint64 address) {
    using namespace opentm::tm_core;
    const auto body = dbgp::build_breakpoint_body(address);
    send_dbgp(dbgshl::cmd::clear_ppu_breakpoint, body, pending{kind::clear_breakpoint, address, 0, 0});
}

void debug_controller::resume(QList<quint64> thread_ids) {
    using namespace opentm::tm_core;
    if (thread_ids.isEmpty()) return;
    const std::vector<std::uint64_t> ids(thread_ids.begin(), thread_ids.end());
    const auto body = dbgp::build_thread_id_list_body(ids);
    if (send_dbgp(dbgshl::cmd::continue_ppu_thread, body, pending{kind::resume, 0, 0, 0}) != 0) {
        halted_threads_.clear();
        set_running(true);
    }
}

void debug_controller::halt(QList<quint64> thread_ids) {
    using namespace opentm::tm_core;
    if (thread_ids.isEmpty()) return;
    const std::vector<std::uint64_t> ids(thread_ids.begin(), thread_ids.end());
    const auto body = dbgp::build_thread_id_list_body(ids);
    if (send_dbgp(dbgshl::cmd::stop_ppu_thread, body, pending{kind::halt, 0, 0, 0}) != 0) {
        halted_threads_ = thread_ids;
    }
}

void debug_controller::step_to(quint64 thread_id, quint64 address) {
    step_to_any(thread_id, QList<quint64>{address});
}

void debug_controller::step_to_any(quint64 thread_id, QList<quint64> addresses) {
    using namespace opentm::tm_core;
    if (addresses.isEmpty()) return;

    // one request per address, the same way breakpoints are set
    int planted = 0;
    for (auto address : addresses) {
        const auto body = dbgp::build_step_body(address, thread_id);
        if (send_dbgp(dbgshl::cmd::step_ppu_thread, body, pending{kind::step, address, thread_id, 0}) == 0) {
            continue;
        }
        step_breakpoints_.insert(address);
        ++planted;
    }
    if (planted == 0) {
        emit log_message(QStringLiteral("    !! step: no breakpoint could be planted"));
        return;
    }

    QStringList where;
    for (auto a : addresses) where << QStringLiteral("0x%1").arg(a, 0, 16);
    emit log_message(QStringLiteral("    <- step thread 0x%1 to %2").arg(thread_id, 0, 16).arg(where.join(QStringLiteral(", "))));

    auto resume_ids = halted_threads_;
    if (resume_ids.isEmpty()) resume_ids.append(thread_id);
    if (!resume_ids.contains(thread_id)) resume_ids.append(thread_id);

    const std::vector<std::uint64_t> ids(resume_ids.begin(), resume_ids.end());
    const auto cont = dbgp::build_thread_id_list_body(ids);
    emit log_message(QStringLiteral("    <- continue %1 thread(s) for the step").arg(resume_ids.size()));
    if (send_dbgp(dbgshl::cmd::continue_ppu_thread, cont, pending{kind::resume, 0, thread_id, 0}) != 0) {
        set_running(true);
    }
}

void debug_controller::finish_read_group(std::uint32_t group) {
    const auto it = reads_.find(group);
    if (it == reads_.end()) return;
    const auto g = it.value();
    reads_.erase(it);

    if (g.failed) {
        emit memory_read_failed(g.address, 0);
        return;
    }

    QByteArray out;
    out.reserve(static_cast<int>(g.length));
    const quint64 first = chunk_base(g.address);
    for (quint64 at = first; out.size() < static_cast<int>(g.length); at += memory_chunk) {
        const auto chunk = g.chunks.value(at);
        if (chunk.isEmpty()) {
            emit memory_read_failed(g.address, 0);
            return;
        }
        const int skip = (at < g.address) ? static_cast<int>(g.address - at) : 0;
        if (skip >= chunk.size()) break;
        const int take = std::min<int>(chunk.size() - skip, static_cast<int>(g.length) - out.size());
        if (take <= 0) break;
        out.append(chunk.constData() + skip, take);
    }
    emit memory_ready(g.address, out);
}

void debug_controller::on_memory_chunk(const opentm::tm_core::dbgp::response& r, const pending& p)
{
    using namespace opentm::tm_core;
    const auto it = reads_.find(p.group);
    if (it == reads_.end()) return;
    auto& g = it.value();

    const auto block = dbgp::parse_read_memory(r);
    if (!block || r.result_code != 0) {
        g.failed = true;
    } else {
        g.chunks.insert(block->address, QByteArray(reinterpret_cast<const char*>(block->data.data()), static_cast<int>(block->data.size())));
    }
    if (--g.outstanding <= 0) finish_read_group(p.group);
}

void debug_controller::on_stop_event(const opentm::tm_core::dbgp::response& r) {
    using namespace opentm::tm_core;
    const auto ev = dbgp::parse_stop_event(r);
    if (!ev) return;

    if (!dbgp::is_ppu_thread_stop(ev->reason)) {
        emit log_message(QStringLiteral("    >> debug event reason 0x%1 (not a thread stop)").arg(ev->reason, 0, 16));
        return;
    }

    set_running(false);
    emit log_message(QStringLiteral("    >> stop event: thread 0x%1 at 0x%2 reason 0x%3").arg(ev->thread_id, 0, 16).arg(ev->address, 0, 16).arg(ev->reason, 0, 16));

    // a step plants a breakpoint at every place the thread could land, so clear all of them, not just the one that fired
    // the rest r live traps
    const auto planted = step_breakpoints_;
    step_breakpoints_.clear();
    for (auto address : planted) {
        send_dbgp(dbgshl::cmd::clear_ppu_breakpoint, dbgp::build_breakpoint_body(address), pending{kind::clear_step_breakpoint, address, 0, 0});
    }
    if (!planted.isEmpty()) {
        emit log_message(QStringLiteral("    <- cleared %1 step breakpoint(s)").arg(planted.size()));
    }

    emit thread_stopped(ev->thread_id, ev->address, ev->reason);
}

void debug_controller::on_frame_received(opentm::tm_core::deci3_frame f) {
    using namespace opentm::tm_core;
    if (f.category != kCatDbgp) return;
    if (!session_ || f.session_b != session_->dbgshl_sb()) return;
    if (f.payload.size() < dbgp::response_header_size) return;
    if ((std::to_integer<std::uint8_t>(f.payload[0]) & 0x80) == 0) return;

    const std::span<const std::byte> view(f.payload.data(), f.payload.size());
    const auto r = dbgp::decode_response(view);
    if (!r) return;

    if (r->cmd == dbgshl::cmd::stop_event) {
        on_stop_event(*r);
        return;
    }

    const auto it = pending_.find(r->req_id);
    if (it == pending_.end()) return;
    const auto p = it.value();
    pending_.erase(it);

    switch (p.what) {
    case kind::read_memory:
        on_memory_chunk(*r, p);
        break;
    case kind::write_memory:
        emit memory_written(p.address, r->result_code);
        break;
    case kind::read_registers: {
        const auto regs = dbgp::parse_ppu_registers(*r);
        if (regs) emit registers_ready(p.thread_id, *regs);
        break;
    }
    case kind::write_registers:
        emit registers_written(p.thread_id, r->result_code);
        break;
    case kind::set_breakpoint:
        if (r->result_code == 0) breakpoints_.insert(p.address);
        emit breakpoint_added(p.address, r->result_code);
        break;
    case kind::clear_breakpoint:
        if (r->result_code == 0) breakpoints_.remove(p.address);
        emit breakpoint_removed(p.address, r->result_code);
        break;
    case kind::clear_step_breakpoint:
        break;
    case kind::attach:
        emit log_message(r->result_code == 0 ? QStringLiteral("    >> attached to the process") : QStringLiteral("    !! attach refused (0x%1) - the body shape is probably wrong").arg(r->result_code, 8, 16, QChar('0')));
        break;
    case kind::resume:
        if (r->result_code != 0) {
            emit log_message(QStringLiteral("    !! continue refused (0x%1)").arg(r->result_code, 8, 16, QChar('0')));
            set_running(false);
            const auto stale = step_breakpoints_;
            step_breakpoints_.clear();
            for (auto address : stale) {
                emit log_message(QStringLiteral("    <- removing stranded step breakpoint at 0x%1").arg(address, 0, 16));
                send_dbgp(dbgshl::cmd::clear_ppu_breakpoint, dbgp::build_breakpoint_body(address), pending{kind::clear_step_breakpoint, address, 0, 0});
            }
        }
        break;
    case kind::halt:
        if (r->result_code == 0) set_running(false);
        emit halt_finished(r->result_code);
        break;
    case kind::step:
        emit log_message(QStringLiteral("    >> step breakpoint at 0x%1 %2").arg(p.address, 0, 16).arg(r->result_code == 0 ? QStringLiteral("planted") : QStringLiteral("REFUSED (0x%1)").arg(r->result_code, 8, 16, QChar('0'))));
        break;
    }
}

} // namespace opentm::tm_ui
