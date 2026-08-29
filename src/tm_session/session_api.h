#pragma once

#include "target_record.h"

#include <QByteArray>

#include <tm_core/dbgp_codec.h>
#include <tm_core/dfmp_codec.h>
#include <tm_core/tcp_connection.h>

#include <QList>
#include <QObject>
#include <QString>

#include <cstdint>
#include <vector>

namespace opentm::tm_ui {

class session_api : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    ~session_api() override = default;

    virtual void set_target(const target_record& r) = 0;
    virtual void clear_target() = 0;
    virtual const target_record& target() const = 0;    
    virtual void set_file_serving_dir(const QString& dir) = 0;
    virtual void set_auto_reconnect(bool /*on*/) {}
    virtual void connect_to_target() = 0;
    virtual void disconnect_from_target() = 0;
    // drop the link and, for a server-backed session, release the target
    virtual void close_target() { disconnect_from_target(); }
    virtual bool is_connected() const = 0;
    virtual bool is_session_ready() const = 0;
    virtual QString peer_summary() const = 0;        
    virtual opentm::tm_core::tcp_connection::state connection_state() const = 0;
    virtual void power_on() = 0;
    virtual void power_off() = 0;
    virtual void power_off_force() = 0;
    virtual void reset_current() = 0;
    virtual void wake_on_lan() = 0;
    virtual void load_executable(const QString& path, const load_options& o) = 0;
    virtual void install_package(const QString& host_path) = 0;
    virtual void settings_refresh() = 0;
    virtual void settings_apply(const QString& host_path, std::uint32_t size) = 0;
    virtual void settings_commit() = 0;
    virtual void list_directory(const QString& path) = 0;
    // file xfers are performed by the target against the host file server, so these only issue the request
    virtual void download_file(const QString& kit_path, const QString& host_path, std::uint32_t size) = 0;
    virtual void upload_file(const QString& host_path, const QString& kit_path) = 0;
    virtual void delete_file(const QString& kit_path) = 0;
    virtual void rename_file(const QString& from, const QString& to) = 0;
    virtual void make_directory(const QString& kit_path, std::uint32_t mode) = 0;
    virtual void set_permissions(const QString& kit_path, std::uint32_t mode) = 0;
    virtual void set_times(const QString& kit_path, std::uint64_t atime, std::uint64_t mtime) = 0;
    // cp side: which logical partitions exist and their state.
    virtual void refresh_lpar_status() = 0;
    virtual void refresh_process_list() = 0;
    virtual void refresh_threads(std::uint32_t pid) = 0;
    virtual void refresh_modules(std::uint32_t pid) = 0;
    virtual void refresh_mutexes(std::uint32_t pid) = 0;
    virtual void refresh_lwmutexes(std::uint32_t pid) = 0;
    virtual void refresh_conds(std::uint32_t pid) = 0;
    virtual void refresh_event_queues(std::uint32_t pid) = 0;
    virtual void refresh_containers(std::uint32_t pid) = 0;
    virtual void pause_process(std::uint32_t pid) = 0;
    virtual void resume_process(std::uint32_t pid) = 0;
    virtual void terminate_process(std::uint32_t pid) = 0;
    virtual void trigger_core_dump(std::uint32_t pid) = 0;

    // Debugger
    virtual bool supports_debugger() const { return false; }
    virtual void debug_read_memory(quint64 /*address*/, quint32 /*length*/) {}
    virtual void debug_write_memory(quint64 /*address*/, QByteArray /*data*/) {}
    virtual void debug_read_registers(quint64 /*thread_id*/) {}
    virtual void debug_write_gpr(quint64 /*thread_id*/, unsigned /*index*/, quint64 /*value*/) {}
    virtual void debug_set_breakpoint(quint64 /*address*/) {}
    virtual void debug_clear_breakpoint(quint64 /*address*/) {}
    virtual void debug_resume(QList<quint64> /*thread_ids*/) {}
    virtual void debug_halt(QList<quint64> /*thread_ids*/) {}
    virtual void debug_step_to(quint64 /*thread_id*/, QList<quint64> /*addresses*/) {}
    virtual void debug_set_process(std::uint32_t /*pid*/) {}

    struct process_summary {
        std::uint32_t                           pid = 0;
        opentm::tm_core::dbgp::process_info     info;
        opentm::tm_core::dbgp::user_memory_stat memory;
    };

signals:
    void log_message(QString line);
    void status_message(QString text);
    void wire_line(QString line);
    void tty_text(QString text);
    void tty_stream_text(std::uint8_t stream, QString text);
    void clear_console();
    void connection_state_changed(opentm::tm_core::tcp_connection::state s);
    void session_ready(std::uint16_t token, std::uint16_t sub_token);
    void session_invalidated();
    void debug_agent_ready();
    void boot_mode_changed(QString name);
    void error(QString message);
    // a protocol the kit refused to register on this connection, usually
    // because another target manager still owns it
    void protocol_rejected(quint32 proto, QString name, quint8 status);
    void sdk_version_received(QString sdk);
    void cp_version_received(QString cp);
    void directory_listed(QString path, std::vector<opentm::tm_core::dfmp_file_entry> entries);
    void process_list_ready(QList<process_summary> processes);
    void threads_ready(std::uint32_t pid, QList<opentm::tm_core::dbgp::ppu_thread_info> items);
    void modules_ready(std::uint32_t pid, QList<opentm::tm_core::dbgp::prx_info> items);
    void mutexes_ready(std::uint32_t pid, QList<opentm::tm_core::dbgp::mutex_info> items);
    void lwmutexes_ready(std::uint32_t pid, QList<opentm::tm_core::dbgp::lwmutex_info> items);
    void conds_ready(std::uint32_t pid, QList<opentm::tm_core::dbgp::cond_info> items);
    void event_queues_ready(std::uint32_t pid, QList<opentm::tm_core::dbgp::event_queue_info> items);
    void containers_ready(std::uint32_t pid, QList<opentm::tm_core::dbgp::container_info> items);
    void load_ext_reply(std::uint32_t lv2_status);
    void install_progress(int percent);
    void install_finished(QString installed_path);
    void install_reply(std::uint32_t lv2_status);
    void target_went_down(QString reason);
    void file_op_finished(QString op, quint32 status);
    void transfer_finished();
    void transfer_failed(std::uint32_t result);

    void debug_memory_ready(quint64 address, QByteArray data);
    void debug_memory_read_failed(quint64 address, quint32 status);
    void debug_memory_written(quint64 address, quint32 status);
    void debug_registers_ready(quint64 thread_id, opentm::tm_core::dbgp::ppu_registers regs);
    void debug_registers_written(quint64 thread_id, quint32 status);
    void debug_breakpoint_added(quint64 address, quint32 status);
    void debug_breakpoint_removed(quint64 address, quint32 status);
    void debug_thread_stopped(quint64 thread_id, quint64 address, quint32 reason);
    void debug_running_changed(bool running);
    void debug_halt_finished(quint32 status);
    void debug_process_changed(quint32 pid);
};

} // namespace opentm::tm_ui
