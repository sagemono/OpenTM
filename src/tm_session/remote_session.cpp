#include "remote_session.h"

#include "load_options_codec.h"
#include "target_record_codec.h"

#include <tm_core/target_type.h>

#include <QJsonArray>

namespace opentm::tm_ui {

namespace {

using namespace opentm::tm_core;

std::uint64_t num(const QJsonValue& v) {
    return v.isString() ? v.toString().toULongLong(nullptr, 0) : static_cast<std::uint64_t>(v.toDouble());
}

std::string str(const QJsonValue& v) { return v.toString().toStdString(); }

dbgp::ppu_thread_info thread_from(const QJsonObject& o) {
    dbgp::ppu_thread_info t;
    t.thread_id     = num(o.value("id"));
    t.name          = str(o.value("name"));
    t.priority      = static_cast<std::uint32_t>(num(o.value("priority")));
    t.base_priority = static_cast<std::uint32_t>(num(o.value("base_priority")));
    t.state         = static_cast<std::uint32_t>(num(o.value("state")));
    t.stack_addr    = num(o.value("stack_addr"));
    t.stack_size    = num(o.value("stack_size"));
    return t;
}

dbgp::prx_info module_from(const QJsonObject& o) {
    dbgp::prx_info m;
    m.handle   = static_cast<std::uint32_t>(num(o.value("id")));
    m.name     = str(o.value("name"));
    m.path     = str(o.value("path"));
    m.mem_size = num(o.value("mem_size"));
    const auto ver = o.value("version").toString().split(QLatin1Char('.'));
    if (ver.size() == 2) {
        m.version_major = static_cast<std::uint8_t>(ver[0].toUInt());
        m.version_minor = static_cast<std::uint8_t>(ver[1].toUInt());
    }
    for (const auto& sv : o.value("segments").toArray()) {
        const auto so = sv.toObject();
        dbgp::prx_segment s;
        s.segment_num = num(so.value("index"));
        s.base        = num(so.value("base"));
        s.mem_size    = num(so.value("mem_size"));
        s.file_size   = num(so.value("file_size"));
        s.attr_flags  = num(so.value("flags"));
        m.segments.push_back(s);
    }
    return m;
}

dbgp::mutex_info mutex_from(const QJsonObject& o) {
    dbgp::mutex_info e;
    e.handle          = static_cast<std::uint32_t>(num(o.value("id")));
    e.name            = str(o.value("name"));
    e.owner_thread_id = num(o.value("owner"));
    e.lock_counter    = static_cast<std::uint32_t>(num(o.value("lock_count")));
    e.attr_protocol   = static_cast<std::uint32_t>(num(o.value("protocol")));
    e.attr_recursive  = static_cast<std::uint32_t>(num(o.value("recursive")));
    return e;
}

dbgp::lwmutex_info lwmutex_from(const QJsonObject& o) {
    dbgp::lwmutex_info e;
    e.handle          = static_cast<std::uint32_t>(num(o.value("id")));
    e.name            = str(o.value("name"));
    e.owner_thread_id = static_cast<std::uint32_t>(num(o.value("owner")));
    e.lock_counter    = static_cast<std::uint32_t>(num(o.value("lock_count")));
    e.attr_protocol   = static_cast<std::uint32_t>(num(o.value("protocol")));
    e.attr_recursive  = static_cast<std::uint32_t>(num(o.value("recursive")));
    return e;
}

dbgp::cond_info cond_from(const QJsonObject& o) {
    dbgp::cond_info e;
    e.handle   = static_cast<std::uint32_t>(num(o.value("id")));
    e.name     = str(o.value("name"));
    e.mutex_id = static_cast<std::uint32_t>(num(o.value("mutex_id")));
    return e;
}

dbgp::event_queue_info evq_from(const QJsonObject& o) {
    dbgp::event_queue_info e;
    e.handle       = static_cast<std::uint32_t>(num(o.value("id")));
    e.name         = str(o.value("name"));
    e.key          = num(o.value("key"));
    e.queue_size   = static_cast<std::uint32_t>(num(o.value("size")));
    e.queued_count = static_cast<std::uint32_t>(num(o.value("queued")));
    return e;
}

dbgp::container_info container_from(const QJsonObject& o) {
    dbgp::container_info e;
    e.id        = static_cast<std::uint32_t>(num(o.value("id")));
    e.parent_id = static_cast<std::uint32_t>(num(o.value("parent")));
    e.total     = static_cast<std::uint32_t>(num(o.value("total")));
    e.available = static_cast<std::uint32_t>(num(o.value("available")));
    return e;
}

quint64 hex_value(const QJsonValue& v) {
    auto text = v.toString().trimmed();
    if (text.isEmpty()) return 0;
    if (text.startsWith(QLatin1String("0x"), Qt::CaseInsensitive)) text = text.mid(2);
    bool ok = false;
    const auto out = text.toULongLong(&ok, 16);
    return ok ? out : 0;
}

template <typename T, typename Fn>
QList<T> list_from(const QJsonArray& arr, Fn conv) {
    QList<T> out;
    out.reserve(arr.size());
    for (const auto& v : arr) out.push_back(conv(v.toObject()));
    return out;
}

} // namespace

remote_session::remote_session(QObject* parent) : session_api(parent) {
    wire_events();
}

remote_session::~remote_session() = default;

void remote_session::wire_events() {
    connect(&rpc_, &rpc_client::event_received, this, &remote_session::on_event);
    connect(&rpc_, &rpc_client::protocol_error, this, &session_api::error);
    connect(&rpc_, &rpc_client::disconnected, this, [this] {
        const auto why = server_gone_reason_.isEmpty() ? tr("session server connection lost") : server_gone_reason_;
        connected_ = session_ready_ = false;
        // without this the indicator keeps whatever state it last saw and the target reads as stuck mid connect
        using state = opentm::tm_core::tcp_connection::state;
        if (state_ != state::disconnected) {
            state_ = state::disconnected;
            emit connection_state_changed(state_);
        }
        emit status_message(why);
        emit log_message(QStringLiteral("    !! %1").arg(why));
        emit session_invalidated();
        emit server_lost(why);
    });
}

bool remote_session::attach_tcp(const QString& host, quint16 port) {
    return rpc_.connect_tcp(host, port);
}

bool remote_session::attach_local(const QString& name) {
    return rpc_.connect_local(name);
}

void remote_session::refresh_status() {
    if (handle_.isEmpty()) return;
    rpc_.call(QStringLiteral("target.status"), {{"target", handle_}},
              [this](bool ok, const QJsonObject& r, const QString&) {
        if (!ok) return;
        record_.host   = r.value("host").toString();
        record_.port   = static_cast<quint16>(r.value("port").toInt());
        record_.file_server_dir = r.value("serve_dir").toString();
        if (const auto t = target_type_from_string(r.value("type").toString().toStdString())) {
            record_.type = *t;
        }
        peer_ = QStringLiteral("%1:%2").arg(record_.host).arg(record_.port);
        adopt_state(r);
    });
}

void remote_session::adopt_state(const QJsonObject& r) {
    const bool was_ready = session_ready_;
    connected_     = r.value("connected").toBool(r.value("already_connected").toBool());
    session_ready_ = r.value("session_ready").toBool();

    using state = opentm::tm_core::tcp_connection::state;
    const auto st = static_cast<state>(r.value("state").toInt(
        static_cast<int>(connected_ ? state::ready : state::disconnected)));
    if (st != state_) {
        state_ = st;
        emit connection_state_changed(state_);
    }
    if (session_ready_ && !was_ready) {
        emit session_ready(0, 0);   
        emit debug_agent_ready(); 
    }
}

void remote_session::send(const QString& method, QJsonObject params) {
    if (!handle_.isEmpty()) params["target"] = handle_;
    rpc_.call(method, params,
              [this, method](bool ok, const QJsonObject&, const QString& err) {
        if (!ok) emit error(QStringLiteral("%1: %2").arg(method, err));
    });
}

void remote_session::set_target(const target_record& r) {
    record_ = r;
    handle_ = r.id.isEmpty() ? (r.name.isEmpty() ? QStringLiteral("%1:%2").arg(r.host).arg(r.port) : r.name) : r.id;
    // the whole record goes over: the server drives resets, WoL and file
    // serving from its own copy, so a partial one silently loses those settings
    rpc_.call(QStringLiteral("target.open"), {{"record", target_record_to_json(r)}},
              [this](bool ok, const QJsonObject& reply, const QString& err) {
        if (!ok) {
            emit error(QStringLiteral("target.open: %1").arg(err));
            return;
        }
        //multiinstancing betwen debugger
        rpc_.call(QStringLiteral("server.version"), {},
                  [this](bool vok, const QJsonObject& v, const QString&) {
            if (!vok) {
                emit log_message(QStringLiteral("    !! this session server is older than this client. It will not share a console between apps. Restart it with 'opentm_dbg --restart-server', or quit the OpenTM tray."));
                return;
            }
            emit log_message(QStringLiteral("    -- session server built %1%2").arg(v.value("build").toString(), v.value("debugger").toBool() ? QString() : QStringLiteral(" (no debugger support)")));
        });

        const auto given = reply.value("target").toString();
        if (!given.isEmpty() && given != handle_) {
            emit log_message(QStringLiteral("    -- attached to session '%1'").arg(given));
            handle_ = given;
        }
        refresh_status();
    });
}

void remote_session::clear_target() {
    record_      = {};
    handle_.clear();
    connected_ = session_ready_ = false;
}

void remote_session::set_file_serving_dir(const QString& dir) {
    record_.file_server_dir = dir;
    send(QStringLiteral("target.serve_dir"), {{"dir", dir}});
}

void remote_session::connect_to_target() {
    connected_ = true;
    state_     = opentm::tm_core::tcp_connection::state::tcp_connecting;
    rpc_.call(QStringLiteral("target.connect"), {{"target", handle_}}, [this](bool ok, const QJsonObject& r, const QString& err) {
        if (!ok) {
            connected_ = false;
            state_     = opentm::tm_core::tcp_connection::state::disconnected;
            emit connection_state_changed(state_);
            emit error(QStringLiteral("target.connect: %1").arg(err));
            return;
        }
        if (r.value("already_connected").toBool()) {
            emit log_message(QStringLiteral("    -- session for '%1' was already open on the server").arg(handle_));
        }
        adopt_state(r);
    });
}

void remote_session::disconnect_from_target() {
    connected_ = session_ready_ = false;
    send(QStringLiteral("target.disconnect"));
}

void remote_session::power_on()        { send(QStringLiteral("target.power_on")); }
void remote_session::power_off()       { send(QStringLiteral("target.power_off")); }
void remote_session::power_off_force() { send(QStringLiteral("target.power_kill")); }
void remote_session::wake_on_lan()     { send(QStringLiteral("target.wake_on_lan")); }
void remote_session::reset_current()   {
    send(QStringLiteral("target.reset"), {{"mode", QStringLiteral("current")}});
}

void remote_session::load_executable(const QString& path, const load_options& o) {
    send(QStringLiteral("process.load"),
         {{"path", path}, {"options", load_options_to_json(o)}});
}

void remote_session::install_package(const QString& host_path) {
    send(QStringLiteral("package.install"), {{"path", host_path}});
}

void remote_session::settings_refresh() { send(QStringLiteral("settings.refresh")); }
void remote_session::settings_commit()  { send(QStringLiteral("settings.commit")); }
void remote_session::settings_apply(const QString& host_path, std::uint32_t size) {
    send(QStringLiteral("settings.apply"), {{"path", host_path}, {"size", static_cast<qint64>(size)}});
}

void remote_session::list_directory(const QString& path) {
    send(QStringLiteral("fs.list"), {{"path", path}});
}

void remote_session::refresh_lpar_status()  { send(QStringLiteral("target.lpar_status")); }
void remote_session::download_file(const QString& kit_path, const QString& host_path, std::uint32_t size) {
    send(QStringLiteral("fs.download"), {{"path", kit_path}, {"to", host_path}, {"size", static_cast<qint64>(size)}});
}
void remote_session::upload_file(const QString& host_path, const QString& kit_path) {
    send(QStringLiteral("fs.upload"), {{"path", host_path}, {"to", kit_path}});
}
void remote_session::delete_file(const QString& kit_path) {
    send(QStringLiteral("fs.delete"), {{"path", kit_path}});
}

void remote_session::rename_file(const QString& from, const QString& to) {
    send(QStringLiteral("fs.rename"), {{"from", from}, {"to", to}});
}

void remote_session::make_directory(const QString& kit_path, std::uint32_t mode) {
    send(QStringLiteral("fs.mkdir"), {{"path", kit_path}, {"mode", QString::number(mode)}});
}

void remote_session::close_target() {
    send(QStringLiteral("target.close"));
    disconnect_from_target();
}

void remote_session::set_permissions(const QString& kit_path, std::uint32_t mode) {
    send(QStringLiteral("fs.chmod"), {{"path", kit_path}, {"mode", QString::number(mode)}});
}

void remote_session::set_times(const QString& kit_path, std::uint64_t atime, std::uint64_t mtime) {
    send(QStringLiteral("fs.utime"), {{"path", kit_path}, {"atime", QString::number(atime)}, {"mtime", QString::number(mtime)}});
}

void remote_session::refresh_process_list() { send(QStringLiteral("process.list")); }
void remote_session::refresh_threads(std::uint32_t pid)       { send(QStringLiteral("process.threads"),       {{"pid", QString::number(pid)}}); }
void remote_session::refresh_modules(std::uint32_t pid)       { send(QStringLiteral("process.modules"),       {{"pid", QString::number(pid)}}); }
void remote_session::refresh_mutexes(std::uint32_t pid)       { send(QStringLiteral("process.mutexes"),       {{"pid", QString::number(pid)}}); }
void remote_session::refresh_lwmutexes(std::uint32_t pid)     { send(QStringLiteral("process.lwmutexes"),     {{"pid", QString::number(pid)}}); }
void remote_session::refresh_conds(std::uint32_t pid)         { send(QStringLiteral("process.conds"),         {{"pid", QString::number(pid)}}); }
void remote_session::refresh_event_queues(std::uint32_t pid)  { send(QStringLiteral("process.event_queues"),  {{"pid", QString::number(pid)}}); }
void remote_session::refresh_containers(std::uint32_t pid)    { send(QStringLiteral("process.containers"),    {{"pid", QString::number(pid)}}); }
void remote_session::pause_process(std::uint32_t pid)     { send(QStringLiteral("process.pause"),     {{"pid", QString::number(pid)}}); }
void remote_session::resume_process(std::uint32_t pid)    { send(QStringLiteral("process.resume"),    {{"pid", QString::number(pid)}}); }
void remote_session::terminate_process(std::uint32_t pid) { send(QStringLiteral("process.terminate"), {{"pid", QString::number(pid)}}); }
void remote_session::trigger_core_dump(std::uint32_t pid) { send(QStringLiteral("process.core_dump"), {{"pid", QString::number(pid)}}); }

void remote_session::on_event(const QString& target, const QString& name, const QJsonObject& p)
{
    if (name == QLatin1String("server_shutdown")) {
        // arrives just before the socket closes; remembered so the disconnect
        // handler can say why instead of guessing
        server_gone_reason_ = p.value("reason").toString(tr("session server shut down"));
        return;
    }
    if (!target.isEmpty() && target != handle_) return;
    if (name == QLatin1String("target_renamed")) {
        record_.name = p.value("name").toString();
        return;
    }
    if (name == QLatin1String("log")) {
        emit log_message(p.value("line").toString());
    } else if (name == QLatin1String("wire")) {
        emit wire_line(p.value("line").toString());
    } else if (name == QLatin1String("status")) {
        emit status_message(p.value("text").toString());
    } else if (name == QLatin1String("tty")) {
        emit tty_text(p.value("text").toString());
    } else if (name == QLatin1String("tty_stream")) {
        emit tty_stream_text(static_cast<std::uint8_t>(p.value("stream").toInt()), p.value("text").toString());
    } else if (name == QLatin1String("clear_console")) {
        emit clear_console();
    } else if (name == QLatin1String("debug_agent_ready")) {
        emit debug_agent_ready();
    } else if (name == QLatin1String("boot_mode")) {
        emit boot_mode_changed(p.value("mode").toString());
    } else if (name == QLatin1String("sdk_version")) {
        emit sdk_version_received(p.value("version").toString());
    } else if (name == QLatin1String("cp_version")) {
        emit cp_version_received(p.value("version").toString());
    } else if (name == QLatin1String("target_down")) {
        emit target_went_down(p.value("reason").toString());
    } else if (name == QLatin1String("transfer_finished")) {
        emit transfer_finished();
    } else if (name == QLatin1String("transfer_failed")) {
        emit transfer_failed(static_cast<std::uint32_t>(num(p.value("result"))));
    } else if (name == QLatin1String("install_reply")) {
        emit install_reply(static_cast<std::uint32_t>(num(p.value("status"))));
    } else if (name == QLatin1String("debug_memory")) {
        emit debug_memory_ready(hex_value(p.value("address")), QByteArray::fromHex(p.value("data").toString().toLatin1()));
    } else if (name == QLatin1String("debug_memory_failed")) {
        emit debug_memory_read_failed(hex_value(p.value("address")), static_cast<quint32>(num(p.value("status"))));
    } else if (name == QLatin1String("debug_memory_written")) {
        emit debug_memory_written(hex_value(p.value("address")), static_cast<quint32>(num(p.value("status"))));
    } else if (name == QLatin1String("debug_registers")) {
        opentm::tm_core::dbgp::ppu_registers regs;
        regs.thread_id = hex_value(p.value("thread"));
        regs.pc  = hex_value(p.value("pc"));
        regs.lr  = hex_value(p.value("lr"));
        regs.ctr = hex_value(p.value("ctr"));
        regs.cr  = static_cast<std::uint32_t>(hex_value(p.value("cr")));
        const auto gprs = p.value("gpr").toArray();
        for (int i = 0; i < gprs.size() && i < static_cast<int>(regs.gpr.size()); ++i) {
            regs.gpr[static_cast<std::size_t>(i)] = hex_value(gprs.at(i));
        }
        emit debug_registers_ready(regs.thread_id, regs);
    } else if (name == QLatin1String("debug_registers_written")) {
        emit debug_registers_written(hex_value(p.value("thread")), static_cast<quint32>(num(p.value("status"))));
    } else if (name == QLatin1String("debug_breakpoint_added")) {
        emit debug_breakpoint_added(hex_value(p.value("address")), static_cast<quint32>(num(p.value("status"))));
    } else if (name == QLatin1String("debug_breakpoint_removed")) {
        emit debug_breakpoint_removed(hex_value(p.value("address")), static_cast<quint32>(num(p.value("status"))));
    } else if (name == QLatin1String("debug_stopped")) {
        emit debug_thread_stopped(hex_value(p.value("thread")), hex_value(p.value("address")), static_cast<quint32>(num(p.value("reason"))));
    } else if (name == QLatin1String("debug_running")) {
        emit debug_running_changed(p.value("running").toBool());
    } else if (name == QLatin1String("debug_halted")) {
        emit debug_halt_finished(static_cast<quint32>(num(p.value("status"))));
    } else if (name == QLatin1String("debug_process")) {
        emit debug_process_changed(static_cast<quint32>(num(p.value("pid"))));
    } else if (name == QLatin1String("conn_state")) {
        using state = opentm::tm_core::tcp_connection::state;
        state_ = static_cast<state>(p.value("state").toInt());
        connected_ = (state_ != state::disconnected && state_ != state::error_state);
        if (!connected_) session_ready_ = false;
        emit connection_state_changed(state_);
    } else if (name == QLatin1String("error")) {
        emit error(p.value("message").toString());
    } else if (name == QLatin1String("session_ready")) {
        session_ready_ = connected_ = true;
        refresh_status();
        emit session_ready(static_cast<std::uint16_t>(p.value("token").toInt()), static_cast<std::uint16_t>(p.value("sub_token").toInt()));
    } else if (name == QLatin1String("session_lost")) {
        session_ready_ = false;
        emit session_invalidated();
    } else if (name == QLatin1String("load_reply")) {
        emit load_ext_reply(static_cast<std::uint32_t>(num(p.value("status"))));
    } else if (name == QLatin1String("install_progress")) {
        emit install_progress(p.value("percent").toInt());
    } else if (name == QLatin1String("install_finished")) {
        emit install_finished(p.value("path").toString());
    } else if (name == QLatin1String("directory")) {
        std::vector<dfmp_file_entry> entries;
        for (const auto& v : p.value("entries").toArray()) {
            const auto o = v.toObject();
            dfmp_file_entry e;
            e.name = str(o.value("name"));
            e.size = num(o.value("size"));
            e.mode = static_cast<std::uint32_t>(num(o.value("mode")));
            e.ctime = static_cast<std::uint32_t>(num(o.value("ctime")));
            e.atime = static_cast<std::uint32_t>(num(o.value("atime")));
            e.mtime = static_cast<std::uint32_t>(num(o.value("mtime")));
            entries.push_back(e);
        }
        emit directory_listed(p.value("path").toString(), std::move(entries));
    } else if (name == QLatin1String("processes")) {
        QList<process_summary> out;
        for (const auto& v : p.value("processes").toArray()) {
            const auto o = v.toObject();
            process_summary s;
            s.pid = static_cast<std::uint32_t>(num(o.value("pid")));
            s.info.self_path        = str(o.value("path"));
            s.info.ppu_thread_count = static_cast<std::uint32_t>(num(o.value("ppu_threads")));
            s.info.spu_thread_count = static_cast<std::uint32_t>(num(o.value("spu_threads")));
            s.memory.local_memory   = static_cast<std::uint32_t>(num(o.value("local_memory")));
            s.memory.remain_memory  = static_cast<std::uint32_t>(num(o.value("remain_memory")));
            out.push_back(s);
        }
        emit process_list_ready(out);
    } else if (name == QLatin1String("objects")) {
        const auto pid   = static_cast<std::uint32_t>(num(p.value("pid")));
        const auto kind  = p.value("kind").toString();
        const auto items = p.value("items").toArray();
        if      (kind == QLatin1String("threads"))      emit threads_ready(pid, list_from<dbgp::ppu_thread_info>(items, thread_from));
        else if (kind == QLatin1String("modules"))      emit modules_ready(pid, list_from<dbgp::prx_info>(items, module_from));
        else if (kind == QLatin1String("mutexes"))      emit mutexes_ready(pid, list_from<dbgp::mutex_info>(items, mutex_from));
        else if (kind == QLatin1String("lwmutexes"))    emit lwmutexes_ready(pid, list_from<dbgp::lwmutex_info>(items, lwmutex_from));
        else if (kind == QLatin1String("conds"))        emit conds_ready(pid, list_from<dbgp::cond_info>(items, cond_from));
        else if (kind == QLatin1String("event_queues")) emit event_queues_ready(pid, list_from<dbgp::event_queue_info>(items, evq_from));
        else if (kind == QLatin1String("containers"))   emit containers_ready(pid, list_from<dbgp::container_info>(items, container_from));
    }
}

namespace {
QString hex_of(quint64 v) { return QStringLiteral("0x%1").arg(v, 0, 16); }
QJsonArray hex_list(const QList<quint64>& vs) {
    QJsonArray a;
    for (auto v : vs) a.append(hex_of(v));
    return a;
}
} // namespace

void remote_session::debug_read_memory(quint64 address, quint32 length) {
    send(QStringLiteral("debug.read_memory"), {{"address", hex_of(address)}, {"length", static_cast<int>(length)}});
}

void remote_session::debug_write_memory(quint64 address, QByteArray data) {
    send(QStringLiteral("debug.write_memory"), {{"address", hex_of(address)}, {"data", QString::fromLatin1(data.toHex())}});
}

void remote_session::debug_read_registers(quint64 thread_id) {
    send(QStringLiteral("debug.read_registers"), {{"thread", hex_of(thread_id)}});
}

void remote_session::debug_write_gpr(quint64 thread_id, unsigned index, quint64 value) {
    send(QStringLiteral("debug.write_gpr"), {{"thread", hex_of(thread_id)}, {"index", static_cast<int>(index)}, {"value", hex_of(value)}});
}

void remote_session::debug_set_breakpoint(quint64 address) {
    send(QStringLiteral("debug.set_breakpoint"), {{"address", hex_of(address)}});
}

void remote_session::debug_clear_breakpoint(quint64 address) {
    send(QStringLiteral("debug.clear_breakpoint"), {{"address", hex_of(address)}});
}

void remote_session::debug_resume(QList<quint64> thread_ids) {
    send(QStringLiteral("debug.resume"), {{"threads", hex_list(thread_ids)}});
}

void remote_session::debug_halt(QList<quint64> thread_ids) {
    send(QStringLiteral("debug.halt"), {{"threads", hex_list(thread_ids)}});
}

void remote_session::debug_step_to(quint64 thread_id, QList<quint64> addresses) {
    send(QStringLiteral("debug.step"), {{"thread", hex_of(thread_id)}, {"addresses", hex_list(addresses)}});
}

void remote_session::debug_set_process(std::uint32_t pid) {
    send(QStringLiteral("debug.attach"), {{"pid", QString::number(pid)}});
}

} // namespace opentm::tm_ui
