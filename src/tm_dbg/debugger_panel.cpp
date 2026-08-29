#include "debugger_panel.h"

#include <QAbstractItemView>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QPalette>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QDockWidget>
#include <QSettings>
#include <QSizePolicy>
#include <QToolBar>

#include <algorithm>
#include <QSplitter>
#include <QStringList>
#include <QTabWidget>
#include <QTableWidget>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace opentm::tm_ui {

namespace {

constexpr quint32 kCodeWindow = 0x200;
constexpr quint32 stack_window = 0x800; // enough?
constexpr quint32 memory_window = 0x200;

QString hex64(quint64 v) {
    return QStringLiteral("0x%1").arg(v, 16, 16, QChar('0'));
}

QString hex_short(quint64 v) {
    return QStringLiteral("0x%1").arg(v, 8, 16, QChar('0'));
}

enum code_column { col_marker = 0, col_address, col_opcode, col_text, col_symbol };

QString decode_cr(std::uint32_t cr) {
    QStringList fields;
    for (int i = 0; i < 8; ++i) {
        const auto f = (cr >> (28 - 4 * i)) & 0xfu;
        QString flags;
        if (f & 0x8u) flags += QStringLiteral("LT");
        if (f & 0x4u) flags += QStringLiteral("GT");
        if (f & 0x2u) flags += QStringLiteral("EQ");
        if (f & 0x1u) flags += QStringLiteral("SO");
        fields << QStringLiteral("cr%1=%2").arg(i).arg(flags);
    }
    return fields.join(QLatin1Char(' '));
}

constexpr std::uint32_t thread_state_stop = 6;

} // namespace

debugger_panel::debugger_panel(QWidget* parent) : QWidget(parent) {
    hide();
    build_ui();
    refresh_actions();
}

void debugger_panel::add_pane(const QString& title, const QString& id, QWidget* body, Qt::DockWidgetArea area)
{
    panes_.append(pane_spec{title, id, body, area});
}

void debugger_panel::build_ui() {
    const auto mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);

    auto* bar = new QToolBar(tr("Run"), this);
    toolbar_ = bar;
    bar->setObjectName(QStringLiteral("run_toolbar"));
    bar->setMovable(false);
    bar->addWidget(new QLabel(tr("Address "), this));

    address_edit_ = new QLineEdit(this);
    address_edit_->setPlaceholderText(QStringLiteral("0x10700"));
    address_edit_->setMaximumWidth(180);
    bar->addWidget(address_edit_);

    auto* go = new QPushButton(tr("Go"), this);
    bar->addWidget(go);
    bar->addSeparator();

    resume_btn_    = new QPushButton(tr("Resume"), this);
    halt_btn_      = new QPushButton(tr("Halt"), this);
    step_into_btn_ = new QPushButton(tr("Step Into"), this);
    step_over_btn_ = new QPushButton(tr("Step Over"), this);
    bp_btn_        = new QPushButton(tr("Toggle Breakpoint"), this);
    for (auto* b : {resume_btn_, halt_btn_, step_into_btn_, step_over_btn_, bp_btn_}) {
        bar->addWidget(b);
    }

    auto* spacer = new QWidget(this);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    bar->addWidget(spacer);
    status_ = new QLabel(this);
    bar->addWidget(status_);

    code_view_ = new QTreeWidget(this);
    code_view_->setRootIsDecorated(false);
    code_view_->setUniformRowHeights(true);
    code_view_->setFont(mono);
    code_view_->setColumnCount(5);
    code_view_->setHeaderLabels({QString(), tr("Address"), tr("Opcode"),
                                 tr("Disassembly"), tr("Function")});
    code_view_->header()->setStretchLastSection(true);
    code_view_->setColumnWidth(col_marker, 28);
    code_view_->setColumnWidth(col_address, 150);
    code_view_->setColumnWidth(col_opcode, 90);

    regs_view_ = new QTableWidget(this);
    regs_view_->setFont(mono);
    regs_view_->setColumnCount(2);
    regs_view_->setHorizontalHeaderLabels({tr("Register"), tr("Value")});
    regs_view_->verticalHeader()->setVisible(false);
    regs_view_->horizontalHeader()->setStretchLastSection(true);
    regs_view_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    add_pane(tr("Registers"), QStringLiteral("pane_registers"), regs_view_, Qt::RightDockWidgetArea);

    stack_view_ = new QTreeWidget(this);
    stack_view_->setRootIsDecorated(false);
    stack_view_->setFont(mono);
    stack_view_->setColumnCount(4);
    stack_view_->setHeaderLabels({tr("#"), tr("Address"), tr("Function"), tr("Frame")});
    add_pane(tr("Callstack"), QStringLiteral("pane_callstack"), stack_view_, Qt::BottomDockWidgetArea);

    threads_view_ = new QTreeWidget(this);
    threads_view_->setRootIsDecorated(false);
    threads_view_->setColumnCount(5);
    threads_view_->setHeaderLabels({tr("Thread"), tr("State"), tr("Name"), tr("Priority"), tr("Stack")});
    add_pane(tr("Threads"), QStringLiteral("pane_threads"), threads_view_, Qt::BottomDockWidgetArea);

    modules_view_ = new QTreeWidget(this);
    modules_view_->setRootIsDecorated(false);
    modules_view_->setColumnCount(5);
    modules_view_->setHeaderLabels({tr("Module"), tr("Attrib"), tr("Start"), tr("Stop"), tr("Size")});
    add_pane(tr("Modules"), QStringLiteral("pane_modules"), modules_view_, Qt::BottomDockWidgetArea);

    process_view_ = new QTreeWidget(this);
    process_view_->setRootIsDecorated(false);
    process_view_->setColumnCount(2);
    process_view_->setHeaderLabels({tr("Property"), tr("Value")});
    add_pane(tr("Process"), QStringLiteral("pane_process"),
                                  process_view_, Qt::BottomDockWidgetArea);

    auto* memory_page = new QWidget(this);
    auto* memory_layout = new QVBoxLayout(memory_page);
    memory_layout->setContentsMargins(2, 2, 2, 2);
    auto* memory_bar = new QHBoxLayout;
    memory_bar->addWidget(new QLabel(tr("Address"), memory_page));
    memory_edit_ = new QLineEdit(memory_page);
    memory_edit_->setPlaceholderText(QStringLiteral("0xd0100000"));
    memory_bar->addWidget(memory_edit_);
    auto* memory_go = new QPushButton(tr("Show"), memory_page);
    memory_bar->addWidget(memory_go);
    memory_layout->addLayout(memory_bar);

    memory_view_ = new QTreeWidget(memory_page);
    memory_view_->setRootIsDecorated(false);
    memory_view_->setUniformRowHeights(true);
    memory_view_->setFont(mono);
    memory_view_->setColumnCount(3);
    memory_view_->setHeaderLabels({tr("Address"), tr("Hex"), tr("ASCII")});
    memory_layout->addWidget(memory_view_, 1);
    add_pane(tr("Memory"), QStringLiteral("pane_memory"), memory_page, Qt::RightDockWidgetArea);

    auto* bp_page = new QWidget(this);
    auto* bp_layout = new QVBoxLayout(bp_page);
    bp_layout->setContentsMargins(2, 2, 2, 2);

    bp_view_ = new QTreeWidget(bp_page);
    bp_view_->setRootIsDecorated(false);
    bp_view_->setFont(mono);
    bp_view_->setColumnCount(2);
    bp_view_->setHeaderLabels({tr("Address"), tr("Function")});
    bp_layout->addWidget(bp_view_, 1);

    auto* bp_bar = new QHBoxLayout;
    auto* bp_remove = new QPushButton(tr("Remove"), bp_page);
    auto* bp_remove_all = new QPushButton(tr("Remove All"), bp_page);
    bp_bar->addWidget(bp_remove);
    bp_bar->addWidget(bp_remove_all);
    bp_bar->addStretch(1);
    bp_edit_ = new QLineEdit(bp_page);
    bp_edit_->setPlaceholderText(QStringLiteral("0x48564"));
    bp_edit_->setMaximumWidth(140);
    bp_bar->addWidget(bp_edit_);
    auto* bp_clear_at = new QPushButton(tr("Clear at Address"), bp_page);
    bp_bar->addWidget(bp_clear_at);
    bp_layout->addLayout(bp_bar);

    add_pane(tr("Breakpoints"), QStringLiteral("pane_breakpoints"), bp_page,
             Qt::BottomDockWidgetArea);

    connect(bp_remove, &QPushButton::clicked, this, &debugger_panel::remove_selected_breakpoint);
    connect(bp_remove_all, &QPushButton::clicked, this, &debugger_panel::remove_all_breakpoints);
    connect(bp_clear_at, &QPushButton::clicked, this, &debugger_panel::clear_typed_breakpoint);
    connect(bp_edit_, &QLineEdit::returnPressed, this, &debugger_panel::clear_typed_breakpoint);
    connect(bp_view_, &QTreeWidget::itemDoubleClicked, this,
            [this](QTreeWidgetItem*, int) { breakpoint_activated(); });

    log_view_ = new QPlainTextEdit(this);
    log_view_->setReadOnly(true);
    log_view_->setMaximumBlockCount(5000);
    add_pane(tr("Log"), QStringLiteral("pane_log"), log_view_, Qt::BottomDockWidgetArea);

    connect(memory_go, &QPushButton::clicked, this, &debugger_panel::go_to_memory);
    connect(memory_edit_, &QLineEdit::returnPressed, this, &debugger_panel::go_to_memory);
    connect(stack_view_, &QTreeWidget::itemSelectionChanged, this, &debugger_panel::stack_frame_activated);
    connect(go, &QPushButton::clicked, this, &debugger_panel::go_to_address);
    connect(address_edit_, &QLineEdit::returnPressed, this, &debugger_panel::go_to_address);
    connect(resume_btn_, &QPushButton::clicked, this, &debugger_panel::do_resume);
    connect(halt_btn_, &QPushButton::clicked, this, &debugger_panel::do_halt);
    connect(step_into_btn_, &QPushButton::clicked, this, &debugger_panel::step_into);
    connect(step_over_btn_, &QPushButton::clicked, this, &debugger_panel::step_over);
    connect(bp_btn_, &QPushButton::clicked, this, &debugger_panel::toggle_breakpoint_here);
    connect(code_view_, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem*, int) { toggle_breakpoint_here(); });
    connect(regs_view_, &QTableWidget::cellDoubleClicked, this, &debugger_panel::edit_register);
    connect(code_view_, &QTreeWidget::itemSelectionChanged, this, &debugger_panel::refresh_actions);
    connect(threads_view_, &QTreeWidget::itemSelectionChanged, this, &debugger_panel::thread_selected);

}

QWidget* debugger_panel::code_widget() const { return code_view_; }

void debugger_panel::refresh_breakpoints() {
    if (!bp_view_) return;
    bp_view_->clear();
    auto sorted = breakpoints_.values();
    std::sort(sorted.begin(), sorted.end());
    for (auto address : sorted) {
        auto* item = new QTreeWidgetItem(bp_view_);
        item->setData(0, Qt::UserRole, QVariant::fromValue<qulonglong>(address));
        item->setText(0, hex64(address));
        item->setText(1, symbol_for(address));
    }
    for (int c = 0; c < bp_view_->columnCount(); ++c) bp_view_->resizeColumnToContents(c);
}

void debugger_panel::remove_selected_breakpoint() {
    const auto* item = bp_view_->currentItem();
    if (!item) return;
    bool ok = false;
    const auto address = item->data(0, Qt::UserRole).toULongLong(&ok);
    if (ok && address != 0) emit breakpoint_clear_requested(address);
}

void debugger_panel::remove_all_breakpoints() {
    const auto all = breakpoints_;
    for (auto address : all) emit breakpoint_clear_requested(address);
}

void debugger_panel::clear_typed_breakpoint() {
    auto text = bp_edit_->text().trimmed();
    if (text.isEmpty()) return;
    if (text.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive)) text = text.mid(2);
    bool ok = false;
    const auto address = text.toULongLong(&ok, 16);
    if (!ok) {
        emit log_message(tr("!! debugger: '%1' is not a hex address").arg(bp_edit_->text()));
        return;
    }
    emit log_message(tr("-- clearing any breakpoint at %1").arg(hex64(address)));
    emit breakpoint_clear_requested(address);
    bp_edit_->clear();
}

void debugger_panel::breakpoint_activated() {
    const auto* item = bp_view_->currentItem();
    if (!item) return;
    bool ok = false;
    const auto address = item->data(0, Qt::UserRole).toULongLong(&ok);
    if (ok && address != 0) show_code_at(address);
}

void debugger_panel::append_log_line(const QString& line) {
    if (log_view_) log_view_->appendPlainText(line);
}

void debugger_panel::set_supported(bool on, const QString& why) {
    supported_ = on;
    if (!on && !why.isEmpty()) status_->setText(why);
    refresh_actions();
}

void debugger_panel::refresh_actions() {
    const bool live = supported_;
    const bool have_pc      = live && have_regs_ && !running_;
    const bool have_threads = live && !process_threads().isEmpty();

    resume_btn_->setEnabled(have_threads);
    halt_btn_->setEnabled(have_threads || (live && thread_id_ != 0));
    step_into_btn_->setEnabled(have_pc);
    step_over_btn_->setEnabled(have_pc);
    bp_btn_->setEnabled(live && selected_address() != 0);
    address_edit_->setEnabled(live);

    if (!supported_) return;
    if (running_) {
        status_->setText(tr("running"));
    } else if (stopped_) {
        const auto named = symbol_for(pc_);
        status_->setText(named.isEmpty() ? tr("stopped at %1  thread %2").arg(hex64(pc_), hex_short(thread_id_)) : tr("stopped in %1  (%2)  thread %3").arg(named, hex64(pc_), hex_short(thread_id_)));
    } else if (have_regs_) {
        status_->setText(tr("thread %1 at %2").arg(hex_short(thread_id_), hex64(pc_)));
    } else if (thread_id_ != 0) {
        status_->setText(tr("thread %1 selected").arg(hex_short(thread_id_)));
    } else {
        status_->setText(tr("pick a thread"));
    }
}

quint64 debugger_panel::selected_address() const {
    const auto* item = code_view_->currentItem();
    if (!item) return 0;
    bool ok = false;
    const auto v = item->data(col_address, Qt::UserRole).toULongLong(&ok);
    return ok ? v : 0;
}

void debugger_panel::go_to_address() {
    bool ok = false;
    auto text = address_edit_->text().trimmed();
    if (text.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive)) text = text.mid(2);
    if (text.isEmpty()) return;
    const auto address = text.toULongLong(&ok, 16);
    if (!ok) {
        emit log_message(tr("!! debugger: '%1' is not a hex address").arg(address_edit_->text()));
        return;
    }
    request_disassembly(address);
}

void debugger_panel::request_disassembly(quint64 address) {
    emit memory_requested(address, kCodeWindow);
}

void debugger_panel::show_code_at(quint64 address) {
    focus_address_ = address;
    const quint64 chunk = 0x100;
    const auto base = (address >= chunk) ? ((address - chunk) & ~(chunk - 1)) : 0;
    request_disassembly(base);
}

void debugger_panel::on_memory_ready(quint64 address, QByteArray bytes) {
    if (stack_requests_.remove(address)) {
        show_callstack(address, bytes);
        return;
    }
    if (memory_requests_.remove(address)) {
        show_memory(address, bytes);
        return;
    }

    code_base_ = address;
    code_      = std::move(bytes);

    const std::span<const std::byte> view(
        reinterpret_cast<const std::byte*>(code_.constData()),
        static_cast<std::size_t>(code_.size()));
    insns_ = disasm_.disassemble(view, code_base_);
    redraw_disassembly();
}

QString debugger_panel::symbol_for(quint64 address) const {
    const auto named = symbols_.describe(address);
    return named ? QString::fromStdString(*named) : QString();
}

bool debugger_panel::load_symbols(const QString& path, QString* error) {
    std::string err;
    if (!symbols_.load(path.toStdString(), &err)) {
        if (error) *error = QString::fromStdString(err);
        emit log_message(tr("!! symbols: %1").arg(QString::fromStdString(err)));
        return false;
    }
    emit log_message(tr("-- %1 function symbols from %2").arg(symbols_.size()).arg(path));
    redraw_disassembly();
    refresh_actions();
    return true;
}

void debugger_panel::request_callstack() {
    stack_view_->clear();
    if (!have_regs_) return;

    for (int i = 0; i < threads_view_->topLevelItemCount(); ++i) {
        auto* item = threads_view_->topLevelItem(i);
        if (item->data(0, Qt::UserRole).toULongLong() == thread_id_) {
            stack_top_ = item->data(4, Qt::UserRole).toULongLong();
            break;
        }
    }

    //r1 = sp
    const auto sp = regs_.gpr[1];
    if (sp == 0) return;

    auto window = stack_window;
    if (stack_top_ > sp && (stack_top_ - sp) < window) {
        window = static_cast<quint32>(stack_top_ - sp);
    }
    if (window < 32) return;

    stack_requests_.insert(sp);
    emit memory_requested(sp, window);
}

void debugger_panel::show_callstack(quint64 base, const QByteArray& stack) {
    const std::span<const std::byte> view(
        reinterpret_cast<const std::byte*>(stack.constData()),
        static_cast<std::size_t>(stack.size()));
    const auto frames = opentm::tm_core::walk_stack(view, base, pc_, base);

    stack_view_->clear();
    int level = 0;
    for (const auto& f : frames) {
        auto* item = new QTreeWidgetItem(stack_view_);
        item->setData(1, Qt::UserRole, QVariant::fromValue<qulonglong>(f.address));
        item->setText(0, QString::number(level++));
        item->setText(1, hex64(f.address));
        item->setText(2, symbol_for(f.address));
        item->setText(3, hex64(f.frame));
    }
    for (int c = 0; c < stack_view_->columnCount(); ++c) stack_view_->resizeColumnToContents(c);
}

void debugger_panel::stack_frame_activated() {
    const auto* item = stack_view_->currentItem();
    if (!item) return;
    bool ok = false;
    const auto address = item->data(1, Qt::UserRole).toULongLong(&ok);
    if (ok && address != 0) show_code_at(address);
}

void debugger_panel::go_to_memory() {
    auto text = memory_edit_->text().trimmed();
    if (text.isEmpty()) return;
    if (text.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive)) text = text.mid(2);
    bool ok = false;
    const auto address = text.toULongLong(&ok, 16);
    if (!ok) {
        emit log_message(tr("!! debugger: '%1' is not a hex address").arg(memory_edit_->text()));
        return;
    }
    memory_requests_.insert(address);
    emit memory_requested(address, memory_window);
}

void debugger_panel::show_memory(quint64 address, const QByteArray& data) {
    memory_view_->clear();
    constexpr int per_row = 16;
    for (int offset = 0; offset < data.size(); offset += per_row) {
        const auto row = data.mid(offset, per_row);
        QString hex, ascii;
        for (int i = 0; i < row.size(); ++i) {
            const auto b = static_cast<unsigned char>(row.at(i));
            hex += QStringLiteral("%1 ").arg(b, 2, 16, QChar('0'));
            ascii += (b >= 0x20 && b < 0x7f) ? QChar(b) : QChar('.');
        }
        auto* item = new QTreeWidgetItem(memory_view_);
        item->setText(0, hex64(address + static_cast<quint64>(offset)));
        item->setText(1, hex.trimmed());
        item->setText(2, ascii);
    }
    for (int c = 0; c < memory_view_->columnCount(); ++c) memory_view_->resizeColumnToContents(c);
}

void debugger_panel::on_memory_read_failed(quint64 address, quint32 status) {
    if (memory_requests_.remove(address)) {
        emit log_message(tr("!! debugger: cannot read %1 - not mapped?").arg(hex64(address)));
        return;
    }
    if (stack_requests_.remove(address)) {
        emit log_message(tr("!! debugger: could not read the stack at %1, so there is no callstack to show").arg(hex64(address)));
        return;
    }
    last_read_status_ = status;
    emit log_message(tr("!! debugger: cannot read %1 (status 0x%2) - is that address mapped in this process?").arg(hex64(address)).arg(status, 8, 16, QChar('0')));
}

void debugger_panel::redraw_disassembly() {
    for (const auto& insn : insns_) {
        if (insn.address == pc_) { pc_insn_ = insn; break; }
    }
    code_view_->clear();
    QTreeWidgetItem* pc_item    = nullptr;
    QTreeWidgetItem* focus_item = nullptr;
    const auto tint    = palette().color(QPalette::Highlight);
    const auto on_tint = palette().color(QPalette::HighlightedText);

    for (const auto& insn : insns_) {
        auto* item = new QTreeWidgetItem(code_view_);
        item->setData(col_address, Qt::UserRole, QVariant::fromValue<qulonglong>(insn.address));
        item->setText(col_address, hex64(insn.address));
        item->setText(col_opcode, QStringLiteral("%1").arg(insn.opcode, 8, 16, QChar('0')));
        item->setText(col_text, QString::fromStdString(insn.text()));
        if (const auto* sym = symbols_.find(insn.address);
            sym != nullptr && sym->address == insn.address) {
            item->setText(col_symbol, QString::fromStdString(sym->name));
        }

        const bool at_pc = have_regs_ && insn.address == pc_;
        QString marker;
        if (breakpoints_.contains(insn.address)) marker += QStringLiteral("*");
        if (at_pc)                               marker += QStringLiteral(">");
        item->setText(col_marker, marker);

        if (at_pc) {
            for (int c = 0; c < code_view_->columnCount(); ++c) {
                item->setBackground(c, tint);
                item->setForeground(c, on_tint);
            }
            pc_item = item;
        }
        if (focus_address_ != 0 && insn.address == focus_address_) focus_item = item;
    }

    if (auto* land = focus_item ? focus_item : pc_item) {
        code_view_->setCurrentItem(land);
        code_view_->scrollToItem(land, QAbstractItemView::PositionAtCenter);
    }
    focus_address_ = 0;
    refresh_actions();
}

void debugger_panel::toggle_breakpoint_here() {
    const auto address = selected_address();
    if (address == 0) return;
    if (breakpoints_.contains(address)) {
        emit breakpoint_clear_requested(address);
    } else {
        emit breakpoint_add_requested(address);
    }
}

void debugger_panel::on_breakpoint_added(quint64 address, quint32 status) {
    if (status == 0xffffffffu) {
        breakpoints_.insert(address);
        emit log_message(tr("-- a breakpoint was already set at %1 (left from an earlier session); it is listed now, press again to remove it").arg(hex64(address)));
        redraw_disassembly();
        return;
    }
    if (status != 0) {
        emit log_message(tr("    !! debugger: breakpoint at %1 refused (0x%2)").arg(hex64(address)).arg(status, 8, 16, QChar('0')));
        return;
    }
    breakpoints_.insert(address);
    refresh_breakpoints();
    redraw_disassembly();
}

void debugger_panel::on_breakpoint_removed(quint64 address, quint32 status) {
    if (status != 0) return;
    breakpoints_.remove(address);
    refresh_breakpoints();
    redraw_disassembly();
}

std::optional<quint64> debugger_panel::step_destination(quint64 pc, bool over) const {
    // find the instruction we are sitting on
    const opentm::tm_core::ppc_insn* here = nullptr;
    for (const auto& i : insns_) {
        if (i.address == pc) { here = &i; break; }
    }
    if (here == nullptr && pc_insn_ && pc_insn_->address == pc) here = &*pc_insn_;
    if (here == nullptr || !here->valid) return std::nullopt;

    if (!here->is_branch) return pc + 4;
    // stepping over a call means landing where it returns to
    if (over && here->is_call) return pc + 4;

    if (here->branch_target) return *here->branch_target;

    using indirect = opentm::tm_core::ppc_insn::indirect;
    if (!have_regs_) return std::nullopt;
    if (here->through == indirect::via_lr)  return regs_.lr;
    if (here->through == indirect::via_ctr) return regs_.ctr;
    return std::nullopt;
}

void debugger_panel::step_into() {
    if (!have_regs_ || pc_ == 0) {
        emit log_message(tr("-- nothing is stopped, so there is nowhere to step from"));
        return;
    }
    const auto dest = step_destination(pc_, false);
    if (!dest) {
        emit log_message(tr("    !! debugger: cannot work out where %1 goes").arg(hex64(pc_)));
        return;
    }

    QList<quint64> targets{*dest};
    // a conditional branch may fall through instead, so cover both...
    for (const auto& i : insns_) {
        if (i.address == pc_ && i.is_conditional && *dest != pc_ + 4) {
            targets.append(pc_ + 4);
            break;
        }
    }
    emit log_message(tr("-- step into from %1").arg(hex64(pc_)));
    step_from_ = pc_;
    emit step_requested(thread_id_, targets);
    status_->setText(tr("stepping from %1...").arg(hex64(pc_)));
}

void debugger_panel::step_over() {
    if (!have_regs_ || pc_ == 0) {
        emit log_message(tr("-- nothing is stopped, so there is nowhere to step from"));
        return;
    }
    const auto dest = step_destination(pc_, true);
    if (!dest) {
        emit log_message(tr("    !! debugger: cannot work out where %1 goes").arg(hex64(pc_)));
        return;
    }
    emit log_message(tr("-- step over from %1 to %2").arg(hex64(pc_), hex64(*dest)));
    step_from_ = pc_;
    emit step_requested(thread_id_, QList<quint64>{*dest});
    status_->setText(tr("stepping from %1...").arg(hex64(pc_)));
}

QList<quint64> debugger_panel::process_threads() const {
    if (!all_threads_.isEmpty()) return all_threads_;
    if (thread_id_ != 0) return {thread_id_};
    return {};
}

void debugger_panel::do_resume() {
    const auto ids = process_threads();
    if (ids.isEmpty()) return;
    emit resume_requested(ids);
}

void debugger_panel::do_halt() {
    const auto ids = process_threads();
    if (ids.isEmpty()) return;
    emit halt_requested(ids);
}

void debugger_panel::on_thread_stopped(quint64 thread_id, quint64 address, quint32 reason) {
    if (step_from_ != 0 && address == step_from_) {
        step_from_ = 0;
        emit log_message(tr("!! a breakpoint at %1 stops the thread as soon as it starts, so the step cannot move. Removing it - set it again if you want it.").arg(hex64(address)));
        breakpoints_.remove(address);
        emit breakpoint_clear_requested(address);
    } else {
        step_from_ = 0;
    }

    thread_id_ = thread_id;
    pc_        = address;
    stopped_   = true;
    running_   = false;

    emit log_message(tr("    ** debugger: thread %1 stopped at %2 (reason 0x%3)").arg(hex_short(thread_id), hex64(address)).arg(reason, 0, 16));

    emit registers_requested(thread_id);
    show_code_at(address);
    refresh_actions();
}

void debugger_panel::on_halt_finished(quint32 status) {
    if (status != 0) {
        emit log_message(tr("!! debugger: halt refused (status 0x%1)").arg(status, 8, 16, QChar('0')));
        return;
    }
    emit log_message(tr("-- halted; re-reading thread states"));
    halt_settle_retries_ = 5;
    emit thread_list_refresh_requested();
}

void debugger_panel::on_running_changed(bool running) {
    running_ = running;
    if (running) stopped_ = false;
    refresh_actions();
}

void debugger_panel::on_registers_ready(quint64 thread_id, opentm::tm_core::dbgp::ppu_registers regs) {
    thread_id_ = thread_id;
    regs_      = regs;
    have_regs_ = true;
    show_registers(regs);

    if (regs.pc > 0xffffffffull) {
        emit log_message(tr("!! debugger: thread %1 returned pc=%2, which is not an address. It is probably not halted - Resume still works.").arg(hex_short(thread_id), hex64(regs.pc)));
        have_regs_ = false;
        refresh_actions();
        return;
    }

    request_callstack();

    if (regs.pc != 0) {
        const bool moved = (regs.pc != pc_);
        pc_ = regs.pc;
        address_edit_->setText(hex64(pc_));
        // fetch the code around the PC unless we are already looking at it
        const bool have_it = !insns_.empty() && pc_ >= insns_.front().address && pc_ <= insns_.back().address;
        if (moved || !have_it) {
            show_code_at(pc_);
            return;
        }
    }
    redraw_disassembly();
}

void debugger_panel::show_registers(const opentm::tm_core::dbgp::ppu_registers& regs) {
    const int rows = static_cast<int>(opentm::tm_core::dbgp::ppu_gpr_count) + 7;
    regs_view_->setRowCount(rows);

    auto put = [this](int row, const QString& name, quint64 value, int width) {
        auto* n = new QTableWidgetItem(name);
        auto* v = new QTableWidgetItem(QStringLiteral("%1").arg(value, width, 16, QChar('0')));
        regs_view_->setItem(row, 0, n);
        regs_view_->setItem(row, 1, v);
    };

    int row = 0;
    put(row, QStringLiteral("pc"),  regs.pc,  16); ++row;
    put(row, QStringLiteral("lr"),  regs.lr,  16); ++row;
    put(row, QStringLiteral("ctr"), regs.ctr, 16); ++row;
    put(row, QStringLiteral("cr"),  regs.cr,   8);
    if (auto* item = regs_view_->item(row, 1)) item->setToolTip(decode_cr(regs.cr));
    ++row;
    {
        auto* n = new QTableWidgetItem(QStringLiteral("cr0-7"));
        auto* v = new QTableWidgetItem(decode_cr(regs.cr));
        regs_view_->setItem(row, 0, n);
        regs_view_->setItem(row, 1, v);
        ++row;
    }
    put(row, QStringLiteral("xer"),   regs.xer,  16); ++row;
    put(row, QStringLiteral("fpscr"), regs.fpscr, 8); ++row;
    for (std::size_t i = 0; i < opentm::tm_core::dbgp::ppu_gpr_count; ++i, ++row) {
        put(row, QStringLiteral("r%1").arg(i), regs.gpr[i], 16);
    }
    regs_view_->resizeColumnToContents(0);
}

void debugger_panel::edit_register(int row, int) {
    const int first_gpr = 7;
    if (row < first_gpr || !have_regs_) return;
    const auto index = static_cast<unsigned>(row - first_gpr);
    if (index >= opentm::tm_core::dbgp::ppu_gpr_count) return;

    bool ok = false;
    const auto current = QStringLiteral("%1").arg(regs_.gpr[index], 16, 16, QChar('0'));
    const auto text = QInputDialog::getText(this, tr("Set r%1").arg(index), tr("New value (hex):"), QLineEdit::Normal, current, &ok);
    if (!ok) return;
    auto trimmed = text.trimmed();
    if (trimmed.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive)) trimmed = trimmed.mid(2);
    bool parsed = false;
    const auto value = trimmed.toULongLong(&parsed, 16);
    if (!parsed) {
        emit log_message(tr("    !! debugger: '%1' is not a hex value").arg(text));
        return;
    }
    emit gpr_write_requested(thread_id_, index, value);
    emit registers_requested(thread_id_);
}

void debugger_panel::on_threads_ready(std::uint32_t, QList<opentm::tm_core::dbgp::ppu_thread_info> threads) {
    threads_view_->clear();
    all_threads_.clear();
    QTreeWidgetItem* halted = nullptr;
    for (const auto& t : threads) {
        all_threads_.append(t.thread_id);
        auto* item = new QTreeWidgetItem(threads_view_);
        item->setData(0, Qt::UserRole, QVariant::fromValue<qulonglong>(t.thread_id));
        item->setData(1, Qt::UserRole, t.state);
        item->setText(0, hex_short(t.thread_id));
        item->setText(1, QString::fromLatin1(opentm::tm_core::dbgp::ppu_thread_state_name(t.state)));
        item->setText(2, QString::fromStdString(t.name));
        item->setText(3, QString::number(t.priority));
        item->setText(4, tr("%1  (%2 KB)").arg(hex64(t.stack_addr)).arg(t.stack_size / 1024));
        if (!halted && t.state == thread_state_stop) halted = item;
    }
    if (!halted && !auto_halted_ && !all_threads_.isEmpty()) {
        auto_halted_ = true;
        emit log_message(tr("-- stopping the process to attach"));
        emit halt_requested(all_threads_);
        refresh_actions();
        return;
    }

    if (!halted && halt_settle_retries_ > 0) {
        --halt_settle_retries_;
        QTimer::singleShot(200, this, [this] { emit thread_list_refresh_requested(); });
        refresh_actions();
        return;
    }
    if (halted) halt_settle_retries_ = 0;

    if (halted && thread_id_ == 0) {
        threads_view_->setCurrentItem(halted);
    } else if (thread_id_ != 0) {
        for (int i = 0; i < threads_view_->topLevelItemCount(); ++i) {
            auto* item = threads_view_->topLevelItem(i);
            if (item->data(0, Qt::UserRole).toULongLong() != thread_id_) continue;
            threads_view_->setCurrentItem(item);
            stack_top_ = item->data(4, Qt::UserRole).toULongLong();
            if (item->data(1, Qt::UserRole).toUInt() == thread_state_stop && !have_regs_) {
                emit registers_requested(thread_id_);
            }
            break;
        }
    }
    refresh_actions();
}

void debugger_panel::on_modules_ready(std::uint32_t, QList<opentm::tm_core::dbgp::prx_info> modules) {
    modules_view_->clear();
    for (const auto& m : modules) {
        auto* item = new QTreeWidgetItem(modules_view_);
        item->setText(0, QString::fromStdString(m.name));
        item->setText(1, hex_short(m.attribute));
        item->setText(2, hex_short(m.start_entry));
        item->setText(3, hex_short(m.stop_entry));
        item->setText(4, QString::number(m.mem_size));
        item->setToolTip(0, QString::fromStdString(m.path));
    }
    for (int c = 0; c < modules_view_->columnCount(); ++c) modules_view_->resizeColumnToContents(c);
}

void debugger_panel::on_process_ready(quint32 pid, opentm::tm_core::dbgp::process_info info) {
    process_view_->clear();
    auto row = [this](const QString& name, const QString& value) {
        auto* item = new QTreeWidgetItem(process_view_);
        item->setText(0, name);
        item->setText(1, value);
    };
    row(tr("Process"),      hex_short(pid));
    row(tr("Path"),         QString::fromStdString(info.self_path));
    row(tr("Parent"),       info.parent_pid == 0 ? tr("no parent") : hex_short(info.parent_pid));
    row(tr("PPU threads"),  QString::number(info.ppu_thread_count));
    row(tr("SPU threads"),  QString::number(info.spu_thread_count));
    row(tr("Raw SPU"),      QString::number(info.raw_spu_count));
    row(tr("Max phys mem"), hex64(info.max_physical_mem_size));
    row(tr("Debug flags"),  hex_short(info.debug_flags));
    process_view_->resizeColumnToContents(0);

    emit modules_requested(pid);
}

void debugger_panel::thread_selected() {
    const auto* item = threads_view_->currentItem();
    if (!item) return;
    bool ok = false;
    const auto tid = item->data(0, Qt::UserRole).toULongLong(&ok);
    if (!ok || tid == 0) return;
    thread_id_ = tid;
    stack_top_ = item->data(4, Qt::UserRole).toULongLong();

    const auto state = item->data(1, Qt::UserRole).toUInt();
    if (state != thread_state_stop) {
        have_regs_ = false;
        regs_view_->setRowCount(0);
        if (tid != reported_thread_ || state != reported_state_) {
            reported_thread_ = tid;
            reported_state_  = state;
            emit log_message(tr("-- thread %1 is %2, not halted. Press Halt to read its registers.").arg(hex_short(tid), QString::fromLatin1(opentm::tm_core::dbgp::ppu_thread_state_name(state))));
        }
        refresh_actions();
        return;
    }
    reported_thread_ = tid;
    reported_state_  = state;
    emit registers_requested(tid);
}

void debugger_panel::release_process() {
    if (!supported_) return;

    for (auto address : breakpoints_) emit breakpoint_clear_requested(address);
    breakpoints_.clear();

    const auto ids = process_threads();
    if (!ids.isEmpty()) {
        emit log_message(tr("-- letting the process run again before detaching"));
        emit resume_requested(ids);
    }
}

void debugger_panel::on_process_changed(quint32 pid) {
    // a reload replaces every thread id and invalidates the code we cached
    emit log_message(tr("-- now looking at process 0x%1").arg(pid, 0, 16));
    on_session_invalidated();
}

void debugger_panel::on_session_invalidated() {
    auto_halted_ = false;
    halt_settle_retries_ = 0;
    pc_insn_.reset();
    stack_requests_.clear();
    memory_requests_.clear();
    if (stack_view_) stack_view_->clear();
    if (memory_view_) memory_view_->clear();
    all_threads_.clear();
    reported_thread_ = 0;
    reported_state_  = 0xffffffffu;
    breakpoints_.clear();
    refresh_breakpoints();
    insns_.clear();
    code_.clear();
    have_regs_ = false;
    stopped_   = false;
    running_   = false;
    pc_        = 0;
    thread_id_ = 0;
    code_view_->clear();
    regs_view_->setRowCount(0);
    threads_view_->clear();
    refresh_actions();
}

} // namespace opentm::tm_ui
