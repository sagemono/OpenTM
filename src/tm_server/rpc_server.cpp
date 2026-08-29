#include "rpc_server.h"

#include <tm_session/load_options_codec.h>
#include <tm_session/target_record_codec.h>

#include <tm_core/target_type.h>

#include <QCoreApplication>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalServer>
#include <QLocalSocket>
#include <QTcpServer>
#include <QTcpSocket>

#include <utility>

namespace opentm::tm_server {

namespace {

using namespace opentm::tm_core;

QString cstr(const std::string& s) { return QString::fromUtf8(s.c_str()); }
QString hex(quint64 v)             { return QStringLiteral("0x%1").arg(v, 0, 16); }

std::uint32_t pid_of(const QJsonObject& p) {
    const auto v = p.value("pid");
    return v.isString() ? static_cast<std::uint32_t>(v.toString().toULong(nullptr, 0)) : static_cast<std::uint32_t>(v.toDouble());
}

QJsonObject to_json(const dfmp_file_entry& e) {
    return {{"name", cstr(e.name)}, {"dir", e.is_directory()}, {"size", static_cast<qint64>(e.size)}, {"mode", static_cast<qint64>(e.mode)}, {"ctime", static_cast<qint64>(e.ctime)}, {"atime", static_cast<qint64>(e.atime)}, {"mtime", static_cast<qint64>(e.mtime)}};
}

QJsonObject to_json(const dbgp::ppu_thread_info& t) {
    return {{"id", hex(t.thread_id)}, {"name", cstr(t.name)}, {"priority", static_cast<qint64>(t.priority)}, {"base_priority", static_cast<qint64>(t.base_priority)}, {"state", static_cast<qint64>(t.state)}, {"state_name", QString::fromUtf8(dbgp::ppu_thread_state_name(t.state))}, {"stack_addr", hex(t.stack_addr)}, {"stack_size", static_cast<qint64>(t.stack_size)}};
}

QJsonObject to_json(const dbgp::prx_info& m) {
    QJsonArray segs;
    for (const auto& s : m.segments) {
        segs.append(QJsonObject{{"index", static_cast<qint64>(s.segment_num)}, {"base", hex(s.base)}, {"mem_size", static_cast<qint64>(s.mem_size)}, {"file_size", static_cast<qint64>(s.file_size)}, {"flags", hex(s.attr_flags)}});
    }
    return {{"id", hex(m.handle)}, {"name", cstr(m.name)}, {"path", cstr(m.path)}, {"mem_size", static_cast<qint64>(m.mem_size)}, {"version", QStringLiteral("%1.%2").arg(m.version_major).arg(m.version_minor)}, {"segments", segs}};
}

QJsonObject to_json(const dbgp::mutex_info& e) {
    return {{"id", hex(e.handle)}, {"name", cstr(e.name)}, {"owner", hex(e.owner_thread_id)}, {"lock_count", static_cast<qint64>(e.lock_counter)}, {"protocol", static_cast<qint64>(e.attr_protocol)}, {"recursive", static_cast<qint64>(e.attr_recursive)}};
}

QJsonObject to_json(const dbgp::lwmutex_info& e) {
    return {{"id", hex(e.handle)}, {"name", cstr(e.name)}, {"owner", hex(e.owner_thread_id)}, {"lock_count", static_cast<qint64>(e.lock_counter)}, {"protocol", static_cast<qint64>(e.attr_protocol)}, {"recursive", static_cast<qint64>(e.attr_recursive)}};
}

QJsonObject to_json(const dbgp::cond_info& e) {
    return {{"id", hex(e.handle)}, {"name", cstr(e.name)}, {"mutex_id", hex(e.mutex_id)}};
}

QJsonObject to_json(const dbgp::event_queue_info& e) {
    return {{"id", hex(e.handle)}, {"name", cstr(e.name)}, {"key", hex(e.key)}, {"size", static_cast<qint64>(e.queue_size)}, {"queued", static_cast<qint64>(e.queued_count)}};
}

QJsonObject to_json(const dbgp::container_info& e) {
    return {{"id", hex(e.id)}, {"parent", hex(e.parent_id)}, {"total", static_cast<qint64>(e.total)}, {"available", static_cast<qint64>(e.available)}};
}

template <typename T>
QJsonArray array_of(const QList<T>& xs) {
    QJsonArray a;
    for (const auto& x : xs) a.append(to_json(x));
    return a;
}

} // namespace

QJsonObject rpc_server::status_json(const managed* m) {
    const auto& r  = m->session->target();
    const auto  sv = target_type_string(r.type);
    return QJsonObject{
        {"target", m->handle}, {"name", m->name},
        {"host", r.host}, {"port", r.port},
        {"type", QString::fromLatin1(sv.data(), static_cast<int>(sv.size()))},
        {"serve_dir", r.file_server_dir},
        {"connected", m->session->is_connected()},
        {"session_ready", m->session->is_session_ready()},
        {"state", static_cast<int>(m->session->connection_state())},
        {"tty_cursor", m->tty.size()}};
}

rpc_server::rpc_server(QObject* parent) : QObject(parent) {
    register_methods();
    if (auto* app = QCoreApplication::instance()) {
        connect(app, &QCoreApplication::aboutToQuit, this, [this] {
            announce_shutdown(QStringLiteral("server is shutting down"));
        });
    }
}

void rpc_server::announce_shutdown(const QString& reason) {
    if (announced_) return;
    announced_ = true;
    const QJsonObject o{{"event", QStringLiteral("server_shutdown")}, {"params", QJsonObject{{"reason", reason}}}};
    const auto line = QJsonDocument(o).toJson(QJsonDocument::Compact) + '\n';
    for (auto* c : clients_) {
        if (!c || !c->isWritable()) continue;
        c->write(line);
        if (auto* t = qobject_cast<QTcpSocket*>(c)) {
            t->flush();
            t->waitForBytesWritten(200);
        } else if (auto* l = qobject_cast<QLocalSocket*>(c)) {
            l->flush();
            l->waitForBytesWritten(200);
        }
    }
}

rpc_server::~rpc_server() {
    for (auto* m : sessions_) delete m;
}

// session maanagement
rpc_server::managed* rpc_server::open_target(const QJsonObject& p, QString& err) {
    // the gui client sends the whole record under "record" 
    // adhoc callers send the handful of flat keys below
    auto r = opentm::tm_ui::target_record_from_json(p.value("record").toObject());
    if (p.contains("id"))        r.id   = p.value("id").toString();
    if (p.contains("target") || p.contains("name")) {
        r.name = p.value("target").toString(p.value("name").toString());
    }
    if (p.contains("host"))      r.host = p.value("host").toString();
    if (p.contains("serve_dir")) r.file_server_dir = p.value("serve_dir").toString();

    if (p.contains("type")) {
        const auto t = target_type_from_string(p.value("type").toString().toStdString());
        if (!t) { err = QStringLiteral("unknown target type"); return nullptr; }
        r.type = *t;
    }
    if (p.contains("port")) {
        r.port = static_cast<quint16>(p.value("port").toInt());
    } else if (!p.contains("record")) {
        r.port = (r.type == target_type::cfw_dex) ? 1000 : 8530;
    }

    if (r.host.isEmpty()) { err = QStringLiteral("host required"); return nullptr; }
    if (r.name.isEmpty()) r.name = QStringLiteral("%1:%2").arg(r.host).arg(r.port);

    // clients that track their own identity (via the GUI) pass 'id'
    // adhoc callers such as the CLI get the name as their handle
    const QString handle = r.id.isEmpty() ? r.name : r.id;

    if (auto it = sessions_.constFind(handle); it != sessions_.constEnd()) {
        auto* have_m = *it;
        const auto& have = have_m->session->target();
        if (have.host != r.host || have.port != r.port || have.type != r.type) {
            err = QStringLiteral("target '%1' is already open on %2:%3").arg(have_m->name, have.host).arg(have.port);
            return nullptr;
        }
        if (p.contains("record")) {
            // re opening with a full record is how the GUI pushes edited
            // properties (reset mode, MAC, timeouts) into a live session
            have_m->session->set_target(r);
        } else if (!r.file_server_dir.isEmpty() && r.file_server_dir != have.file_server_dir) {
            have_m->session->set_file_serving_dir(r.file_server_dir);
        }
        if (have_m->name != r.name) {
            const auto was = have_m->name;
            have_m->name = r.name;
            emit log_message(QStringLiteral("target '%1' renamed to '%2'").arg(was, r.name));
            broadcast_event(handle, QStringLiteral("target_renamed"), {{"name", r.name}});
        }
        return have_m;
    }

    for (auto* have_m : std::as_const(sessions_)) {
        const auto& have = have_m->session->target();
        if (have.host == r.host && have.port == r.port) {
            emit log_message(QStringLiteral("'%1' is already open as '%2' - attaching to that session").arg(handle, have_m->handle));
            return have_m;
        }
    }

    auto* m = new managed;
    m->handle  = handle;
    m->name    = r.name;
    m->session = new opentm::tm_ui::target_session(this);
    sessions_.insert(handle, m);
    wire_session_events(m);
    m->session->set_target(r);

    emit log_message(QStringLiteral("opened target '%1' (%2:%3)").arg(r.name, r.host).arg(r.port));
    broadcast_event(handle, QStringLiteral("target_opened"), {{"name", r.name}, {"host", r.host}, {"port", r.port}});
    return m;
}

void rpc_server::close_target(const QString& handle) {
    auto it = sessions_.find(handle);
    if (it == sessions_.end()) return;
    auto* m = *it;
    const auto name = m->name;
    sessions_.erase(it);
    m->session->disconnect_from_target();
    m->session->deleteLater();
    delete m;
    emit log_message(QStringLiteral("closed target '%1'").arg(name));
    broadcast_event(handle, QStringLiteral("target_closed"), {});
}

rpc_server::managed* rpc_server::resolve_target(const QJsonObject& p, QString& err) {
    const auto want = p.value("target").toString();
    if (!want.isEmpty()) {
        if (const auto it = sessions_.constFind(want); it != sessions_.constEnd()) {
            return *it;
        }
        // handle miss: fall back to the display name so the cli  stays usable
        for (auto* m : sessions_) {
            if (m->name == want) return m;
        }
        err = QStringLiteral("no open target named '%1'").arg(want);
        return nullptr;
    }

    if (sessions_.size() == 1) return *sessions_.begin();
    err = sessions_.isEmpty() ? QStringLiteral("no targets open - call target.open first") : QStringLiteral("several targets open; name one with \"target\"");
    return nullptr;
}

void rpc_server::wire_session_events(managed* m) {
    using ts  = opentm::tm_ui::target_session;
    using api = opentm::tm_ui::session_api;
    auto* s = m->session;
    // events carry the stable handle, so a rename never has to rewire these
    const QString tag = m->handle;

    connect(s, &ts::log_message, this, [this, tag](const QString& l) {
        emit log_message(QStringLiteral("[%1] %2").arg(tag, l));
        broadcast_event(tag, QStringLiteral("log"), {{"line", l}});
    });
    connect(s, &ts::tty_text, this, [this, m, tag](const QString& t) {
        for (const auto& l : t.split('\n', Qt::SkipEmptyParts)) {
            m->tty.append(l.trimmed());
        }
        while (m->tty.size() > kMaxTtyLines) m->tty.removeFirst();
        broadcast_event(tag, QStringLiteral("tty"), {{"text", t}});
    });
    connect(s, &api::connection_state_changed, this, [this, tag](opentm::tm_core::tcp_connection::state st) {
        broadcast_event(tag, QStringLiteral("conn_state"), {{"state", static_cast<int>(st)}});
    });
    connect(s, &api::status_message, this, [this, tag](const QString& t) {
        broadcast_event(tag, QStringLiteral("status"), {{"text", t}});
    });
    connect(s, &api::wire_line, this, [this, tag](const QString& l) {
        broadcast_event(tag, QStringLiteral("wire"), {{"line", l}});
    });
    connect(s, &api::tty_stream_text, this, [this, tag](std::uint8_t stream, const QString& t) {
        broadcast_event(tag, QStringLiteral("tty_stream"), {{"stream", stream}, {"text", t}});
    });
    connect(s, &api::clear_console, this, [this, tag] {
        broadcast_event(tag, QStringLiteral("clear_console"), {});
    });
    connect(s, &api::debug_agent_ready, this, [this, tag] {
        broadcast_event(tag, QStringLiteral("debug_agent_ready"), {});
    });
    connect(s, &api::boot_mode_changed, this, [this, tag](const QString& v) {
        broadcast_event(tag, QStringLiteral("boot_mode"), {{"mode", v}});
    });
    connect(s, &api::sdk_version_received, this, [this, tag](const QString& v) {
        broadcast_event(tag, QStringLiteral("sdk_version"), {{"version", v}});
    });
    connect(s, &api::cp_version_received, this, [this, tag](const QString& v) {
        broadcast_event(tag, QStringLiteral("cp_version"), {{"version", v}});
    });
    connect(s, &api::target_went_down, this, [this, tag](const QString& why) {
        broadcast_event(tag, QStringLiteral("target_down"), {{"reason", why}});
    });

    connect(s, &api::debug_memory_ready, this, [this, tag](quint64 a, QByteArray d) {
        broadcast_event(tag, QStringLiteral("debug_memory"), {{"address", hex(a)}, {"data", QString::fromLatin1(d.toHex())}});
    });
    connect(s, &api::debug_memory_read_failed, this, [this, tag](quint64 a, quint32 st) {
        broadcast_event(tag, QStringLiteral("debug_memory_failed"), {{"address", hex(a)}, {"status", static_cast<qint64>(st)}});
    });
    connect(s, &api::debug_memory_written, this, [this, tag](quint64 a, quint32 st) {
        broadcast_event(tag, QStringLiteral("debug_memory_written"), {{"address", hex(a)}, {"status", static_cast<qint64>(st)}});
    });
    connect(s, &api::debug_registers_ready, this,
            [this, tag](quint64 tid, opentm::tm_core::dbgp::ppu_registers r) {
        QJsonArray gprs;
        for (auto v : r.gpr) gprs.append(hex(v));
        broadcast_event(tag, QStringLiteral("debug_registers"), {{"thread", hex(tid)}, {"pc", hex(r.pc)}, {"lr", hex(r.lr)}, {"ctr", hex(r.ctr)}, {"cr", hex(r.cr)}, {"gpr", gprs}});
    });
    connect(s, &api::debug_registers_written, this, [this, tag](quint64 tid, quint32 st) {
        broadcast_event(tag, QStringLiteral("debug_registers_written"), {{"thread", hex(tid)}, {"status", static_cast<qint64>(st)}});
    });
    connect(s, &api::debug_breakpoint_added, this, [this, tag](quint64 a, quint32 st) {
        broadcast_event(tag, QStringLiteral("debug_breakpoint_added"), {{"address", hex(a)}, {"status", static_cast<qint64>(st)}});
    });
    connect(s, &api::debug_breakpoint_removed, this, [this, tag](quint64 a, quint32 st) {
        broadcast_event(tag, QStringLiteral("debug_breakpoint_removed"), {{"address", hex(a)}, {"status", static_cast<qint64>(st)}});
    });
    connect(s, &api::debug_thread_stopped, this,
            [this, tag](quint64 tid, quint64 a, quint32 reason) {
        broadcast_event(tag, QStringLiteral("debug_stopped"), {{"thread", hex(tid)}, {"address", hex(a)}, {"reason", static_cast<qint64>(reason)}});
    });
    connect(s, &api::debug_running_changed, this, [this, tag](bool running) {
        broadcast_event(tag, QStringLiteral("debug_running"), {{"running", running}});
    });
    connect(s, &api::debug_halt_finished, this, [this, tag](quint32 st) {
        broadcast_event(tag, QStringLiteral("debug_halted"), {{"status", static_cast<qint64>(st)}});
    });
    connect(s, &api::debug_process_changed, this, [this, tag](quint32 pid) {
        broadcast_event(tag, QStringLiteral("debug_process"), {{"pid", static_cast<qint64>(pid)}});
    });
    connect(s, &api::transfer_finished, this, [this, tag] {
        broadcast_event(tag, QStringLiteral("transfer_finished"), {});
    });
    connect(s, &api::transfer_failed, this, [this, tag](std::uint32_t r) {
        broadcast_event(tag, QStringLiteral("transfer_failed"), {{"result", hex(r)}});
    });
    connect(s, &api::install_reply, this, [this, tag](std::uint32_t st) {
        broadcast_event(tag, QStringLiteral("install_reply"), {{"status", hex(st)}});
    });
    connect(s, &ts::session_ready, this, [this, tag](std::uint16_t tok, std::uint16_t sub) {
        broadcast_event(tag, QStringLiteral("session_ready"), {{"token", tok}, {"sub_token", sub}});
    });
    connect(s, &ts::session_invalidated, this, [this, tag] {
        broadcast_event(tag, QStringLiteral("session_lost"), {});
    });
    connect(s, &ts::error, this, [this, tag](const QString& msg) {
        broadcast_event(tag, QStringLiteral("error"), {{"message", msg}});
    });

    connect(s, &api::load_ext_reply, this, [this, tag](std::uint32_t st) {
        broadcast_event(tag, QStringLiteral("load_reply"), {{"status", hex(st)}});
    });
    connect(s, &api::install_progress, this, [this, tag](int pct) {
        broadcast_event(tag, QStringLiteral("install_progress"), {{"percent", pct}});
    });
    connect(s, &api::install_finished, this, [this, tag](const QString& p) {
        broadcast_event(tag, QStringLiteral("install_finished"), {{"path", p}});
    });

    connect(s, &api::directory_listed, this,
            [this, tag](const QString& path, std::vector<dfmp_file_entry> entries) {
        QJsonArray arr;
        for (const auto& e : entries) arr.append(to_json(e));
        broadcast_event(tag, QStringLiteral("directory"), {{"path", path}, {"entries", arr}});
    });

    connect(s, &api::process_list_ready, this,
            [this, tag](QList<api::process_summary> ps) {
        QJsonArray arr;
        for (const auto& p : ps) {
            QJsonObject o;
            o["pid"]           = hex(p.pid);
            o["path"]          = cstr(p.info.self_path);
            o["ppu_threads"]   = static_cast<qint64>(p.info.ppu_thread_count);
            o["spu_threads"]   = static_cast<qint64>(p.info.spu_thread_count);
            o["local_memory"]  = static_cast<qint64>(p.memory.local_memory);
            o["remain_memory"] = static_cast<qint64>(p.memory.remain_memory);
            arr.append(o);
        }
        broadcast_event(tag, QStringLiteral("processes"), {{"processes", arr}});
    });

    auto forward = [this, tag](const char* kind) {
        return [this, tag, kind](std::uint32_t pid, auto list) {
            broadcast_event(tag, QStringLiteral("objects"), {{"kind", QLatin1String(kind)}, {"pid", hex(pid)}, {"items", array_of(list)}});
        };
    };
    connect(s, &api::threads_ready,      this, forward("threads"));
    connect(s, &api::modules_ready,      this, forward("modules"));
    connect(s, &api::mutexes_ready,      this, forward("mutexes"));
    connect(s, &api::lwmutexes_ready,    this, forward("lwmutexes"));
    connect(s, &api::conds_ready,        this, forward("conds"));
    connect(s, &api::event_queues_ready, this, forward("event_queues"));
    connect(s, &api::containers_ready,   this, forward("containers"));
}

//methods
void rpc_server::register_methods() {
    auto add = [this](const char* name, bool mutating, bool needs_target, bool needs_session, const char* help, auto fn) {
        methods_.insert(QLatin1String(name), method{fn, mutating, needs_target, needs_session, help});
    };
    auto ack = [](const char* key, const QJsonValue& v) {
        return QJsonObject{{QLatin1String(key), v}};
    };

    // server
    add("server.version", false, false, false,
        "When this server was built, and what it can do. Use it to spot a server older than the client talking to it.",
        [](managed*, const QJsonObject&, QString&) {
            return QJsonObject{
                {"build", QStringLiteral(__DATE__ " " __TIME__)},
                {"debugger", true},
            };
        });

    add("server.methods", false, false, false, "List available methods and their requirements.",
        [this](managed*, const QJsonObject&, QString&) {
            QJsonArray arr;
            for (auto it = methods_.constBegin(); it != methods_.constEnd(); ++it) {
                arr.append(QJsonObject{{"method", it.key()}, {"mutating", it->mutating}, {"needs_target", it->needs_target}, {"needs_session", it->needs_session}, {"help", QLatin1String(it->help)}});
            }
            return QJsonObject{{"methods", arr}};
        });

    add("server.status", false, false, false, "Server-wide state.",
        [this](managed*, const QJsonObject&, QString&) {
            return QJsonObject{{"targets", sessions_.size()}, {"clients", clients_.size()}, {"allow_mutating", allow_mutating_}, {"has_owner", owner_ != nullptr}};
        });

    add("server.own", false, false, false, "Claim ownership. With --exit-with-owner the server quits when this " "client disconnects.",
        [this](managed*, const QJsonObject&, QString& err) -> QJsonObject {
            if (!exit_with_owner_) {
                err = QStringLiteral("server was not started with " "--exit-with-owner");
                return {};
            }
            if (owner_ && owner_ != current_client_) {
                err = QStringLiteral("already owned by another client");
                return {};
            }
            owner_ = current_client_;
            return {{"owned", true}};
        });

    // targets
    add("target.open", false, false, false, "Open a session. Params: host, [target|name], [id], [port], [type], " "[serve_dir], or a full [record]. Idempotent: re-opening an existing " "handle updates it.",
        [this](managed*, const QJsonObject& p, QString& err) -> QJsonObject {
            auto* m = open_target(p, err);
            if (!m) return {};
            return {{"target", m->handle}, {"name", m->name}, {"host", m->session->target().host}, {"port", m->session->target().port}};
        });

    add("target.close", false, true, false, "Close a session.",
        [this](managed* m, const QJsonObject&, QString&) {
            const auto handle = m->handle;
            const auto name   = m->name;
            close_target(handle);
            return QJsonObject{{"closed", name}, {"target", handle}};
        });

    add("target.list", false, false, false, "Every open session and its state.",
        [this](managed*, const QJsonObject&, QString&) {
            QJsonArray arr;
            for (auto* m : sessions_) arr.append(status_json(m));
            return QJsonObject{{"targets", arr}};
        });

    add("target.status", false, true, false, "State of one session.",
        [](managed* m, const QJsonObject&, QString&) {
            return status_json(m);
        });

    add("target.connect", false, true, false, "Open the console session.",
        [](managed* m, const QJsonObject&, QString&) {
            const bool already = m->session->is_connected();
            if (!already) m->session->connect_to_target();
            return QJsonObject{
                {"target", m->handle},
                {"name", m->name},
                {"already_connected", already},
                {"session_ready", m->session->is_session_ready()},
                {"state", static_cast<int>(m->session->connection_state())}};
        });

    add("target.disconnect", false, true, false, "Close the console session.",
        [ack](managed* m, const QJsonObject&, QString&) {
            m->session->disconnect_from_target();
            return ack("disconnected", m->name);
        });

    add("target.serve_dir", false, true, false, "Set the host directory exposed as /app_home/.",
        [ack](managed* m, const QJsonObject& p, QString& err) -> QJsonObject {
            const auto dir = p.value("dir").toString();
            if (dir.isEmpty()) { err = QStringLiteral("dir required"); return {}; }
            m->session->set_file_serving_dir(dir);
            return ack("serve_dir", dir);
        });

    // reads
    add("fs.list", false, true, true,
        "List a kit directory. Answers with a 'directory' event.",
        [ack](managed* m, const QJsonObject& p, QString&) {
            const auto path = p.value("path").toString(QStringLiteral("/"));
            m->session->list_directory(path);
            return ack("requested", path);
        });

    add("tty.read", false, true, false, "Read buffered TTY from `since` (default 0). Returns lines + cursor.",
        [](managed* m, const QJsonObject& p, QString&) {
            const int since = qMax(0, p.value("since").toInt(0));
            QJsonArray arr;
            for (int i = since; i < m->tty.size(); ++i) arr.append(m->tty[i]);
            return QJsonObject{{"lines", arr}, {"cursor", m->tty.size()}};
        });

    add("fs.download", true, true, true,
        "Pull a target file to the host. Params: path (target), to (host), [size].",
        [ack](managed* m, const QJsonObject& p, QString& err) -> QJsonObject {
            const auto from = p.value("path").toString();
            const auto to   = p.value("to").toString();
            if (from.isEmpty() || to.isEmpty()) { err = QStringLiteral("path and to required"); return {}; }
            m->session->download_file(from, to, static_cast<std::uint32_t>(p.value("size").toDouble()));
            return ack("downloading", from);
        });

    add("fs.upload", true, true, true,
        "Push a host file to the target. Params: path (host), to (target).",
        [ack](managed* m, const QJsonObject& p, QString& err) -> QJsonObject {
            const auto from = p.value("path").toString();
            const auto to   = p.value("to").toString();
            if (from.isEmpty() || to.isEmpty()) { err = QStringLiteral("path and to required"); return {}; }
            if (!QFileInfo::exists(from)) { err = QStringLiteral("no such file: %1").arg(from); return {}; }
            m->session->upload_file(from, to);
            return ack("uploading", from);
        });

    add("fs.delete", true, true, true, "Delete a file on the target.",
        [ack](managed* m, const QJsonObject& p, QString& err) -> QJsonObject {
            const auto path = p.value("path").toString();
            if (path.isEmpty()) { err = QStringLiteral("path required"); return {}; }
            m->session->delete_file(path);
            return ack("deleting", path);
        });

    add("fs.rename", true, true, true, "Rename a file or directory on the target.", [ack](managed* m, const QJsonObject& p, QString& err) -> QJsonObject {
            const auto from = p.value("from").toString();
            const auto to   = p.value("to").toString();
            if (from.isEmpty() || to.isEmpty()) { err = QStringLiteral("from and to required"); return {}; }
            m->session->rename_file(from, to);
            return ack("renaming", from);
        });

    add("fs.mkdir", true, true, true, "Create a directory on the target.", [ack](managed* m, const QJsonObject& p, QString& err) -> QJsonObject {
            const auto path = p.value("path").toString();
            if (path.isEmpty()) { err = QStringLiteral("path required"); return {}; }
            bool ok = false;
            auto mode = p.value("mode").toString().toUInt(&ok);
            if (!ok) mode = 0777u;
            m->session->make_directory(path, mode);
            return ack("mkdir", path);
        });

    add("fs.chmod", true, true, true, "Set unix permissions on a target file.", [ack](managed* m, const QJsonObject& p, QString& err) -> QJsonObject {
            const auto path = p.value("path").toString();
            if (path.isEmpty()) { err = QStringLiteral("path required"); return {}; }
            bool ok = false;
            const auto mode = p.value("mode").toString().toUInt(&ok);
            if (!ok) { err = QStringLiteral("mode required"); return {}; }
            m->session->set_permissions(path, mode);
            return ack("chmod", path);
        });

    add("fs.utime", true, true, true, "Set access and modification times on a target file.", [ack](managed* m, const QJsonObject& p, QString& err) -> QJsonObject {
            const auto path = p.value("path").toString();
            if (path.isEmpty()) { err = QStringLiteral("path required"); return {}; }
            bool ok_a = false, ok_m = false;
            const auto atime = p.value("atime").toString().toULongLong(&ok_a);
            const auto mtime = p.value("mtime").toString().toULongLong(&ok_m);
            if (!ok_a || !ok_m) { err = QStringLiteral("atime and mtime required"); return {}; }
            m->session->set_times(path, atime, mtime);
            return ack("utime", path);
        });

    add("target.lpar_status", false, true, true, "Ask the CP for its logical-partition table. Answers in the log.", [ack](managed* m, const QJsonObject&, QString&) {
            m->session->refresh_lpar_status();
            return ack("requested", true);
        });

    add("process.list", false, true, true,
        "Enumerate processes. Answers with a 'processes' event.",
        [ack](managed* m, const QJsonObject&, QString&) {
            m->session->refresh_process_list();
            return ack("requested", true);
        });

    using api = opentm::tm_ui::session_api;
    struct drill { const char* name; void (api::*slot)(std::uint32_t); const char* help; };
    const drill drills[] = {
        {"process.threads",      &api::refresh_threads,      "PPU threads of a process."},
        {"process.modules",      &api::refresh_modules,      "Loaded modules."},
        {"process.mutexes",      &api::refresh_mutexes,      "Mutexes."},
        {"process.lwmutexes",    &api::refresh_lwmutexes,    "Light-weight mutexes."},
        {"process.conds",        &api::refresh_conds,        "Condition variables."},
        {"process.event_queues", &api::refresh_event_queues, "Event queues."},
        {"process.containers",   &api::refresh_containers,   "Memory containers."},
    };
    for (const auto& d : drills) {
        auto slot = d.slot;
        add(d.name, false, true, true, d.help,
            [slot, ack](managed* m, const QJsonObject& p, QString& err) -> QJsonObject {
                if (!p.contains("pid")) { err = QStringLiteral("pid required"); return {}; }
                (m->session->*slot)(pid_of(p));
                return ack("requested", p.value("pid"));
            });
    }

    {
        auto hex_arg = [](const QJsonObject& p, const char* key, bool* ok = nullptr) -> quint64 {
            auto text = p.value(QLatin1String(key)).toString().trimmed();
            if (text.startsWith(QLatin1String("0x"), Qt::CaseInsensitive)) text = text.mid(2);
            bool parsed = false;
            const auto v = text.toULongLong(&parsed, 16);
            if (ok) *ok = parsed;
            return parsed ? v : 0;
        };
        auto hex_list_arg = [](const QJsonObject& p, const char* key) {
            QList<quint64> out;
            for (const auto& v : p.value(QLatin1String(key)).toArray()) {
                auto text = v.toString().trimmed();
                if (text.startsWith(QLatin1String("0x"), Qt::CaseInsensitive)) text = text.mid(2);
                bool parsed = false;
                const auto x = text.toULongLong(&parsed, 16);
                if (parsed) out.append(x);
            }
            return out;
        };

        add("debug.attach", true, true, true,
            "Point the debugger at a process. Every other debug verb needs this first.",
            [ack](managed* m, const QJsonObject& p, QString& err) -> QJsonObject {
                if (!p.contains("pid")) { err = QStringLiteral("pid required"); return {}; }
                m->session->debug_set_process(pid_of(p));
                return ack("attached", p.value("pid"));
            });

        add("debug.read_memory", false, true, true,
            "Read memory. Answers with a 'debug_memory' event.",
            [ack, hex_arg](managed* m, const QJsonObject& p, QString& err) -> QJsonObject {
                bool ok = false;
                const auto address = hex_arg(p, "address", &ok);
                if (!ok) { err = QStringLiteral("address required, as hex"); return {}; }
                const auto length = p.value("length").toInt(0x100);
                m->session->debug_read_memory(address, static_cast<quint32>(length));
                return ack("requested", true);
            });

        add("debug.write_memory", true, true, true,
            "Write memory. `data` is hex, two characters per byte.",
            [ack, hex_arg](managed* m, const QJsonObject& p, QString& err) -> QJsonObject {
                bool ok = false;
                const auto address = hex_arg(p, "address", &ok);
                if (!ok) { err = QStringLiteral("address required, as hex"); return {}; }
                const auto data = QByteArray::fromHex(p.value("data").toString().toLatin1());
                if (data.isEmpty()) { err = QStringLiteral("data required, as hex"); return {}; }
                m->session->debug_write_memory(address, data);
                return ack("requested", true);
            });

        add("debug.read_registers", false, true, true,
            "Read a thread's PPU registers. Answers with a 'debug_registers' event.",
            [ack, hex_arg](managed* m, const QJsonObject& p, QString& err) -> QJsonObject {
                bool ok = false;
                const auto thread = hex_arg(p, "thread", &ok);
                if (!ok) { err = QStringLiteral("thread required, as hex"); return {}; }
                m->session->debug_read_registers(thread);
                return ack("requested", true);
            });

        add("debug.write_gpr", true, true, true,
            "Write one general purpose register.",
            [ack, hex_arg](managed* m, const QJsonObject& p, QString& err) -> QJsonObject {
                bool ok = false, ok_v = false;
                const auto thread = hex_arg(p, "thread", &ok);
                const auto value  = hex_arg(p, "value", &ok_v);
                if (!ok || !ok_v) { err = QStringLiteral("thread and value required, as hex"); return {}; }
                const auto index = p.value("index").toInt(-1);
                if (index < 0 || index > 31) { err = QStringLiteral("index must be 0..31"); return {}; }
                m->session->debug_write_gpr(thread, static_cast<unsigned>(index), value);
                return ack("requested", true);
            });

        struct bp_verb { const char* name; bool set; const char* help; };
        const bp_verb bps[] = {
            {"debug.set_breakpoint",   true,  "Set a PPU breakpoint."},
            {"debug.clear_breakpoint", false, "Clear a PPU breakpoint."},
        };
        for (const auto& b : bps) {
            const bool set = b.set;
            add(b.name, true, true, true, b.help,
                [ack, hex_arg, set](managed* m, const QJsonObject& p, QString& err) -> QJsonObject {
                    bool ok = false;
                    const auto address = hex_arg(p, "address", &ok);
                    if (!ok) { err = QStringLiteral("address required, as hex"); return {}; }
                    if (set) m->session->debug_set_breakpoint(address);
                    else     m->session->debug_clear_breakpoint(address);
                    return ack("requested", true);
                });
        }

        add("debug.resume", true, true, true, "Continue the given threads.",
            [ack, hex_list_arg](managed* m, const QJsonObject& p, QString& err) -> QJsonObject {
                const auto ids = hex_list_arg(p, "threads");
                if (ids.isEmpty()) { err = QStringLiteral("threads required"); return {}; }
                m->session->debug_resume(ids);
                return ack("requested", true);
            });

        add("debug.halt", true, true, true, "Stop the given threads.",
            [ack, hex_list_arg](managed* m, const QJsonObject& p, QString& err) -> QJsonObject {
                const auto ids = hex_list_arg(p, "threads");
                if (ids.isEmpty()) { err = QStringLiteral("threads required"); return {}; }
                m->session->debug_halt(ids);
                return ack("requested", true);
            });

        add("debug.step", true, true, true,
            "Step a thread. `addresses` lists every landing place to cover - a "
            "conditional branch needs both the target and the fall-through.",
            [ack, hex_arg, hex_list_arg](managed* m, const QJsonObject& p, QString& err) -> QJsonObject {
                bool ok = false;
                const auto thread = hex_arg(p, "thread", &ok);
                if (!ok) { err = QStringLiteral("thread required, as hex"); return {}; }
                const auto addresses = hex_list_arg(p, "addresses");
                if (addresses.isEmpty()) { err = QStringLiteral("addresses required"); return {}; }
                m->session->debug_step_to(thread, addresses);
                return ack("requested", true);
            });
    }

    // mutating
    add("process.load", true, true, true, "LOAD_EXT a host-side SELF. `options` takes any load_options field; " "anything omitted keeps its default.",
        [ack](managed* m, const QJsonObject& p, QString& err) -> QJsonObject {
            const auto path = p.value("path").toString();
            if (path.isEmpty()) { err = QStringLiteral("path required"); return {}; }
            m->session->load_executable(
                path, opentm::tm_ui::load_options_from_json(p.value("options").toObject()));
            return ack("loading", path);
        });

    add("package.install", true, true, true, "Install a host-side .pkg.",
        [ack](managed* m, const QJsonObject& p, QString& err) -> QJsonObject {
            const auto path = p.value("path").toString();
            if (!QFileInfo::exists(path)) {
                err = QStringLiteral("no such file: %1").arg(path);
                return {};
            }
            m->session->install_package(path);
            return ack("installing", path);
        });

    const drill proc_ctl[] = {
        {"process.pause",     &api::pause_process,     "Stop a process."},
        {"process.resume",    &api::resume_process,    "Continue a process."},
        {"process.terminate", &api::terminate_process, "Terminate a process."},
        {"process.core_dump", &api::trigger_core_dump, "Trigger a core dump."},
    };
    for (const auto& d : proc_ctl) {
        auto slot = d.slot;
        add(d.name, true, true, true, d.help,
            [slot, ack](managed* m, const QJsonObject& p, QString& err) -> QJsonObject {
                if (!p.contains("pid")) { err = QStringLiteral("pid required"); return {}; }
                (m->session->*slot)(pid_of(p));
                return ack("requested", p.value("pid"));
            });
    }

    struct act { const char* name; void (api::*slot)(); const char* help; };
    const act acts[] = {
        {"target.power_on",    &api::power_on,         "Power the target on."},
        {"target.power_off",   &api::power_off,        "Shut the target down."},
        {"target.power_kill",  &api::power_off_force,  "Force power off."},
        {"target.wake_on_lan", &api::wake_on_lan,      "Send a WoL magic packet."},
        {"settings.refresh",   &api::settings_refresh, "Dump XMB settings."},
        {"settings.commit",    &api::settings_commit,  "Commit transferred settings."},
    };
    for (const auto& a : acts) {
        auto slot = a.slot;
        add(a.name, true, true, false, a.help,
            [slot, ack](managed* m, const QJsonObject&, QString&) {
                (m->session->*slot)();
                return ack("sent", true);
            });
    }

    add("target.reset", true, true, false, "Reset using the target's configured reset mode.",
        [ack](managed* m, const QJsonObject& p, QString& err) -> QJsonObject {
            const auto mode = p.value("mode").toString(QStringLiteral("current"));
            if (mode != QLatin1String("current")) {
                err = QStringLiteral("only mode 'current' is supported; set the " "reset mode in the target's properties");
                return {};
            }
            m->session->reset_current();
            return ack("reset", mode);
        });

    add("settings.apply", true, true, true, "Transfer a host-side .SFT. Optional `size` overrides the file's own; " "follow with settings.commit and a reset.",
        [ack](managed* m, const QJsonObject& p, QString& err) -> QJsonObject {
            const auto path = p.value("path").toString();
            const QFileInfo fi(path);
            if (!fi.exists()) { err = QStringLiteral("no such file: %1").arg(path); return {}; }
            const auto size = static_cast<std::uint32_t>(
                p.contains("size") ? p.value("size").toDouble() : fi.size());
            m->session->settings_apply(path, size);
            return ack("applying", path);
        });
}

// transport
bool rpc_server::listen_tcp(quint16 port) {
    tcp_ = new QTcpServer(this);
    if (!tcp_->listen(QHostAddress::LocalHost, port)) {
        emit log_message(QStringLiteral("tcp listen failed: %1").arg(tcp_->errorString()));
        return false;
    }
    connect(tcp_, &QTcpServer::newConnection, this, &rpc_server::on_new_tcp_client);
    emit log_message(QStringLiteral("listening on 127.0.0.1:%1").arg(port));
    return true;
}

bool rpc_server::listen_local(const QString& name) {
    local_ = new QLocalServer(this);
    QLocalServer::removeServer(name);
    if (!local_->listen(name)) {
        emit log_message(QStringLiteral("local listen failed: %1").arg(local_->errorString()));
        return false;
    }
    connect(local_, &QLocalServer::newConnection, this, &rpc_server::on_new_local_client);
    emit log_message(QStringLiteral("listening on local socket '%1'").arg(name));
    return true;
}

void rpc_server::on_new_tcp_client() {
    while (auto* s = tcp_->nextPendingConnection()) attach(s);
}

void rpc_server::on_new_local_client() {
    while (auto* s = local_->nextPendingConnection()) attach(s);
}

void rpc_server::attach(QIODevice* dev) {
    clients_.append(dev);
    buffers_[dev] = {};
    connect(dev, &QIODevice::readyRead, this, [this, dev] { on_ready_read(dev); });
    connect(dev, &QObject::destroyed, this, [this, dev] {
        clients_.removeAll(dev);
        buffers_.remove(dev);
        if (dev == owner_) {
            owner_ = nullptr;
            emit log_message(QStringLiteral("owner disconnected - shutting down"));
            QCoreApplication::quit();
        }
    });
    if (auto* t = qobject_cast<QTcpSocket*>(dev)) {
        connect(t, &QTcpSocket::disconnected, t, &QObject::deleteLater);
    } else if (auto* l = qobject_cast<QLocalSocket*>(dev)) {
        connect(l, &QLocalSocket::disconnected, l, &QObject::deleteLater);
    }
    emit log_message(QStringLiteral("client connected (%1 total)").arg(clients_.size()));
}

void rpc_server::on_ready_read(QIODevice* dev) {
    auto& buf = buffers_[dev];
    buf += dev->readAll();
    int nl;
    while ((nl = buf.indexOf('\n')) >= 0) {
        const QByteArray line = buf.left(nl).trimmed();
        buf.remove(0, nl + 1);
        if (!line.isEmpty()) handle_line(dev, line);
    }
}

void rpc_server::handle_line(QIODevice* dev, const QByteArray& line) {
    QJsonParseError perr{};
    const auto doc = QJsonDocument::fromJson(line, &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
        send(dev, {{"ok", false}, {"error", QStringLiteral("bad json: %1").arg(perr.errorString())}});
        return;
    }
    const auto req    = doc.object();
    const auto id     = req.value("id");
    const auto name   = req.value("method").toString();
    const auto params = req.value("params").toObject();

    QString err;
    QJsonObject result;

    current_client_ = dev;
    const auto it = methods_.constFind(name);
    if (it == methods_.constEnd()) {
        err = QStringLiteral("unknown method '%1'").arg(name);
    } else if (it->mutating && !allow_mutating_) {
        err = QStringLiteral("'%1' changes target state; server started " "without --allow-mutating").arg(name);
    } else {
        managed* m = nullptr;
        if (it->needs_target) {
            m = resolve_target(params, err);
            if (m && it->needs_session && !m->session->is_session_ready()) {
                err = QStringLiteral("target '%1' has no session - call " "target.connect first").arg(m->name);
                m = nullptr;
            }
        }
        if (err.isEmpty()) result = it->fn(m, params, err);
    }
    current_client_ = nullptr;

    QJsonObject reply;
    if (!id.isUndefined()) reply["id"] = id;
    if (err.isEmpty()) {
        reply["ok"]     = true;
        reply["result"] = result;
    } else {
        reply["ok"]    = false;
        reply["error"] = err;
    }
    send(dev, reply);
}

void rpc_server::send(QIODevice* dev, const QJsonObject& obj) {
    if (!dev || !dev->isWritable()) return;
    dev->write(QJsonDocument(obj).toJson(QJsonDocument::Compact));
    dev->write("\n");
}

void rpc_server::broadcast_event(const QString& target, const QString& name, const QJsonObject& params)
{
    QJsonObject o{{"event", name}, {"target", target}};
    if (!params.isEmpty()) o["params"] = params;
    for (auto* c : clients_) send(c, o);
}

} // namespace opentm::tm_server
