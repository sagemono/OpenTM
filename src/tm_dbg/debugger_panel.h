#pragma once

#include <tm_core/dbgp_codec.h>
#include <tm_core/ppc_disasm.h>
#include <tm_core/ppc_stack.h>

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QSet>
#include <QString>
#include <QWidget>

#include <cstdint>
#include <optional>
#include <vector>

class QAction;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QTableWidget;
class QTabWidget;
class QTreeWidget;
class QTreeWidgetItem;

namespace opentm::tm_ui {

class debugger_panel : public QWidget {
    Q_OBJECT
public:
    explicit debugger_panel(QWidget* parent = nullptr);

    void set_supported(bool on, const QString& why = {});

public slots:
    void on_threads_ready(std::uint32_t pid, QList<opentm::tm_core::dbgp::ppu_thread_info> threads);
    void on_modules_ready(std::uint32_t pid, QList<opentm::tm_core::dbgp::prx_info> modules);
    void on_process_ready(quint32 pid, opentm::tm_core::dbgp::process_info info);
    void on_memory_ready(quint64 address, QByteArray data);
    void on_memory_read_failed(quint64 address, quint32 status);
    void on_registers_ready(quint64 thread_id, opentm::tm_core::dbgp::ppu_registers regs);
    void on_breakpoint_added(quint64 address, quint32 status);
    void on_breakpoint_removed(quint64 address, quint32 status);
    void on_thread_stopped(quint64 thread_id, quint64 address, quint32 reason);
    void on_running_changed(bool running);
    void on_halt_finished(quint32 status);
    void on_session_invalidated();
    void on_process_changed(quint32 pid);

signals:
    void log_message(QString line);
    void memory_requested(quint64 address, quint32 length);
    void memory_write_requested(quint64 address, QByteArray data);
    void registers_requested(quint64 thread_id);
    void gpr_write_requested(quint64 thread_id, unsigned index, quint64 value);
    void breakpoint_add_requested(quint64 address);
    void breakpoint_clear_requested(quint64 address);
    void resume_requested(QList<quint64> thread_ids);
    void halt_requested(QList<quint64> thread_ids);
    void step_requested(quint64 thread_id, QList<quint64> addresses);
    void threads_requested(std::uint32_t pid);
    void modules_requested(std::uint32_t pid);
    void thread_list_refresh_requested();

private:
    void build_ui();
    void refresh_actions();
    void go_to_address();
    void request_disassembly(quint64 address);
    void toggle_breakpoint_here();
    void step_into();
    void step_over();
    void do_resume();
    void do_halt();
    void edit_register(int row, int column);
    void thread_selected();
    void request_callstack();
    void show_callstack(const QByteArray& stack);
    void stack_frame_activated();
    void go_to_memory();
    void show_memory(quint64 address, const QByteArray& data);
    void show_registers(const opentm::tm_core::dbgp::ppu_registers& regs);
    void redraw_disassembly();
    quint64 selected_address() const;
    QList<quint64> process_threads() const;
    std::optional<quint64> step_destination(quint64 pc, bool over) const;

    opentm::tm_core::ppc_disassembler disasm_;

    quint64                              code_base_ = 0;
    QByteArray                           code_;
    std::vector<opentm::tm_core::ppc_insn> insns_;

    quint64 pc_        = 0;
    quint64 thread_id_ = 0;
    bool    running_   = false;
    bool    stopped_   = false;
    bool    supported_ = true;
    QSet<quint64> breakpoints_;
    QList<quint64> all_threads_;
    bool    auto_halted_ = false;
    quint64 reported_thread_ = 0;
    std::uint32_t reported_state_ = 0xffffffffu;

    opentm::tm_core::dbgp::ppu_registers regs_{};
    quint32 last_read_status_ = 0;
    quint64 stack_base_ = 0;
    bool    stack_pending_ = false;
    quint64 stack_top_ = 0;
    quint64 memory_base_    = 0;
    bool    memory_pending_ = false;
    bool                                 have_regs_ = false;

    QLineEdit*      address_edit_ = nullptr;
    QTreeWidget*    code_view_    = nullptr;
    QTableWidget*   regs_view_    = nullptr;
    QTreeWidget*    threads_view_ = nullptr;
    QTreeWidget*    modules_view_ = nullptr;
    QTreeWidget*    process_view_ = nullptr;
    QTreeWidget*    stack_view_   = nullptr;
    QTreeWidget*    memory_view_  = nullptr;
    QLineEdit*      memory_edit_  = nullptr;
    QLabel*         status_       = nullptr;
    QPushButton*    resume_btn_   = nullptr;
    QPushButton*    halt_btn_     = nullptr;
    QPushButton*    step_into_btn_ = nullptr;
    QPushButton*    step_over_btn_ = nullptr;
    QPushButton*    bp_btn_        = nullptr;
};

} // namespace opentm::tm_ui
