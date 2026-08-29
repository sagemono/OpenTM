#pragma once

#include "rpc_client.h"
#include "session_api.h"

#include <QJsonObject>

namespace opentm::tm_ui {

class remote_session : public session_api {
    Q_OBJECT
public:
    explicit remote_session(QObject* parent = nullptr);
    ~remote_session() override;

    bool attach_tcp(const QString& host, quint16 port);
    bool attach_local(const QString& name);
    bool is_attached() const { return rpc_.is_connected(); }

    void set_target(const target_record& r) override;
    void clear_target() override;
    const target_record& target() const override { return record_; }

    void set_file_serving_dir(const QString& dir) override;
    void connect_to_target() override;
    void disconnect_from_target() override;
    void close_target() override;

    bool is_connected() const override      { return connected_; }
    bool is_session_ready() const override  { return session_ready_; }
    QString peer_summary() const override   { return peer_; }
    opentm::tm_core::tcp_connection::state connection_state() const override {
        return state_;
    }

    void power_on() override;
    void power_off() override;
    void power_off_force() override;
    void reset_current() override;
    void wake_on_lan() override;

    void load_executable(const QString& path, const load_options& o) override;
    void install_package(const QString& host_path) override;

    void settings_refresh() override;
    void settings_apply(const QString& host_path, std::uint32_t size) override;
    void settings_commit() override;

    void list_directory(const QString& path) override;
    void download_file(const QString& kit_path, const QString& host_path, std::uint32_t size) override;
    void upload_file(const QString& host_path, const QString& kit_path) override;
    void delete_file(const QString& kit_path) override;
    void rename_file(const QString& from, const QString& to) override;
    void make_directory(const QString& kit_path, std::uint32_t mode) override;
    void set_permissions(const QString& kit_path, std::uint32_t mode) override;
    void set_times(const QString& kit_path, std::uint64_t atime, std::uint64_t mtime) override;

    void refresh_lpar_status() override;
    void refresh_process_list() override;
    void refresh_threads(std::uint32_t pid) override;
    void refresh_modules(std::uint32_t pid) override;
    void refresh_mutexes(std::uint32_t pid) override;
    void refresh_lwmutexes(std::uint32_t pid) override;
    void refresh_conds(std::uint32_t pid) override;
    void refresh_event_queues(std::uint32_t pid) override;
    void refresh_containers(std::uint32_t pid) override;

    void pause_process(std::uint32_t pid) override;
    void resume_process(std::uint32_t pid) override;
    void terminate_process(std::uint32_t pid) override;
    void trigger_core_dump(std::uint32_t pid) override;

    bool supports_debugger() const override { return true; }
    void debug_read_memory(quint64 address, quint32 length) override;
    void debug_write_memory(quint64 address, QByteArray data) override;
    void debug_read_registers(quint64 thread_id) override;
    void debug_write_gpr(quint64 thread_id, unsigned index, quint64 value) override;
    void debug_set_breakpoint(quint64 address) override;
    void debug_clear_breakpoint(quint64 address) override;
    void debug_resume(QList<quint64> thread_ids) override;
    void debug_halt(QList<quint64> thread_ids) override;
    void debug_step_to(quint64 thread_id, QList<quint64> addresses) override;
    void debug_set_process(std::uint32_t pid) override;

signals:
    void server_lost(QString reason);

private:
    void wire_events();
    void refresh_status();
    void adopt_state(const QJsonObject& r);
    void send(const QString& method, QJsonObject params = {});
    void on_event(const QString& target, const QString& name, const QJsonObject& params);

    rpc_client    rpc_;
    target_record record_;
    QString       handle_;   // server-side session key, stable across renames
    bool          connected_     = false;
    bool          session_ready_ = false;
    QString       peer_;
    QString       server_gone_reason_;
    opentm::tm_core::tcp_connection::state state_ =
        opentm::tm_core::tcp_connection::state::disconnected;
};

} // namespace opentm::tm_ui
