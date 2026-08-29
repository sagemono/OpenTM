#pragma once

#include "file_explorer_controller.h"
#include "frame_dispatcher.h"
#include "host_file_server.h"
#include "kernel_explorer_controller.h"
#include "debug_controller.h"
#include "session_api.h"
#include "session_controller.h"
#include "target_actions.h"
#include "target_record.h"

#include <tm_core/tcp_connection.h>

#include <QTimer>

namespace opentm::tm_ui {

class target_session : public session_api {
    Q_OBJECT
public:
    explicit target_session(QObject* parent = nullptr);
    ~target_session() override;
    void set_target(const target_record& r) override;
    void clear_target() override;
    const target_record& target() const override { return record_; }

    void set_file_serving_dir(const QString& dir) override;
    void connect_to_target() override;
    void disconnect_from_target() override;

    bool is_connected() const override;
    bool is_session_ready() const override;
    QString peer_summary() const override;
    opentm::tm_core::tcp_connection::state connection_state() const override {
        return conn_.current_state();
    }

    void power_on() override        { when_ready([this] { actions_.power_on(); }); }
    void power_off() override       { when_ready([this] { user_disconnected_ = true; actions_.power_off(); }); }
    void power_off_force() override { when_ready([this] { user_disconnected_ = true; actions_.power_off_force(); }); }
    void reset_current() override   { when_ready([this] { actions_.reset_current(); }); }
    void wake_on_lan() override     { actions_.wake_on_lan(); }

    void load_executable(const QString& path, const load_options& o) override {
        actions_.send_load_executable(path, o);
    }
    void install_package(const QString& host_path) override {
        actions_.install_package(host_path);
    }

    void settings_refresh() override { actions_.send_settings_refresh(); }
    void settings_commit() override  { actions_.send_settings_commit(); }
    void settings_apply(const QString& host_path, std::uint32_t size) override {
        actions_.send_settings_apply(host_path, size);
    }

    void list_directory(const QString& path) override {
        explorer_.list_directory(path);
    }
    void download_file(const QString& kit_path, const QString& host_path, std::uint32_t size) override {
        // the target will drive DRFP at this path, which sits outside the serving directory on purpose
        files_.authorise_transfer_path(host_path);
        explorer_.download_file(kit_path, host_path, size);
    }
    void upload_file(const QString& host_path, const QString& kit_path) override {
        files_.authorise_transfer_path(host_path);
        explorer_.upload_file(host_path, kit_path);
    }
    void rename_file(const QString& from, const QString& to) override {
        explorer_.rename_file(from, to);
    }
    void make_directory(const QString& kit_path, std::uint32_t mode) override {
        explorer_.make_directory(kit_path, mode);
    }
    void set_permissions(const QString& kit_path, std::uint32_t mode) override {
        explorer_.set_permissions(kit_path, mode);
    }
    void set_times(const QString& kit_path, std::uint64_t atime, std::uint64_t mtime) override {
        explorer_.set_times(kit_path, atime, mtime);
    }
    void delete_file(const QString& kit_path) override {
        explorer_.delete_file(kit_path);
    }

    void refresh_lpar_status() override             { actions_.request_lpar_status(); }
    void refresh_process_list() override            { kernel_.refresh_process_list(); }
    void refresh_threads(std::uint32_t pid) override      { kernel_.refresh_threads(pid); }
    void refresh_modules(std::uint32_t pid) override      { kernel_.refresh_modules(pid); }
    void refresh_mutexes(std::uint32_t pid) override      { kernel_.refresh_mutexes(pid); }
    void refresh_lwmutexes(std::uint32_t pid) override    { kernel_.refresh_lwmutexes(pid); }
    void refresh_conds(std::uint32_t pid) override        { kernel_.refresh_conds(pid); }
    void refresh_event_queues(std::uint32_t pid) override { kernel_.refresh_event_queues(pid); }
    void refresh_containers(std::uint32_t pid) override   { kernel_.refresh_containers(pid); }

    void pause_process(std::uint32_t pid) override     { kernel_.pause_process(pid); }
    void resume_process(std::uint32_t pid) override    { kernel_.resume_process(pid); }
    void terminate_process(std::uint32_t pid) override { kernel_.terminate_process(pid); }
    void trigger_core_dump(std::uint32_t pid) override { kernel_.trigger_core_dump(pid); }

    bool supports_debugger() const override { return true; }
    void debug_read_memory(quint64 a, quint32 n) override        { debug_.read_memory(a, n); }
    void debug_write_memory(quint64 a, QByteArray d) override    { debug_.write_memory(a, std::move(d)); }
    void debug_read_registers(quint64 tid) override              { debug_.read_registers(tid); }
    void debug_write_gpr(quint64 tid, unsigned i, quint64 v) override { debug_.write_gpr(tid, i, v); }
    void debug_set_breakpoint(quint64 a) override                { debug_.set_breakpoint(a); }
    void debug_clear_breakpoint(quint64 a) override              { debug_.clear_breakpoint(a); }
    void debug_resume(QList<quint64> ids) override               { debug_.resume(std::move(ids)); }
    void debug_halt(QList<quint64> ids) override                 { debug_.halt(std::move(ids)); }
    void debug_step_to(quint64 tid, QList<quint64> a) override   { debug_.step_to_any(tid, std::move(a)); }
    void debug_set_process(std::uint32_t pid) override           { debug_.set_process(pid); }

    opentm::tm_core::tcp_connection* connection() { return &conn_; }
    session_controller*              session()    { return &session_; }
    target_actions*                  actions()    { return &actions_; }
    file_explorer_controller*        explorer()   { return &explorer_; }
    kernel_explorer_controller*      kernel()     { return &kernel_; }
    debug_controller*                debugger()   { return &debug_; }
    host_file_server*                files()      { return &files_; }
    frame_dispatcher*                dispatcher() { return &disp_; }

    void set_auto_reconnect(bool on) override { auto_reconnect_ = on; }
    bool is_reconnecting() const noexcept { return reconnect_timer_.isActive(); }

private:
    void wire();
    void on_link_state(opentm::tm_core::tcp_connection::state s);
    void schedule_reconnect();
    void when_ready(std::function<void()> verb);
    void end_control_only_session();

    std::vector<std::function<void()>> pending_verbs_;
    bool control_only_session_ = false;
    void cancel_reconnect();

    QTimer reconnect_timer_;
    int    reconnect_attempts_ = 0;
    bool   auto_reconnect_     = true;
    bool   user_disconnected_  = false;
    bool   was_established_    = false;
    static constexpr int kMaxReconnectAttempts = 30;

    opentm::tm_core::tcp_connection conn_;
    frame_dispatcher                disp_;
    session_controller              session_{&conn_};
    host_file_server                files_{&conn_};
    file_explorer_controller        explorer_{&conn_, &session_};
    kernel_explorer_controller      kernel_{&conn_, &session_};
    debug_controller                debug_{&conn_, &session_};
    target_actions                  actions_{&conn_, &session_};
    target_record                   record_;
};

} // namespace opentm::tm_ui
