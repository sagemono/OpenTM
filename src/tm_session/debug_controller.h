#pragma once

#include <tm_core/dbgp_codec.h>
#include <tm_core/deci3_codec.h>

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QMetaType>
#include <QObject>
#include <QSet>
#include <QString>

#include <cstdint>

namespace opentm::tm_core { class tcp_connection; }

namespace opentm::tm_ui {

class session_controller;

class debug_controller : public QObject {
    Q_OBJECT
public:
    explicit debug_controller(opentm::tm_core::tcp_connection* conn, session_controller* session, QObject* parent = nullptr);
    ~debug_controller() override;

    static constexpr std::uint32_t memory_chunk = 0x100;

    // an arbitrary window becomes this many aligned reads
    static constexpr quint64 chunk_base(quint64 address) noexcept {
        return address & ~static_cast<quint64>(memory_chunk - 1);
    }
    static constexpr int chunk_count(quint64 address, quint32 length) noexcept {
        if (length == 0) return 0;
        return static_cast<int>((chunk_base(address + length - 1) - chunk_base(address)) / memory_chunk) + 1;
    }

    std::uint32_t process_id() const noexcept { return pid_; }
    bool is_running() const noexcept { return running_; }
    QList<quint64> breakpoints() const;
    bool has_breakpoint(std::uint64_t address) const;

public slots:
    void set_process(std::uint32_t pid);

    void attach_process();
    void forget_state();

    void read_memory(quint64 address, quint32 length);
    void write_memory(quint64 address, QByteArray data);

    void read_registers(quint64 thread_id);
    void write_gpr(quint64 thread_id, unsigned index, quint64 value);

    void set_breakpoint(quint64 address);
    void clear_breakpoint(quint64 address);

    void resume(QList<quint64> thread_ids);
    void halt(QList<quint64> thread_ids);
    //steps are temp breakpoints
    void step_to(quint64 thread_id, quint64 address);
    //a conditional branch needs covering at both the target and the fallthru, since which one is taken is not known until it runs
    void step_to_any(quint64 thread_id, QList<quint64> addresses);

    void on_frame_received(opentm::tm_core::deci3_frame f);

signals:
    void log_message(QString line);
    void memory_ready(quint64 address, QByteArray data);
    void memory_read_failed(quint64 address, quint32 status);
    void memory_written(quint64 address, quint32 status);
    void registers_ready(quint64 thread_id, opentm::tm_core::dbgp::ppu_registers regs);
    void registers_written(quint64 thread_id, quint32 status);
    void breakpoint_added(quint64 address, quint32 status);
    void breakpoint_removed(quint64 address, quint32 status);
    void thread_stopped(quint64 thread_id, quint64 address, quint32 reason);
    void running_changed(bool running);
    void halt_finished(quint32 status);
    void process_changed(quint32 pid);

private:
    enum class kind {
        read_memory,
        write_memory,
        read_registers,
        write_registers,
        set_breakpoint,
        clear_breakpoint,
        clear_step_breakpoint,
        attach,
        resume,
        halt,
        step,
    };

    struct pending {
        kind          what = kind::read_memory;
        std::uint64_t address   = 0;
        std::uint64_t thread_id = 0;
        std::uint32_t group     = 0;
    };

    // one ui level read fanned out over however many aligned chunks it covers...
    struct read_group {
        std::uint64_t address = 0;
        std::uint32_t length  = 0;
        int           outstanding = 0;
        bool          failed  = false;
        QHash<quint64, QByteArray> chunks;
    };

    std::uint32_t send_dbgp(std::uint32_t cmd, const std::vector<std::byte>& body, const pending& p);
    void set_running(bool on);
    void on_memory_chunk(const opentm::tm_core::dbgp::response& r, const pending& p);
    void on_stop_event(const opentm::tm_core::dbgp::response& r);
    void finish_read_group(std::uint32_t group);

    opentm::tm_core::tcp_connection* connection_ = nullptr;
    session_controller*              session_    = nullptr;
    std::uint32_t                    pid_        = 0;
    bool                             running_    = false;
    std::uint32_t                    next_group_ = 1;

    QHash<std::uint32_t, pending>    pending_;
    QHash<std::uint32_t, read_group> reads_;
    QSet<quint64>                    breakpoints_;
    // which threads we stopped, so a step can put back exactly those. the kit acks a partial thread list and then ignores it
    QList<quint64>                   halted_threads_;
    // step breakpoints are ours, not the users and they are cleared on the stop[
    QSet<quint64>                    step_breakpoints_;
};

} // namespace opentm::tm_ui

Q_DECLARE_METATYPE(opentm::tm_core::dbgp::ppu_registers)
