#pragma once

#include <tm_core/dbgp_codec.h>
#include <tm_core/deci3_codec.h>

#include <QHash>
#include <QList>
#include <QMetaType>
#include <QObject>
#include <QString>

#include <cstdint>
#include <optional>

namespace opentm::tm_core { class tcp_connection; }

namespace opentm::tm_ui {

class session_controller;

class kernel_explorer_controller : public QObject {
    Q_OBJECT
public:
    explicit kernel_explorer_controller(opentm::tm_core::tcp_connection* conn, session_controller* session, QObject* parent = nullptr);
    ~kernel_explorer_controller() override;
    struct process_summary {
        std::uint32_t                       pid = 0;
        opentm::tm_core::dbgp::process_info info;
        opentm::tm_core::dbgp::user_memory_stat memory;
    };

public slots:
    void refresh_process_list();
    void refresh_threads(std::uint32_t pid);
    void refresh_modules(std::uint32_t pid);
    void refresh_mutexes(std::uint32_t pid);
    void refresh_lwmutexes(std::uint32_t pid);
    void refresh_conds(std::uint32_t pid);
    void refresh_event_queues(std::uint32_t pid);
    void refresh_containers(std::uint32_t pid);
    void trigger_core_dump(std::uint32_t pid);
    void resume_process(std::uint32_t pid);
    void pause_process(std::uint32_t pid);
    void terminate_process(std::uint32_t pid);
    void on_frame_received(opentm::tm_core::deci3_frame f);

signals:
    void log_message(QString line);
    void process_list_ready(QList<process_summary> processes);
    void threads_ready(std::uint32_t pid, QList<opentm::tm_core::dbgp::ppu_thread_info> threads);
    void modules_ready(std::uint32_t pid, QList<opentm::tm_core::dbgp::prx_info> modules);
    void mutexes_ready(std::uint32_t pid, QList<opentm::tm_core::dbgp::mutex_info> entries);
    void lwmutexes_ready(std::uint32_t pid, QList<opentm::tm_core::dbgp::lwmutex_info> entries);
    void conds_ready(std::uint32_t pid, QList<opentm::tm_core::dbgp::cond_info> entries);
    void event_queues_ready(std::uint32_t pid, QList<opentm::tm_core::dbgp::event_queue_info> entries);
    void containers_ready(std::uint32_t pid, QList<opentm::tm_core::dbgp::container_info> entries);

private:
    struct pending {
        std::uint32_t cmd = 0;
        std::uint32_t pid = 0;
        std::uint32_t handle = 0;
        std::uint32_t round = 0;
    };

    template <class Info>
    struct sync_accum {
        std::uint32_t pid = 0;
        QList<std::uint32_t> handles_remaining;
        QList<Info> info;
    };

    std::uint32_t send_dbgp(std::uint32_t cmd, std::uint32_t pid, const std::vector<std::byte>& body, std::uint32_t handle = 0, std::uint32_t round = 0);

    void on_process_list_reply(const opentm::tm_core::dbgp::response& r);
    void on_process_info_reply(const opentm::tm_core::dbgp::response& r, std::uint32_t pid);
    void on_user_memory_stat_reply(const opentm::tm_core::dbgp::response& r, std::uint32_t pid);
    void on_thread_list_reply(const opentm::tm_core::dbgp::response& r, std::uint32_t pid);
    void on_thread_info_reply(const opentm::tm_core::dbgp::response& r, std::uint32_t pid, std::uint64_t tid, std::uint32_t round);
    template <class Info>
    void on_info_list_reply(const opentm::tm_core::dbgp::response& r, std::uint32_t pid, std::uint32_t info_cmd, QList<std::uint32_t>& expected_set, QList<Info>& accumulator);
    template <class Info>
    void refresh_sync(sync_accum<Info>& st, std::uint32_t pid, std::uint32_t list_cmd);
    template <class Info>
    void on_sync_info(sync_accum<Info>& st, std::optional<Info> parsed, std::uint32_t handle, void (kernel_explorer_controller::*ready)(std::uint32_t, QList<Info>));

    void maybe_emit_process_list();

    opentm::tm_core::tcp_connection* connection_ = nullptr;
    session_controller*              session_    = nullptr;
    QHash<std::uint32_t, pending> pending_;

    struct {
        QList<process_summary> summaries;
        QList<std::uint32_t> remaining_pids;
        bool got_list = false;
    } process_state_;

    struct {
        std::uint32_t pid = 0;
        int expected = 0;
        int received = 0;
        std::uint32_t round = 0;
        QList<opentm::tm_core::dbgp::ppu_thread_info> info;
    } threads_state_;

    sync_accum<opentm::tm_core::dbgp::mutex_info>       mutexes_state_;
    sync_accum<opentm::tm_core::dbgp::lwmutex_info>     lwmutexes_state_;
    sync_accum<opentm::tm_core::dbgp::cond_info>        conds_state_;
    sync_accum<opentm::tm_core::dbgp::event_queue_info> evq_state_;

    struct {
        std::uint32_t pid = 0;
        int expected = 0;
        int received = 0;
        QList<opentm::tm_core::dbgp::prx_info> info;
    } modules_state_;
};

} // namespace opentm::tm_ui

Q_DECLARE_METATYPE(QList<opentm::tm_ui::kernel_explorer_controller::process_summary>)
