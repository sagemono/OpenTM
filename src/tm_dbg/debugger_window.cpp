#include "debugger_window.h"

#include "debugger_panel.h"

#include <tm_core/target_type.h>

#include <QApplication>
#include <QCloseEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QDir>
#include <QDockWidget>
#include <QHBoxLayout>
#include <QLabel>
#include <QComboBox>
#include <QVariantMap>
#include <QJsonArray>
#include <QLineEdit>
#include <tm_session/rpc_client.h>
#include <QMenu>
#include <QMenuBar>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QStatusBar>
#include <QToolBar>
#include <QVBoxLayout>
#include <QWidget>

namespace opentm::tm_ui {

debugger_window::debugger_window(const session_backend& backend, const target_record& target, QWidget* parent) : QMainWindow(parent), backend_(backend), target_(target)
{
    build_ui();
    if (backend_.k == session_backend::kind::in_process) {
        if (!target_.host.isEmpty()) attach();
    } else {
        discover_targets();
    }
}

debugger_window::~debugger_window() = default;

void debugger_window::build_ui() {
    setWindowTitle(tr("OpenTM Debugger"));
    resize(1100, 720);

    auto* bar = addToolBar(tr("Target"));
    bar->setMovable(false);
    bar->addWidget(new QLabel(tr("Target  "), this));

    target_box_ = new QComboBox(this);
    target_box_->setEditable(true);
    target_box_->setMinimumWidth(280);
    target_box_->lineEdit()->setPlaceholderText(QStringLiteral("10.0.0.231"));
    if (!target_.host.isEmpty()) target_box_->setEditText(target_.host);
    bar->addWidget(target_box_);

    attach_btn_ = new QPushButton(tr("Attach"), this);
    bar->addWidget(attach_btn_);

    auto* symbols_btn = new QPushButton(tr("Symbols..."), this);
    symbols_btn->setToolTip(tr("Load function names from the .elf or the .self it was built into"));
    bar->addWidget(symbols_btn);
    connect(symbols_btn, &QPushButton::clicked, this, [this] {
        const auto path = QFileDialog::getOpenFileName(
            this, tr("Load symbols"), target_.file_server_dir,
            tr("Executables (*.elf *.self *.prx *.sprx);;All files (*)"));
        if (!path.isEmpty()) panel_->load_symbols(path);
    });
    connect(attach_btn_, &QPushButton::clicked, this, &debugger_window::attach);
    connect(target_box_->lineEdit(), &QLineEdit::returnPressed, this, &debugger_window::attach);

    panel_ = new debugger_panel(this);
    build_panes();
    auto* view_menu = menuBar()->addMenu(tr("&View"));
    connect(view_menu, &QMenu::aboutToShow, this, [this, view_menu] {
        view_menu->clear();
        if (auto* panes = createPopupMenu()) {
            for (auto* action : panes->actions()) view_menu->addAction(action);
            panes->deleteLater();
        }
        view_menu->addSeparator();
        view_menu->addAction(tr("Reset Layout"), this, [this] { reset_layout(); });
    });

    state_ = new QLabel(tr("not attached"), this);
    statusBar()->addPermanentWidget(state_);

    connect(panel_, &debugger_panel::log_message, this, &debugger_window::append_log);
}

void debugger_window::closeEvent(QCloseEvent* e) {
    save_layout();
    if (session_ && panel_) {
        panel_->release_process();
        QCoreApplication::processEvents(QEventLoop::AllEvents, 250);
    }
    QMainWindow::closeEvent(e);
}

void debugger_window::try_symbols_for(const QString& kit_path) {
    if (!panel_ || panel_->has_symbols() || kit_path.isEmpty()) return;

    QStringList dirs;
    if (!target_.file_server_dir.isEmpty()) dirs << target_.file_server_dir;
    const auto local = qEnvironmentVariable("LOCALAPPDATA");
    if (!local.isEmpty() && !target_.name.isEmpty()) {
        dirs << QDir(local).absoluteFilePath(QStringLiteral("OpenTM/app_home/") + target_.name);
    }
    if (dirs.isEmpty()) {
        append_log(tr("-- nowhere to look for symbols; use Symbols... to pick the .elf or .self"));
        return;
    }

    const auto base = QFileInfo(kit_path).completeBaseName();
    for (const auto& dir : dirs) {
        for (const auto& suffix : {QStringLiteral(".elf"), QStringLiteral(".self")}) {
            const QFileInfo candidate(QDir(dir), base + suffix);
            if (!candidate.isFile()) continue;
            if (panel_->load_symbols(candidate.absoluteFilePath())) return;
        }
    }
    append_log(tr("-- no symbols for '%1' in %2. Use Symbols... to point at the .elf or the .self it was built into.").arg(base, dirs.join(QStringLiteral(", "))));
}

void debugger_window::build_panes() {
    setDockNestingEnabled(true);
    setDockOptions(QMainWindow::AnimatedDocks | QMainWindow::AllowNestedDocks | QMainWindow::AllowTabbedDocks | QMainWindow::GroupedDragging);

    addToolBarBreak();
    addToolBar(panel_->run_toolbar());
    auto* code = panel_->code_widget();
    code->setMinimumSize(420, 260);
    setCentralWidget(code);

    for (const auto& pane : panel_->panes()) {
        auto* dock = new QDockWidget(pane.title, this);
        dock->setObjectName(pane.id);
        dock->setWidget(pane.body);
        dock->setAllowedAreas(Qt::AllDockWidgetAreas);
        addDockWidget(pane.area, dock);
        docks_.insert(pane.id, dock);
    }
    reset_layout();
    restore_layout();
}

void debugger_window::reset_layout() {
    auto at = [this](const QString& id) { return docks_.value(id); };
    for (const auto& id : {QStringLiteral("pane_registers"), QStringLiteral("pane_memory")}) {
        if (auto* d = at(id)) { d->setFloating(false); d->show(); addDockWidget(Qt::RightDockWidgetArea, d); }
    }
    const QStringList bottom{QStringLiteral("pane_callstack"), QStringLiteral("pane_threads"), QStringLiteral("pane_modules"), QStringLiteral("pane_process"), QStringLiteral("pane_log")};
    QDockWidget* previous = nullptr;
    for (const auto& id : bottom) {
        auto* d = at(id);
        if (!d) continue;
        d->setFloating(false);
        d->show();
        addDockWidget(Qt::BottomDockWidgetArea, d);
        if (previous) tabifyDockWidget(previous, d);
        previous = d;
    }
    if (auto* first = at(QStringLiteral("pane_callstack"))) first->raise();
}

void debugger_window::save_layout() {
    QSettings s;
    s.setValue(QStringLiteral("debugger/panes"), saveState(3));
}

void debugger_window::restore_layout() {
    QSettings s;
    const auto state = s.value(QStringLiteral("debugger/panes")).toByteArray();
    if (!state.isEmpty()) restoreState(state, 3);
}

void debugger_window::append_log(const QString& line) {
    if (panel_) panel_->append_log_line(line);
}

void debugger_window::set_state(const QString& text) {
    if (state_) state_->setText(text);
}

target_record debugger_window::selected_target() const {
    const int row = target_box_->currentIndex();
    if (row >= 0) {
        const auto data = target_box_->itemData(row).toMap();
        if (!data.isEmpty() &&
            data.value(QStringLiteral("text")).toString() == target_box_->currentText()) {
            target_record r = target_;
            r.id   = data.value(QStringLiteral("handle")).toString();
            r.name = data.value(QStringLiteral("name")).toString();
            r.host = data.value(QStringLiteral("host")).toString();
            r.port = static_cast<quint16>(data.value(QStringLiteral("port")).toInt());
            if (const auto t = opentm::tm_core::target_type_from_string(data.value(QStringLiteral("type")).toString().toStdString())) {
                r.type = *t;
            }
            return r;
        }
    }

    target_record r = target_;
    r.host = target_box_->currentText().trimmed();
    r.name = r.host;
    r.id   = r.host;
    return r;
}

void debugger_window::discover_targets() {
    if (backend_.k == session_backend::kind::in_process) return;

    auto* probe = new rpc_client(this);
    const bool ok = (backend_.k == session_backend::kind::remote_local) ? probe->connect_local(backend_.local_name) : probe->connect_tcp(backend_.host, backend_.port);
    if (!ok) {
        append_log(tr("-- could not ask the session server what it has open"));
        probe->deleteLater();
        return;
    }

    probe->call(QStringLiteral("target.list"), {}, [this, probe](bool called, const QJsonObject& reply, const QString& err) {
        probe->deleteLater();
        if (!called) {
            append_log(tr("-- target.list failed: %1").arg(err));
            return;
        }
        const auto targets = reply.value(QStringLiteral("targets")).toArray();
        const auto wanted  = target_box_->currentText().trimmed();

        target_box_->clear();
        int preferred = -1;
        for (const auto& v : targets) {
            const auto o = v.toObject();
            const auto host = o.value(QStringLiteral("host")).toString();
            const auto name = o.value(QStringLiteral("name")).toString();
            const auto label = (name.isEmpty() || name == host) ? host : QStringLiteral("%1  (%2)").arg(name, host);
            QVariantMap data{
                {QStringLiteral("handle"), o.value(QStringLiteral("target")).toString()},
                {QStringLiteral("name"),   name},
                {QStringLiteral("host"),   host},
                {QStringLiteral("port"),   o.value(QStringLiteral("port")).toInt()},
                {QStringLiteral("type"),   o.value(QStringLiteral("type")).toString()},
                {QStringLiteral("text"),   label},
            };
            target_box_->addItem(label, data);
            if (!wanted.isEmpty() && host == wanted) preferred = target_box_->count() - 1;
        }

        if (target_box_->count() == 0) {
            append_log(tr("-- the session server has no console open. Connect one in the target manager, or type a host here."));
            if (!wanted.isEmpty()) target_box_->setEditText(wanted);
            return;
        }

        target_box_->setCurrentIndex(preferred >= 0 ? preferred : 0);
        append_log(tr("-- %1 console(s) open on the server").arg(target_box_->count()));
        if (target_box_->count() == 1 || preferred >= 0) attach();// one console and nothing to choose, just attach to it
    });
}

void debugger_window::attach() {
    const auto chosen = selected_target();
    if (chosen.host.isEmpty()) {
        append_log(tr("!! no target given"));
        return;
    }
    target_ = chosen;
    const auto host = target_.host;

    if (session_) {
        session_->disconnect(this);
        session_->disconnect(panel_);
        panel_->disconnect(session_);
        session_->deleteLater();
        session_ = nullptr;
    }
    ready_ = false;
    asked_for_processes_ = false;
    panel_->on_session_invalidated();

    QString err;
    session_ = make_session(backend_, this, &err);
    if (!session_) {
        append_log(tr("!! %1").arg(err));
        set_state(tr("no session"));
        return;
    }
    if (!backend_.fallback_reason.isEmpty()) {
        append_log(tr("** %1").arg(backend_.fallback_reason));
    }

    switch (backend_.k) {
    case session_backend::kind::in_process:
        append_log(tr("-- this process owns the console session. The target manager cannot hold it at the same time."));
        break;
    case session_backend::kind::remote_tcp:
        append_log(tr("-- attaching through the session server at %1:%2").arg(backend_.host).arg(backend_.port));
        break;
    case session_backend::kind::remote_local:
        append_log(tr("-- attaching through the session server '%1'").arg(backend_.local_name));
        break;
    }

    wire_session();
    session_->set_target(target_);
    session_->connect_to_target();
    set_state(tr("attaching to %1...").arg(host));

    if (session_->is_session_ready()) {
        ready_ = true;
        ask_for_process_list();
    }
}

void debugger_window::ask_for_process_list() {
    if (!session_ || !ready_ || asked_for_processes_) return;
    asked_for_processes_ = true;
    session_->refresh_process_list();
}

void debugger_window::wire_session() {
    auto* s = session_;
    auto* d = panel_;

    d->set_supported(s->supports_debugger(), tr("this session cannot reach a debug agent"));

    connect(s, &session_api::log_message, this, &debugger_window::append_log);
    connect(s, &session_api::error, this, [this](const QString& m) {
        if (m.contains(QLatin1String("unknown method 'debug."))) {
            if (!warned_stale_) {
                warned_stale_ = true;
                append_log(tr("!! the session server does not know the debugger verbs, so it is older than this build. Restart it with 'opentm_dbg --restart-server' (this drops the sessions it is holding), or quit the OpenTM tray and start again."));
                set_state(tr("session server is out of date"));
            }
            return;
        }
        append_log(QStringLiteral("!! ") + m);
    });

    connect(d, &debugger_panel::memory_requested,           s, &session_api::debug_read_memory);
    connect(d, &debugger_panel::memory_write_requested,     s, &session_api::debug_write_memory);
    connect(d, &debugger_panel::registers_requested,        s, &session_api::debug_read_registers);
    connect(d, &debugger_panel::gpr_write_requested,        s, &session_api::debug_write_gpr);
    connect(d, &debugger_panel::breakpoint_add_requested,   s, &session_api::debug_set_breakpoint);
    connect(d, &debugger_panel::breakpoint_clear_requested, s, &session_api::debug_clear_breakpoint);
    connect(d, &debugger_panel::resume_requested,           s, &session_api::debug_resume);
    connect(d, &debugger_panel::halt_requested,             s, &session_api::debug_halt);
    connect(d, &debugger_panel::step_requested,             s, &session_api::debug_step_to);
    connect(d, &debugger_panel::threads_requested,          s, &session_api::refresh_threads);
    connect(d, &debugger_panel::modules_requested,          s, &session_api::refresh_modules);
    connect(s, &session_api::modules_ready,                 d, &debugger_panel::on_modules_ready);

    connect(s, &session_api::debug_memory_ready,       d, &debugger_panel::on_memory_ready);
    connect(s, &session_api::debug_memory_read_failed, d, &debugger_panel::on_memory_read_failed);
    connect(s, &session_api::debug_registers_ready,    d, &debugger_panel::on_registers_ready);
    connect(s, &session_api::debug_breakpoint_added,   d, &debugger_panel::on_breakpoint_added);
    connect(s, &session_api::debug_breakpoint_removed, d, &debugger_panel::on_breakpoint_removed);
    connect(s, &session_api::debug_thread_stopped,     d, &debugger_panel::on_thread_stopped);
    connect(s, &session_api::debug_running_changed,    d, &debugger_panel::on_running_changed);
    connect(s, &session_api::debug_halt_finished,      d, &debugger_panel::on_halt_finished);
    connect(s, &session_api::debug_process_changed,    d, &debugger_panel::on_process_changed);
    connect(d, &debugger_panel::thread_list_refresh_requested, this, [this] {
        if (session_ && pid_ != 0) session_->refresh_threads(pid_);
    });
    connect(s, &session_api::session_invalidated,      d, &debugger_panel::on_session_invalidated);
    connect(s, &session_api::threads_ready,            d, &debugger_panel::on_threads_ready);
    connect(s, &session_api::process_list_ready, d,
            [this, s, d](QList<session_api::process_summary> processes) {
                if (processes.isEmpty()) {
                    set_state(tr("attached, no process running"));
                    return;
                }
                const auto pid = processes.front().pid;
                pid_ = pid;
                s->debug_set_process(pid);
                s->refresh_threads(pid);
                d->on_process_ready(pid, processes.front().info);
                self_path_ = QString::fromStdString(processes.front().info.self_path);
                try_symbols_for(self_path_);
                d->setEnabled(true);
                set_state(tr("attached to pid 0x%1").arg(pid, 0, 16));
            });

    connect(s, &session_api::session_ready, this, [this](std::uint16_t, std::uint16_t) {
        ready_ = true;
        set_state(tr("session ready, looking for a process"));
        ask_for_process_list();
    });
    connect(s, &session_api::debug_agent_ready, this, [this] { ask_for_process_list(); });
    connect(s, &session_api::session_invalidated, this, [this] { ready_ = false; });
    connect(s, &session_api::protocol_rejected, this,
            [this](quint32, const QString& name, quint8 status) {
        if (status != 0x02) return;
        append_log(tr("!! %1 is owned by another connection. Both apps have to go through the same session server: start the target manager without --standalone, and leave background sessions on in its Preferences.").arg(name));
        set_state(tr("%1 owned by another connection").arg(name));
    });
    connect(s, &session_api::target_went_down, this, [this](const QString& why) {
        set_state(tr("target down: %1").arg(why));
    });
}

} // namespace opentm::tm_ui
