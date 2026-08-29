#include "target_session.h"

#include <QTimer>

#include <QDir>
#include <QHostAddress>
#include <QHostInfo>
#include <QRegularExpression>
#include <QStandardPaths>

namespace opentm::tm_ui {

namespace {

QString default_app_home(const QString& target_name) {
    QString safe = target_name.isEmpty() ? QStringLiteral("target") : target_name;
    safe.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9_.-]")), QStringLiteral("_"));

    const QString base = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    return QDir::cleanPath(QStringLiteral("%1/OpenTM/app_home/%2").arg(base, safe));
}
} // namespace

target_session::target_session(QObject* parent) : session_api(parent) {
    reconnect_timer_.setSingleShot(true);
    connect(&reconnect_timer_, &QTimer::timeout, this, [this] {
        emit log_message(QStringLiteral("    -- reconnect attempt %1/%2 to %3:%4").arg(reconnect_attempts_).arg(kMaxReconnectAttempts).arg(record_.host).arg(record_.port));
        emit status_message(tr("Reconnecting (%1/%2)...").arg(reconnect_attempts_).arg(kMaxReconnectAttempts));
        connect_to_target();
    });
    wire();
}

target_session::~target_session() = default;

void target_session::wire() {
    using tc = opentm::tm_core::tcp_connection;

    connect(&conn_, &tc::frame_received, &disp_, &frame_dispatcher::on_frame_received);
    connect(&conn_, &tc::state_changed,  &session_, &session_controller::on_connection_state);

    connect(&disp_, &frame_dispatcher::version_string_received,   &session_, &session_controller::on_version_string);
    connect(&disp_, &frame_dispatcher::session_token_received,    &session_, &session_controller::on_session_token);
    connect(&disp_, &frame_dispatcher::sub_token_received,        &session_, &session_controller::on_sub_token);
    connect(&disp_, &frame_dispatcher::session_handshake_acked,   &session_, &session_controller::on_handshake_acked);
    connect(&disp_, &frame_dispatcher::session_reaped,            &session_, &session_controller::on_session_reaped);
    connect(&disp_, &frame_dispatcher::server_rejection,          &session_, &session_controller::on_server_rejection);
    connect(&disp_, &frame_dispatcher::debug_agent_up,            &session_, &session_controller::on_debug_agent_up);
    connect(&disp_, &frame_dispatcher::boot_param_received,       &session_, &session_controller::on_boot_param);
    connect(&disp_, &frame_dispatcher::dex_tsmp_reply,            &session_, &session_controller::on_dex_tsmp_reply);
    connect(&disp_, &frame_dispatcher::tty_text,                  &session_, &session_controller::on_tty_text);
    connect(&disp_, &frame_dispatcher::raw_frame,                 &files_,    &host_file_server::on_frame_received);
    connect(&disp_, &frame_dispatcher::raw_frame,                 &kernel_,   &kernel_explorer_controller::on_frame_received);
    connect(&disp_, &frame_dispatcher::raw_frame,                 &debug_,    &debug_controller::on_frame_received);
    connect(&disp_, &frame_dispatcher::dfmp_get_entries_reply,    &explorer_, &file_explorer_controller::on_dfmp_get_entries_reply);
    connect(&disp_, &frame_dispatcher::dfmp_op_reply,             &explorer_, &file_explorer_controller::on_dfmp_op_reply);
    connect(&disp_, &frame_dispatcher::netmp_register_nack, this, [this](std::uint8_t status, std::uint32_t proto) {
        static const struct { std::uint32_t proto; const char* name; } kNames[] = {
            {0x00000200u, "DBGP"}, {0x00000100u, "DFMP"}, {0x00000110u, "DRFP"},
            {0x00000310u, "TTYP"}, {0x00800011u, "CTRLP"}, {0x00000020u, "CP"},
            {0x00800300u, "TTY"},
        };
        QString name = QStringLiteral("0x%1").arg(proto, 8, 16, QChar('0'));
        for (const auto& n : kNames) {
            if (n.proto == proto) { name = QLatin1String(n.name); break; }
        }
        // 0x0c is this connection asking twice for something it already has;
        // anything else means the kit will not give it to us
        const bool already_ours = status == 0x0c;
        emit log_message(QStringLiteral("       !! %1 registration refused (status 0x%2)%3")
                             .arg(name).arg(status, 2, 16, QChar('0'))
                             .arg(already_ours ? QStringLiteral(" - this connection already holds it")
                                               : QStringLiteral(" - another connection still owns it")));
        emit protocol_rejected(proto, name, status);
    });
    connect(&explorer_, &file_explorer_controller::file_op_finished, this, &session_api::file_op_finished);
    connect(&disp_,     &frame_dispatcher::frame_logged,          this, &session_api::wire_line);
    connect(&disp_,     &frame_dispatcher::tty_text,              this, &session_api::tty_text);
    connect(&session_,  &session_controller::log_message,         this, &session_api::log_message);
    connect(&files_,    &host_file_server::log_message,           this, &session_api::log_message);
    connect(&explorer_, &file_explorer_controller::log_message,   this, &session_api::log_message);
    connect(&kernel_,   &kernel_explorer_controller::log_message, this, &session_api::log_message);
    connect(&debug_,    &debug_controller::log_message,           this, &session_api::log_message);
    connect(&debug_, &debug_controller::memory_ready,        this, &session_api::debug_memory_ready);
    connect(&debug_, &debug_controller::memory_read_failed,  this, &session_api::debug_memory_read_failed);
    connect(&debug_, &debug_controller::memory_written,      this, &session_api::debug_memory_written);
    connect(&debug_, &debug_controller::registers_ready,     this, &session_api::debug_registers_ready);
    connect(&debug_, &debug_controller::registers_written,   this, &session_api::debug_registers_written);
    connect(&debug_, &debug_controller::breakpoint_added,    this, &session_api::debug_breakpoint_added);
    connect(&debug_, &debug_controller::breakpoint_removed,  this, &session_api::debug_breakpoint_removed);
    connect(&debug_, &debug_controller::thread_stopped,      this, &session_api::debug_thread_stopped);
    connect(&debug_, &debug_controller::running_changed,     this, &session_api::debug_running_changed);
    connect(&debug_, &debug_controller::halt_finished,       this, &session_api::debug_halt_finished);
    connect(&debug_, &debug_controller::process_changed,     this, &session_api::debug_process_changed);
    connect(&actions_,  &target_actions::log_message,             this, &session_api::log_message);
    connect(&conn_,     &tc::log_message,                         this, &session_api::log_message);
    connect(&conn_,     &tc::error_occurred,                      this, &session_api::error);
    connect(&session_, &session_controller::session_ready,       this, &session_api::session_ready);
    connect(&session_, &session_controller::boot_mode_changed,   this, &session_api::boot_mode_changed);
    connect(&session_, &session_controller::session_ready,       &actions_, [this] {
        if (!session_.control_only()) actions_.request_current_boot_param();
    });
    connect(&session_, &session_controller::session_ready,       this, [this] {
        auto queued = std::move(pending_verbs_);
        pending_verbs_.clear();
        for (auto& v : queued) v();
    });
    connect(&disp_, &frame_dispatcher::power_reply, this, [this](quint16 cmd, quint32 result) {
        emit log_message(QStringLiteral("    << power reply cmd=0x%1 result=0x%2").arg(cmd, 4, 16, QChar('0')).arg(result, 8, 16, QChar('0')));
        QTimer::singleShot(0, this, &target_session::end_control_only_session);
    });
    connect(&session_, &session_controller::session_invalidated, this, &session_api::session_invalidated);
    connect(&session_, &session_controller::session_invalidated, &debug_, &debug_controller::forget_state);
    connect(&session_, &session_controller::debug_agent_ready,   this, &session_api::debug_agent_ready);
    connect(&session_, &session_controller::sdk_version_received, this, &session_api::sdk_version_received);
    connect(&session_, &session_controller::status_message,      this, &session_api::status_message);
    connect(&conn_,    &tc::state_changed,                       this, &session_api::connection_state_changed);
    connect(&conn_,    &tc::state_changed,                       this, &target_session::on_link_state);
    connect(&actions_,  &target_actions::status_message,  this, &session_api::status_message);
    connect(&actions_,  &target_actions::clear_console,   this, &session_api::clear_console);
    connect(&explorer_, &file_explorer_controller::status_message, this, &session_api::status_message);
    connect(&files_,    &host_file_server::status_message, this, &session_api::status_message);
    connect(&disp_, &frame_dispatcher::tty_stream_text,   this, &session_api::tty_stream_text);
    connect(&disp_, &frame_dispatcher::cp_version_received, this, &session_api::cp_version_received);
    connect(&disp_, &frame_dispatcher::dcmp_status, this, [this](std::uint8_t code) {
        namespace dcmp = opentm::tm_core::dcmp;
        emit log_message(QStringLiteral("    >> DCMP STATUS/%1").arg(QString::fromLatin1(dcmp::status_name(code))));

        if (dcmp::status_means_going_down(code)) {
            user_disconnected_ = true;
            cancel_reconnect();
            emit status_message(tr("Target reported %1").arg(QString::fromLatin1(dcmp::status_name(code))));
            emit target_went_down(QString::fromLatin1(dcmp::status_name(code)));
        } else if (dcmp::status_means_coming_up(code)) {
            user_disconnected_ = false;
            emit status_message(tr("Target reported %1").arg(QString::fromLatin1(dcmp::status_name(code))));
            if (code == dcmp::status_code::system_boot
                || code == dcmp::status_code::lpar_boot) {
                session_.on_target_came_up();
                actions_.request_current_boot_param();
            }
        }
    });

    connect(&disp_, &frame_dispatcher::transfer_acked, this,
            [this](std::uint32_t seq, std::uint32_t result) {
        if (result == 0) {
            emit log_message(QStringLiteral("       >> transfer accepted (seq=0x%1)").arg(seq, 8, 16, QChar('0')));
            return;
        }
        emit log_message(QStringLiteral("       !! transfer refused, result=0x%1 (seq=0x%2)").arg(result, 0, 16).arg(seq, 8, 16, QChar('0')));
        emit transfer_failed(result);
    });
    connect(&disp_, &frame_dispatcher::transfer_finished, this, [this] {
        emit log_message(QStringLiteral("       >> transfer complete"));
        emit session_api::transfer_finished();
    });

    connect(&disp_, &frame_dispatcher::lpar_status_received, this,
            [this](std::vector<opentm::tm_core::tsmp_lpar_entry> entries) {
        emit log_message(QStringLiteral("    >> LPAR status: %1 partition(s)").arg(entries.size()));
        for (const auto& e : entries) {
            emit log_message(QStringLiteral("       %1 %2  detail=0x%3").arg(QString::fromStdString(e.name), -14).arg(QString::fromLatin1(opentm::tm_core::tsmp_lpar_state_name(e.status)), -8).arg(e.detail, 8, 16, QChar('0')));
        }
    });
    connect(&disp_, &frame_dispatcher::load_ext_reply,    this, &session_api::load_ext_reply);
    connect(&disp_, &frame_dispatcher::install_progress,  this, &session_api::install_progress);
    connect(&disp_, &frame_dispatcher::install_finished,  this, &session_api::install_finished);
    connect(&disp_, &frame_dispatcher::install_reply,     this, &session_api::install_reply);

    connect(&explorer_, &file_explorer_controller::directory_listed, this, &session_api::directory_listed);

    connect(&kernel_, &kernel_explorer_controller::threads_ready,      this, &session_api::threads_ready);
    connect(&kernel_, &kernel_explorer_controller::modules_ready,      this, &session_api::modules_ready);
    connect(&kernel_, &kernel_explorer_controller::mutexes_ready,      this, &session_api::mutexes_ready);
    connect(&kernel_, &kernel_explorer_controller::lwmutexes_ready,    this, &session_api::lwmutexes_ready);
    connect(&kernel_, &kernel_explorer_controller::conds_ready,        this, &session_api::conds_ready);
    connect(&kernel_, &kernel_explorer_controller::event_queues_ready, this, &session_api::event_queues_ready);
    connect(&kernel_, &kernel_explorer_controller::containers_ready,   this, &session_api::containers_ready);
    connect(&kernel_, &kernel_explorer_controller::process_list_ready, this,
            [this](QList<kernel_explorer_controller::process_summary> ps) {
        QList<session_api::process_summary> out;
        out.reserve(ps.size());
        for (const auto& p : ps) out.push_back({p.pid, p.info, p.memory});
        emit process_list_ready(out);
    });
}

void target_session::set_target(const target_record& r) {
    
    if (is_connected() && r.type != record_.type) {
        emit error(QStringLiteral("ignoring target change to %1 while connected to %2 - ""disconnect first").arg(r.name, record_.name));
        return;
    }

    record_ = r;
    if (record_.file_server_dir.isEmpty()) {
        record_.file_server_dir = default_app_home(r.name);
        QDir().mkpath(record_.file_server_dir);
        emit log_message(QStringLiteral("    -- no File Server Dir set for '%1'; using %2").arg(r.name, record_.file_server_dir));
        emit log_message(QStringLiteral("       Set one in Target Properties to serve your own build ""directory instead."));
    }
    const bool is_dex = (r.type == opentm::tm_core::target_type::cfw_dex);

    actions_.set_target(record_);
    session_.set_target_type(r.type);
    conn_.set_single_socket(is_dex);
    files_.set_file_server_dir(record_.file_server_dir);
    explorer_.set_serving_dir(record_.file_server_dir);
    files_.set_cfw_dex(is_dex);
}

void target_session::set_file_serving_dir(const QString& dir) {
    record_.file_server_dir = dir;
    files_.set_file_server_dir(dir);
    explorer_.set_serving_dir(dir);
}

void target_session::on_link_state(opentm::tm_core::tcp_connection::state s) {
    using state = opentm::tm_core::tcp_connection::state;
    if (s == state::ready) {
        was_established_ = true;
        if (reconnect_attempts_ > 0) {
            emit log_message(QStringLiteral("    -- reconnected to %1:%2 after %3 attempt(s)").arg(record_.host).arg(record_.port).arg(reconnect_attempts_));
            emit status_message(tr("Reconnected"));
        }
        cancel_reconnect();
        return;
    }
    if (s == state::disconnected || s == state::error_state) {
        // only chase links that were up and went away on their own
        if (was_established_ && !user_disconnected_) schedule_reconnect();
        was_established_ = false;
    }
}

void target_session::when_ready(std::function<void()> verb) {
    if (is_session_ready()) { verb(); return; }
    if (record_.host.isEmpty()) { emit status_message(tr("No target selected")); return; }
    pending_verbs_.push_back(std::move(verb));
    emit status_message(tr("Reaching the communications processor on %1...").arg(record_.name.isEmpty() ? record_.host : record_.name));
    user_disconnected_ = false;
    if (conn_.current_state() == opentm::tm_core::tcp_connection::state::disconnected) {
        control_only_session_ = true;
        session_.set_control_only(true);
        connect_to_target();
    }
}

void target_session::end_control_only_session() {
    if (!control_only_session_) return;
    control_only_session_ = false;
    session_.set_control_only(false);
    user_disconnected_ = true;
    emit log_message(QStringLiteral("    -- control-only session done, closing"));
    disconnect_from_target();
}

void target_session::schedule_reconnect() {
    if (!auto_reconnect_ || reconnect_timer_.isActive()) return;
    if (++reconnect_attempts_ > kMaxReconnectAttempts) {
        emit log_message(QStringLiteral("    !! giving up reconnecting to %1:%2 after %3 attempts").arg(record_.host).arg(record_.port).arg(kMaxReconnectAttempts));
        emit status_message(tr("Disconnected - reconnect gave up"));
        reconnect_attempts_ = 0;
        return;
    }
    const int base = record_.timeouts.reconnect_ms > 0 ? record_.timeouts.reconnect_ms : 2000;
    // back off so a kit that stays down doesn't get hammered for 30 tries
    const int delay = qMin(base * qMin(reconnect_attempts_, 8), 30000);
    emit log_message(QStringLiteral("    -- link to %1:%2 lost; retrying in %3 ms").arg(record_.host).arg(record_.port).arg(delay));
    reconnect_timer_.start(delay);
}

void target_session::cancel_reconnect() {
    reconnect_timer_.stop();
    reconnect_attempts_ = 0;
}

void target_session::connect_to_target() {
    user_disconnected_ = false;

    using state = opentm::tm_core::tcp_connection::state;
    const auto st = conn_.current_state();
    if (st != state::disconnected && st != state::error_state) {
        emit log_message(QStringLiteral("    -- already connected to %1 - ignoring connect").arg(record_.host));
        return;
    }

    QHostAddress addr;
    if (addr.setAddress(record_.host)) {
        conn_.connect_to_target(addr, record_.port);
        return;
    }
    const auto info = QHostInfo::fromName(record_.host);
    if (info.addresses().isEmpty()) {
        emit error(QStringLiteral("cannot resolve host '%1'").arg(record_.host));
        return;
    }
    conn_.connect_to_target(info.addresses().first(), record_.port);
}

void target_session::disconnect_from_target() {
    user_disconnected_ = true;
    cancel_reconnect();
    session_.send_warmup_deregister();
    conn_.disconnect_and_wait(1500);
}

bool target_session::is_connected() const {
    return conn_.current_state() == opentm::tm_core::tcp_connection::state::ready;
}

bool target_session::is_session_ready() const {
    return session_.is_ready();
}

QString target_session::peer_summary() const {
    return conn_.peer_summary();
}

void target_session::clear_target() {
    record_ = {};
    actions_.clear_target();
}

} // namespace opentm::tm_ui
