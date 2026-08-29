#include "main_window.h"
#include <tm_session/frame_dispatcher.h>
#include <tm_session/file_explorer_controller.h>
#include <tm_session/host_file_server.h>
#include <tm_session/kernel_explorer_controller.h>
#include "controllers/load_controller.h"
#include <tm_session/session_controller.h>
#include <tm_session/target_actions.h>
#include "about_dialog.h"
#include "file_properties_dialog.h"
#include "app_prefs.h"
#include "app_style.h"
#include "preferences_dialog.h"
#include "panels/cp_panel.h"
#include "discovery_dialog.h"
#include "shortcut_settings_dialog.h"
#include "panels/console_panel.h"
#include "panels/file_explorer_panel.h"
#include "panels/kernel_explorer_panel.h"
#include "panels/target_panel.h"
#include "panels/wire_log_panel.h"

#include <tm_launch/launch.h>
#include <tm_launch/single_instance.h>

#include <tm_core/target_type.h>
#include <tm_core/tcp_connection.h>

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QCoreApplication>
#include <QThread>
#include <QStyle>
#include <QStyleFactory>
#include <QDockWidget>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHostAddress>
#include <QIcon>
#include <QProgressDialog>
#include <QPushButton>
#include <QHostInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QRegularExpression>
#include <QDateTime>
#include <QFile>
#include <QSettings>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTabWidget>
#include <QTimer>
#include <QToolBar>

namespace opentm::tm_ui {

namespace {

QIcon ico(const char* name) {
    const QString path = QStringLiteral(":/icons/%1.bmp").arg(QLatin1String(name));
    QIcon icon(path);
    if (icon.isNull()) { // TODO: remove
        qWarning("OpenTM: icon resource %s failed to load. is icons.qrc " "compiled in? (check qrc_icons.cpp.obj is tens of KB, not " "a few hundred bytes)", qUtf8Printable(path));
    }
    return icon;
}

} // namespace

main_window::main_window(const session_backend& backend, QWidget* parent) : QMainWindow(parent), backend_(backend)
{
    switch (backend_.k) {
    case session_backend::kind::in_process:
        setWindowTitle(backend_.fallback_reason.isEmpty() ? tr("OpenTM - standalone") : tr("OpenTM - standalone (no session server)"));
        break;
    case session_backend::kind::remote_tcp:
        setWindowTitle(tr("OpenTM - via opentm_server %1:%2").arg(backend_.host).arg(backend_.port));
        break;
    case session_backend::kind::remote_local:
        setWindowTitle(tr("OpenTM - via opentm_server '%1'").arg(backend_.local_name));
        break;
    }
    resize(1024, 640);

    load_controller_ = new load_controller(nullptr, this, this);
    connect(load_controller_, &load_controller::status_message, this, &main_window::set_status_for_current);

    build_target_dock();
    build_menus();
    register_shortcuts();
    build_toolbar();
    build_central();
    build_wire_log();
    build_console();
    build_status_indicator();
    update_action_states();
    statusBar()->showMessage(tr("Ready"));
    update_state_indicator(opentm::tm_core::tcp_connection::state::disconnected);

    QSettings s;
    if (s.contains("mw_geometry")) restoreGeometry(s.value("mw_geometry").toByteArray());
    if (s.contains("mw_state"))    restoreState(s.value("mw_state").toByteArray());

    if (!backend_.fallback_reason.isEmpty()) {
        log_wire(tr("!! could not start the session server: %1").arg(backend_.fallback_reason));
        log_wire(tr("   running standalone - console sessions will end when ""this window closes"));
        statusBar()->showMessage(tr("No session server: running standalone (see Wire Log)"), 15000);
    }
}

main_window::~main_window() {
    qDeleteAll(slots_);
}

void main_window::closeEvent(QCloseEvent* event) {
    QSettings s;
    s.setValue("mw_geometry", saveGeometry());
    s.setValue("mw_state", saveState());
    if (backend_.k == session_backend::kind::in_process) {
        for (auto* slot : slots_) slot->session->disconnect_from_target();
    }
    QMainWindow::closeEvent(event);
}

main_window::target_slot* main_window::slot_for(const target_record& r, const QPersistentModelIndex& idx)
{
    if (r.id.isEmpty()) return nullptr;
    if (const auto it = slots_.constFind(r.id); it != slots_.constEnd()) {
        (*it)->row = idx;
        return *it;
    }

    QString err;
    auto* s = make_session(backend_, this, &err);
    if (!s) {
        log_wire(tr("    !! %1").arg(err));
        stub_status(err);
        return nullptr;
    }

    s->set_auto_reconnect(auto_reconnect_enabled());

    auto* slot    = new target_slot;
    slot->session = s;
    slot->row     = idx;
    slot->files   = new file_explorer_panel(files_stack_);
    slot->kernel  = new kernel_explorer_panel(kernel_stack_);
    slot->console = new console_panel(console_stack_);
    slot->wire    = new wire_log_panel(wire_stack_);
    slot->cp      = new cp_panel(cp_stack_);
    files_stack_->addWidget(slot->files);
    kernel_stack_->addWidget(slot->kernel);
    console_stack_->addWidget(slot->console);
    wire_stack_->addWidget(slot->wire);
    cp_stack_->addWidget(slot->cp);
    slots_.insert(r.id, slot);

    connect(slot->cp, &cp_panel::log_message, this, &main_window::log_wire);
    wire_slot_panels(slot);
    return slot;
}

void main_window::wire_slot_panels(target_slot* slot) {
    auto* s = slot->session;

    connect(s, &session_api::debug_agent_ready, this, [this, slot] { on_debug_agent_ready(slot); });

    connect(s, &session_api::status_message, this, [this, slot](const QString& t) {
        if (target_panel_ && slot->row.isValid()) {
            target_panel_->set_status_for(slot->row, t);
        }
        if (slot == active_) statusBar()->showMessage(t, 4000);
    });
    connect(s, &session_api::sdk_version_received, this, [this, slot](QString sdk) {
        if (target_panel_ && slot->row.isValid()) {
            target_panel_->set_property_for(slot->row, QStringLiteral("sdk"), sdk);
        }
    });
    connect(s, &session_api::boot_mode_changed, this, [this, slot](QString mode) {
        if (target_panel_ && slot->row.isValid()) {
            target_panel_->set_property_for(slot->row, QStringLiteral("boot mode"), mode);
        }
        if (slot == active_) statusBar()->showMessage(tr("Target is in %1").arg(mode), 6000);
    });
    connect(s, &session_api::cp_version_received, this, [this, slot](QString cp) {
        if (target_panel_ && slot->row.isValid()) {
            target_panel_->set_property_for(slot->row, QStringLiteral("cp"), cp);
        }
    });

    connect(s, &session_api::wire_line,   slot->wire, &wire_log_panel::append);
    connect(s, &session_api::log_message, slot->wire, &wire_log_panel::append);

    // a shutdown or reset drops the link; the trees below are then stale and
    // reading them as live state is how you end up chasing ghosts
    connect(s, &session_api::connection_state_changed, this,
            [this, slot](opentm::tm_core::tcp_connection::state st) {
        using state = opentm::tm_core::tcp_connection::state;
        if (target_panel_ && slot->row.isValid()) {
            target_panel_->set_state_icon_for(slot->row, st);
        }
        if (st == state::disconnected || st == state::error_state) {
            slot->kernel->clear();
            slot->files->clear();
        }
    });

    connect(s, &session_api::tty_stream_text, slot->console, &console_panel::append_stream);
    connect(s, &session_api::clear_console, slot->console, &console_panel::clear);

    connect(s, &session_api::target_went_down, this, [this, slot](const QString& reason) {
        slot->kernel->clear();
        slot->files->clear();
        if (target_panel_ && slot->row.isValid()) {
            target_panel_->set_status_for(slot->row, reason);
        }
    });

    connect(s, &session_api::transfer_finished, this, [this, slot] {
        if (!slot->pending_transfer.isEmpty()) {
            if (slot == active_) statusBar()->showMessage(slot->pending_transfer, 5000);
            slot->pending_transfer.clear();
        }
        if (!slot->refresh_after_transfer.isEmpty()) {
            slot->session->list_directory(slot->refresh_after_transfer);
            slot->refresh_after_transfer.clear();
        }
    });
    connect(s, &session_api::transfer_failed, this, [this, slot](std::uint32_t result) {
        slot->pending_transfer.clear();
        slot->refresh_after_transfer.clear();
        const auto msg = tr("Transfer failed (0x%1)").arg(result, 0, 16);
        if (target_panel_ && slot->row.isValid()) target_panel_->set_status_for(slot->row, msg);
        if (slot == active_) statusBar()->showMessage(msg, 8000);
    });

    connect(slot->console, &console_panel::settings_requested, this, &main_window::open_preferences);
    connect(slot->console, &console_panel::save_requested, this, [this, slot] {
        save_text_to_file(slot->console->current_text(), slot->console->current_channel_name());
    });
    connect(slot->wire, &wire_log_panel::save_requested, this, [this, slot] {
        save_text_to_file(slot->wire->text(), tr("wire-log"));
    });

    connect(slot->files, &file_explorer_panel::list_directory_requested, s, &session_api::list_directory);
    connect(slot->files, &file_explorer_panel::log_message, slot->wire, &wire_log_panel::append);
    connect(slot->files, &file_explorer_panel::run_requested, this, [this, slot](const QString& kit_path) {
        run_from_explorer(slot, kit_path);
    });

    connect(slot->files, &file_explorer_panel::download_requested, this, [this, slot](const QString& kit_path, quint32 size) {
        const QString suggested = QDir(last_download_dir()).filePath(QFileInfo(kit_path).fileName());
        const auto to = QFileDialog::getSaveFileName(this, tr("Download %1").arg(kit_path), suggested);
        if (to.isEmpty()) return;
        set_last_download_dir(QFileInfo(to).absolutePath());
        slot->pending_transfer = tr("Downloaded %1").arg(QFileInfo(to).fileName());
        slot->session->download_file(kit_path, to, size);
    });

    connect(slot->files, &file_explorer_panel::upload_requested, this, [this, slot](const QString& kit_dir) {
        const auto from = QFileDialog::getOpenFileName(this, tr("Upload to %1").arg(kit_dir), last_upload_dir());
        if (from.isEmpty()) return;
        set_last_upload_dir(QFileInfo(from).absolutePath());
        QString dir = kit_dir;
        if (!dir.endsWith(QLatin1Char('/'))) dir += QLatin1Char('/');
        slot->pending_transfer = tr("Uploaded %1").arg(QFileInfo(from).fileName());
        slot->refresh_after_transfer = dir;
        slot->session->upload_file(from, dir + QFileInfo(from).fileName());
    });

    connect(slot->files, &file_explorer_panel::delete_requested, this, [this, slot](const QString& kit_path, bool is_dir) {
        // real TM deletes folders with the same op it uses for files, so this
        // takes the same path; only the wording differs
        const auto reply = QMessageBox::warning(
            this, is_dir ? tr("Delete Folder") : tr("Delete File"),
            is_dir ? tr("Permanently delete this folder from the target?\n\n%1").arg(kit_path)
                   : tr("Permanently delete this file from the target?\n\n%1").arg(kit_path),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
        if (reply != QMessageBox::Yes) return;
        slot->session->delete_file(kit_path);
    });
    connect(slot->files, &file_explorer_panel::rename_requested, this,
            [this, slot](const QString& kit_path, const QString& new_name) {
        if (!slot->session->is_session_ready()) {
            stub_status(tr("Connect to the target before renaming %1.").arg(kit_path));
            return;
        }
        QString dir = kit_path.left(kit_path.lastIndexOf(QLatin1Char('/')) + 1);
        slot->session->rename_file(kit_path, dir + new_name);
        slot->refresh_after_transfer = slot->files->current_path();
        if (!slot->refresh_after_transfer.isEmpty()) {
            slot->session->list_directory(slot->refresh_after_transfer);
        }
    });
    connect(slot->files, &file_explorer_panel::make_dir_requested, this,
            [this, slot](const QString& kit_dir, const QString& name) {
        if (!slot->session->is_session_ready()) {
            stub_status(tr("Connect to the target before creating folders."));
            return;
        }
        QString dir = kit_dir;
        if (!dir.endsWith(QLatin1Char('/'))) dir += QLatin1Char('/');
        slot->session->make_directory(dir + name, 0777u);
        slot->refresh_after_transfer = slot->files->current_path();
        if (!slot->refresh_after_transfer.isEmpty()) {
            slot->session->list_directory(slot->refresh_after_transfer);
        }
    });
    connect(slot->files, &file_explorer_panel::properties_requested, this,
            [this, slot](const QString& kit_path, quint32 mode, quint64 atime, quint64 mtime, quint64 ctime) {
        if (!slot->session->is_session_ready()) {
            stub_status(tr("Connect to the target before editing %1.").arg(kit_path));
            return;
        }
        file_properties_dialog::values before{mode, atime, mtime, ctime};
        file_properties_dialog dlg(kit_path, before, this);
        if (dlg.exec() != QDialog::Accepted) return;

        const auto after = dlg.result();
        slot->session->set_permissions(kit_path, after.mode);
        slot->session->set_times(kit_path, after.atime, after.mtime);
        slot->refresh_after_transfer = slot->files->current_path();
        if (!slot->refresh_after_transfer.isEmpty()) {
            slot->session->list_directory(slot->refresh_after_transfer);
        }
    });
    connect(s, &session_api::directory_listed, slot->files, &file_explorer_panel::show_directory);

    auto* k = slot->kernel;
    connect(k, &kernel_explorer_panel::refresh_requested,      s, &session_api::refresh_process_list);
    connect(k, &kernel_explorer_panel::threads_requested,      s, &session_api::refresh_threads);
    connect(k, &kernel_explorer_panel::modules_requested,      s, &session_api::refresh_modules);
    connect(k, &kernel_explorer_panel::mutexes_requested,      s, &session_api::refresh_mutexes);
    connect(k, &kernel_explorer_panel::lwmutexes_requested,    s, &session_api::refresh_lwmutexes);
    connect(k, &kernel_explorer_panel::conds_requested,        s, &session_api::refresh_conds);
    connect(k, &kernel_explorer_panel::event_queues_requested, s, &session_api::refresh_event_queues);
    connect(k, &kernel_explorer_panel::containers_requested,   s, &session_api::refresh_containers);
    connect(k, &kernel_explorer_panel::core_dump_requested,    s, &session_api::trigger_core_dump);
    connect(k, &kernel_explorer_panel::resume_requested,       s, &session_api::resume_process);
    connect(k, &kernel_explorer_panel::pause_requested,        s, &session_api::pause_process);
    connect(k, &kernel_explorer_panel::terminate_requested,    s, &session_api::terminate_process);
    connect(s, &session_api::process_list_ready,  k, &kernel_explorer_panel::on_process_list_ready);
    connect(s, &session_api::threads_ready,       k, &kernel_explorer_panel::on_threads_ready);
    connect(s, &session_api::modules_ready,       k, &kernel_explorer_panel::on_modules_ready);
    connect(s, &session_api::mutexes_ready,       k, &kernel_explorer_panel::on_mutexes_ready);
    connect(s, &session_api::lwmutexes_ready,     k, &kernel_explorer_panel::on_lwmutexes_ready);
    connect(s, &session_api::conds_ready,         k, &kernel_explorer_panel::on_conds_ready);
    connect(s, &session_api::event_queues_ready,  k, &kernel_explorer_panel::on_event_queues_ready);
    connect(s, &session_api::containers_ready,    k, &kernel_explorer_panel::on_containers_ready);

}

wire_log_panel* main_window::active_wire() const {
    return active_ ? active_->wire : idle_wire_;
}
console_panel* main_window::active_console() const {
    return active_ ? active_->console : idle_console_;
}
file_explorer_panel* main_window::active_files() const {
    return active_ ? active_->files : idle_files_;
}
kernel_explorer_panel* main_window::active_kernel() const {
    return active_ ? active_->kernel : idle_kernel_;
}

void main_window::close_slot(const QString& id) {
    const auto it = slots_.find(id);
    if (it == slots_.end()) return;
    auto* slot = *it;
    slots_.erase(it);
    if (slot == active_) activate(nullptr);
    // disconnecting only drops the link; a server-backed session also has to be
    // closed or the supervisor keeps the target forever
    slot->session->close_target();
    QObject::disconnect(slot->session, nullptr, this, nullptr);
    slot->session->deleteLater();

    for (auto* w : {static_cast<QWidget*>(slot->files), static_cast<QWidget*>(slot->kernel), static_cast<QWidget*>(slot->console), static_cast<QWidget*>(slot->wire)}) {
        if (auto* stack = qobject_cast<QStackedWidget*>(w->parentWidget())) {
            stack->removeWidget(w);
        }
        w->deleteLater();
    }
    delete slot;
}

void main_window::set_cp_tab_visible(bool on) {
    const int at = explorer_tabs_->indexOf(cp_stack_);
    if (on && at < 0) {
        explorer_tabs_->addTab(cp_stack_, ico("table_gear"), tr("Communications Processor"));
    } else if (!on && at >= 0) {
        explorer_tabs_->removeTab(at);
        cp_stack_->setParent(this);
        cp_stack_->hide();
    }
}

void main_window::activate(target_slot* slot) {
    if (active_ == slot) return;

    for (const auto& c : active_conns_) QObject::disconnect(c);
    active_conns_.clear();

    active_        = slot;
    session_       = slot ? slot->session : nullptr;
    active_target_ = slot ? slot->row : QPersistentModelIndex();
    if (load_controller_) load_controller_->set_session(session_);

    if (session_) wire_active_session();

    files_stack_->setCurrentWidget(active_files());
    kernel_stack_->setCurrentWidget(active_kernel());
    console_stack_->setCurrentWidget(active_console());
    wire_stack_->setCurrentWidget(active_wire());

    // the comms processor is DECR hardware; a DEX has nothing behind this
    const bool has_cp = slot && slot->session
        && slot->session->target().type == opentm::tm_core::target_type::decr_tcp
        && !slot->session->target().host.isEmpty();
    if (has_cp) {
        slot->cp->set_host(slot->session->target().host);
        cp_stack_->setCurrentWidget(slot->cp);
    } else {
        cp_stack_->setCurrentWidget(idle_cp_);
    }
    set_cp_tab_visible(has_cp);

    update_action_states();
    update_state_indicator(session_ ? session_->connection_state() : opentm::tm_core::tcp_connection::state::disconnected);
}

void main_window::wire_active_session() {
    auto* s = session_;
    auto keep = [this](QMetaObject::Connection c) { active_conns_.append(c); };

    keep(connect(s, &session_api::connection_state_changed, this, &main_window::on_connection_state));
    keep(connect(s, &session_api::error, this, &main_window::on_connection_error));
    keep(connect(s, &session_api::session_ready, this, &main_window::on_session_ready_changed));
    keep(connect(s, &session_api::session_invalidated, this, &main_window::on_session_invalidated));
    keep(connect(s, &session_api::session_ready, this, &main_window::on_probe_handshake_acked));
    keep(connect(s, &session_api::load_ext_reply, load_controller_, &load_controller::on_load_ext_reply));
    keep(connect(s, &session_api::load_ext_reply, this, [this](std::uint32_t status) {
        if (status != 0) return;
        QTimer::singleShot(750, this, [this] {
            if (session_ && session_->is_session_ready()) {
                session_->refresh_process_list();
            }
        });
    }));

    keep(connect(s, &session_api::install_progress, this, [this](int percent) {
        if (install_dialog_) install_dialog_->setValue(percent);
        set_status_for_current(tr("Installing... %1%").arg(percent));
    }));
    keep(connect(s, &session_api::install_finished, this, [this](const QString& dir) {
        install_result_path_ = dir;
        if (install_dialog_) install_dialog_->setValue(100);
        set_status_for_current(tr("Installed to %1").arg(dir));
    }));
    keep(connect(s, &session_api::install_reply, this, [this](std::uint32_t status) {
        finish_install_progress(status);
        if (status != 0) {
            set_status_for_current(tr("Install failed: LV2 status 0x%1").arg(status, 8, 16, QChar('0')));
        }
    }));

    keep(connect(reset_action_, &QAction::triggered, s, &session_api::reset_current));
    keep(connect(power_on_action_, &QAction::triggered, s, &session_api::power_on));
    keep(connect(power_off_action_, &QAction::triggered, s, &session_api::power_off));
    keep(connect(power_off_force_action_, &QAction::triggered, s, &session_api::power_off_force));
    keep(connect(wake_on_lan_action_, &QAction::triggered, s, &session_api::wake_on_lan));
}

void main_window::build_menus() {
    auto* file_menu = menuBar()->addMenu(tr("&File"));
    add_action_    = file_menu->addAction(tr("&Add Target"));
    edit_action_   = file_menu->addAction(tr("&Edit Target"));
    properties_action_ = file_menu->addAction(tr("&Properties"));
    remove_action_ = file_menu->addAction(tr("&Remove Target"));
    file_menu->addSeparator();
    import_props_action_ = file_menu->addAction(tr("&Import Properties"));
    export_props_action_ = file_menu->addAction(tr("E&xport Properties"));
    file_menu->addSeparator();
    auto* quit_action = file_menu->addAction(tr("&Quit"));
    quit_action->setShortcut(QKeySequence::Quit);

    connect(add_action_, &QAction::triggered, target_panel_, &target_panel::add_target);
    connect(edit_action_, &QAction::triggered, target_panel_, &target_panel::edit_target);
    connect(properties_action_, &QAction::triggered, target_panel_, &target_panel::edit_properties);
    connect(remove_action_, &QAction::triggered, target_panel_, &target_panel::remove_target);
    connect(import_props_action_, &QAction::triggered, target_panel_, &target_panel::import_properties);
    connect(export_props_action_, &QAction::triggered, target_panel_, &target_panel::export_properties);
    connect(quit_action, &QAction::triggered, this, &QMainWindow::close);

    auto* target_menu = menuBar()->addMenu(tr("&Target"));
    connect_action_    = target_menu->addAction(tr("&Connect"));
    disconnect_action_ = target_menu->addAction(tr("&Disconnect"));
    target_menu->addSeparator();

    auto* power_menu = target_menu->addMenu(tr("&Power Control"));
    power_on_action_ = power_menu->addAction(tr("Switch Power &On"));
    power_off_action_ = power_menu->addAction(tr("&Shutdown"));
    power_off_force_action_ = power_menu->addAction(tr("Shutdown (&Force)"));

    reset_action_ = target_menu->addAction(tr("&Reset"));
    auto* reset_menu = target_menu->addMenu(tr("Reset &Mode"));

    auto* mode_menu = reset_menu;
    reset_mode_group_ = new QActionGroup(this);
    reset_mode_group_->setExclusive(true);
    static const struct { const char* label; int mode; } kModes[] = {
        {"Debug Mode",           0},
        {"System Software Mode", 1},
        {"Release Mode",         2},
        {"Advanced (explicit)",  3},
    };
    for (const auto& mdef : kModes) {
        auto* a = mode_menu->addAction(tr(mdef.label));
        a->setCheckable(true);
        a->setData(mdef.mode);
        reset_mode_group_->addAction(a);
        connect(a, &QAction::triggered, this, [this, m = mdef.mode] {
            target_panel_->set_reset_mode_for_current(m);
        });
    }

    target_menu->addSeparator();
    load_executable_action_  = target_menu->addAction(tr("&Load and Run Executable"));
    load_from_device_action_ = nullptr;
    install_action_ = target_menu->addAction(tr("&Install Package"));
    target_menu->addSeparator();
    wake_on_lan_action_  = target_menu->addAction(tr("&Wake on LAN"));


    connect(connect_action_, &QAction::triggered, this, &main_window::on_connect_target);
    connect(disconnect_action_, &QAction::triggered, this, &main_window::on_disconnect_target);
    connect(load_executable_action_, &QAction::triggered, load_controller_, &load_controller::load_and_run);
    connect(install_action_, &QAction::triggered, this, &main_window::on_install_package);

    connect_action_->setIcon(ico("connect"));
    disconnect_action_->setIcon(ico("disconnect"));
    reset_action_->setIcon(ico("arrow_refresh"));
    power_on_action_->setIcon(ico("control_play_blue"));
    power_off_action_->setIcon(ico("control_stop_blue"));
    power_off_force_action_->setIcon(ico("control_stop_blue"));
    wake_on_lan_action_->setIcon(ico("lightning"));
    load_executable_action_->setIcon(ico("application_go"));
    install_action_->setIcon(ico("package_add"));
    add_action_->setIcon(ico("computer_add"));
    edit_action_->setIcon(ico("computer_edit"));
    remove_action_->setIcon(ico("computer_delete"));
    properties_action_->setIcon(ico("cog"));
    import_props_action_->setIcon(ico("page_white_get"));
    export_props_action_->setIcon(ico("page_white_put"));

    auto* view_menu = menuBar()->addMenu(tr("&View"));
    view_menu->setObjectName(QStringLiteral("ViewMenu"));

    auto* tools_menu = menuBar()->addMenu(tr("&Tools"));
    auto* search_action = tools_menu->addAction(tr("&Search for Targets..."));
    search_action->setIcon(ico("folder_explore"));
    name_action(QStringLiteral("target.search"), search_action, QKeySequence(QStringLiteral("Ctrl+Shift+F")));
    connect(search_action, &QAction::triggered, target_panel_, &target_panel::search_requested);
    tools_menu->addSeparator();

    auto* prefs_action = tools_menu->addAction(tr("&Preferences..."));
    prefs_action->setIcon(ico("cog"));
    name_action(QStringLiteral("tools.preferences"), prefs_action, QKeySequence(QStringLiteral("Ctrl+,")));
    connect(prefs_action, &QAction::triggered, this, &main_window::open_preferences);


    auto* shortcuts_action = tools_menu->addAction(tr("Configure &Shortcuts..."));
    shortcuts_action->setIcon(ico("table_gear"));
    connect(shortcuts_action, &QAction::triggered, this, [this] {
        shortcut_settings_dialog dlg(named_actions_, this);
        dlg.exec();
    });

    auto* help_menu = menuBar()->addMenu(tr("&Help"));
    auto* about_action = help_menu->addAction(tr("&About OpenTM"));
    about_action->setIcon(ico("cog"));
    name_action(QStringLiteral("help.about"), about_action, QKeySequence(QStringLiteral("F1")));
    connect(about_action, &QAction::triggered, this, [this] {
        about_dialog(this).exec();
    });

    // panel focus: cheap way to drive the whole window from the keyboard
    view_menu->addSeparator();
    auto* focus_menu = view_menu->addMenu(tr("&Go To"));
    const struct { const char* key; const char* label; const char* seq; int tab; } kGoTo[] = {
        {"goto.targets",  QT_TR_NOOP("&Targets"),        "Ctrl+1", -1},
        {"goto.files",    QT_TR_NOOP("&File Explorer"),  "Ctrl+2",  0},
        {"goto.kernel",   QT_TR_NOOP("&Kernel Explorer"),"Ctrl+3",  1},
        {"goto.console",  QT_TR_NOOP("&Console"),        "Ctrl+4", -2},
        {"goto.wire",     QT_TR_NOOP("&Wire Log"),       "Ctrl+5", -3},
    };
    for (const auto& g : kGoTo) {
        auto* a = focus_menu->addAction(tr(g.label));
        name_action(QLatin1String(g.key), a, QKeySequence(QLatin1String(g.seq)));
        const int tab = g.tab;
        connect(a, &QAction::triggered, this, [this, tab] {
            if (tab >= 0) {
                explorer_tabs_->setCurrentIndex(tab);
                explorer_tabs_->currentWidget()->setFocus();
            } else if (tab == -1 && target_dock_) {
                target_dock_->show(); target_dock_->raise(); target_panel_->setFocus();
            } else if (tab == -2 && console_dock_) {
                console_dock_->show(); console_dock_->raise();
            } else if (tab == -3 && wire_log_dock_) {
                wire_log_dock_->show(); wire_log_dock_->raise();
            }
        });
    }

    auto* style_menu = view_menu->addMenu(tr("&Style"));
    auto* style_group = new QActionGroup(this);
    style_group->setExclusive(true);
    const auto current_style = QApplication::style()->objectName();
    for (const auto& name : available_styles()) {
        auto* a = style_menu->addAction(name);
        a->setCheckable(true);
        if (name.compare(current_style, Qt::CaseInsensitive) == 0) a->setChecked(true);
        style_group->addAction(a);
        connect(a, &QAction::triggered, this, [name] {
            set_saved_style(name);
            opentm::tm_launch::single_instance::request(QLatin1String(opentm::tm_launch::supervisor_socket), QJsonDocument(QJsonObject{{"method", "restyle"}}).toJson(QJsonDocument::Compact), 500);
        });
    }
}

void main_window::name_action(const QString& key, QAction* a, const QKeySequence& def) {
    if (!a) return;
    a->setShortcut(def);
    // stashed so Configure Shortcuts can offer a Reset
    a->setProperty("default_shortcut", QVariant::fromValue(def));
    a->setShortcutContext(Qt::WindowShortcut);
    named_actions_.insert(key, a);
}

void main_window::register_shortcuts() {
    struct binding { const char* key; QAction* action; const char* seq; };
    const binding kBindings[] = {
        {"target.connect",     connect_action_,         "F7"},
        {"target.disconnect",  disconnect_action_,      "Ctrl+D"},
        {"target.reset",       reset_action_,           "Ctrl+R"},
        {"target.power_on",    power_on_action_,        "F5"},
        {"target.power_off",   power_off_action_,       "F6"},
        {"target.power_kill",  power_off_force_action_, "Shift+F6"},
        {"target.wake_on_lan", wake_on_lan_action_,     "F8"},
        {"target.load",        load_executable_action_, "Ctrl+E"},
        {"target.install",     install_action_,         "Ctrl+I"},
        {"target.add",         add_action_,             "Ctrl+N"},
        {"target.edit",        edit_action_,            "Ctrl+Shift+E"},
        {"target.remove",      remove_action_,          "Delete"},
        {"target.properties",  properties_action_,      "Alt+Return"},
        {"props.import",       import_props_action_,    "Ctrl+Shift+I"},
        {"props.export",       export_props_action_,    "Ctrl+Shift+X"},
    };
    for (const auto& b : kBindings) {
        name_action(QLatin1String(b.key), b.action, QKeySequence(QLatin1String(b.seq)));
    }
    shortcut_settings_dialog::apply_saved(named_actions_);
}

void main_window::run_from_explorer(target_slot* slot, const QString& kit_path) {
    if (!slot || !slot->session) return;
    if (!slot->session->is_session_ready()) {
        stub_status(tr("Connect to the target before running %1.").arg(kit_path));
        return;
    }

    const auto& rec = slot->session->target();
    const auto reply = QMessageBox::question(
        this, tr("Run on Target"),
        tr("Run this executable on %1?\n\n%2\n\n""The target is reset into debug mode first, as Load and Run does.").arg(rec.name.isEmpty() ? rec.host : rec.name, kit_path),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
    if (reply != QMessageBox::Yes) return;

    load_options opts = rec.load;
    opts.reset_target = true;
    opts.clear_streams = true;

    log_wire(tr(">>> Run on Target: %1").arg(kit_path));
    slot->session->load_executable(kit_path, opts);
}

void main_window::save_text_to_file(const QString& text, const QString& suggested_name) {
    QString name = suggested_name;
    name.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9_.-]")), QStringLiteral("_"));
    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
    const QString suggested = QDir(last_save_dir()).filePath(QStringLiteral("%1-%2.log").arg(name, stamp));

    const auto path = QFileDialog::getSaveFileName(this, tr("Save Log"), suggested, tr("Log files (*.log *.txt);;All files (*.*)"));
    if (path.isEmpty()) return;
    set_last_save_dir(QFileInfo(path).absolutePath());

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        stub_status(tr("Could not write %1").arg(path));
        return;
    }
    f.write(text.toUtf8());
    statusBar()->showMessage(tr("Saved %1").arg(path), 5000);
}

void main_window::open_preferences() {
    preferences_dialog dlg(this);
    connect(&dlg, &preferences_dialog::applied, this, [this] {
        for (auto* slot : slots_) {
            slot->console->apply_font_from_prefs();
            slot->session->set_auto_reconnect(auto_reconnect_enabled());
        }
        if (idle_console_) idle_console_->apply_font_from_prefs();
        if (active_ && active_->session && active_->session->is_session_ready()) {
            const auto path = active_->files->current_path();
            if (!path.isEmpty()) active_->session->list_directory(path);
        }
    });
    dlg.exec();
}


void main_window::build_toolbar() {
    main_toolbar_ = addToolBar(tr("Main"));
    main_toolbar_->setObjectName(QStringLiteral("MainToolBar"));
    main_toolbar_->setMovable(true);
    main_toolbar_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    main_toolbar_->addAction(connect_action_);
    main_toolbar_->addAction(disconnect_action_);
    main_toolbar_->addSeparator();
    main_toolbar_->addAction(power_on_action_);
    main_toolbar_->addAction(power_off_action_);
    main_toolbar_->addAction(reset_action_);
    main_toolbar_->addSeparator();
    main_toolbar_->addAction(wake_on_lan_action_);
    main_toolbar_->addSeparator();
    main_toolbar_->addAction(add_action_);
    main_toolbar_->addAction(edit_action_);
    main_toolbar_->addAction(remove_action_);
}

void main_window::build_target_dock() {
    target_panel_ = new target_panel(this);
    connect(target_panel_, &target_panel::selection_changed, this, &main_window::on_target_selection_changed);
    connect(target_panel_, &target_panel::context_menu_requested, this, &main_window::on_target_context_menu);
    connect(target_panel_, &target_panel::target_removed, this, &main_window::close_slot);
    connect(target_panel_, &target_panel::log_message, this, &main_window::log_wire);
    connect(target_panel_, &target_panel::xmb_apply_requested, this, [this](const QString& host_path, quint32 size) {
        if (!session_) return;
        log_wire(tr(">>> XMB overrides -> target, then reset to apply"));
        session_->settings_apply(host_path, size);
        QTimer::singleShot(900, this, [this] {
            if (session_) session_->settings_commit();
        });
        QTimer::singleShot(1800, this, [this] {
            if (session_) session_->reset_current();
        });
    });
    connect(target_panel_, &target_panel::xmb_refresh_requested, this, [this] {
        if (session_) session_->settings_refresh();
    });
    connect(target_panel_, &target_panel::search_requested, this, [this]() {
                discovery_dialog dlg(this);
                if (dlg.exec() != QDialog::Accepted) return;
                int skipped = 0;
                for (const auto& r : dlg.selected_records()) {
                    if (target_panel_->already_present(r)) { ++skipped; continue; }
                    const auto idx = target_panel_->append_discovered(r);
                    if (idx.isValid()) probe_queue_.append(idx);
                }
                if (skipped > 0) {
                    stub_status(tr("%n target(s) already in the list, not added again", "", skipped));
                }
                kick_next_probe();
            });

    target_dock_ = new QDockWidget(tr("Targets"), this);
    target_dock_->setObjectName(QStringLiteral("TargetsDock"));
    target_dock_->setWidget(target_panel_);
    target_dock_->setWindowIcon(ico("computer"));
    target_dock_->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    addDockWidget(Qt::LeftDockWidgetArea, target_dock_);
    resizeDocks({ target_dock_ }, { 320 }, Qt::Horizontal);
    if (auto* view = findChild<QMenu*>(QStringLiteral("ViewMenu"))) {
        view->addAction(target_dock_->toggleViewAction());
    }
}

void main_window::build_central() {
    explorer_tabs_ = new QTabWidget(this);
    explorer_tabs_->setDocumentMode(true);
    explorer_tabs_->setTabPosition(QTabWidget::North);
    explorer_tabs_->setMovable(true);

    files_stack_ = new QStackedWidget(explorer_tabs_);
    idle_files_  = new file_explorer_panel(files_stack_);
    idle_files_->setEnabled(false);
    files_stack_->addWidget(idle_files_);
    explorer_tabs_->addTab(files_stack_, ico("folder_explore"), tr("File Explorer"));

    kernel_stack_ = new QStackedWidget(explorer_tabs_);
    idle_kernel_  = new kernel_explorer_panel(kernel_stack_);
    idle_kernel_->setEnabled(false);
    kernel_stack_->addWidget(idle_kernel_);
    explorer_tabs_->addTab(kernel_stack_, ico("table_gear"), tr("Kernel Explorer"));

    auto add_stub = [this](const QString& title, const QString& body) {
        auto* lbl = new QLabel(body);
        lbl->setAlignment(Qt::AlignCenter);
        lbl->setStyleSheet(QStringLiteral("color:#777; font-style:italic; padding:24px;"));
        explorer_tabs_->addTab(lbl, ico("text_align_left"), title);
    };
    add_stub(tr("File Trace"), tr("File-serving trace - not implemented yet"));

    cp_stack_ = new QStackedWidget(this);
    cp_stack_->hide();
    idle_cp_  = new cp_panel(cp_stack_);
    idle_cp_->setEnabled(false);
    cp_stack_->addWidget(idle_cp_);

    setCentralWidget(explorer_tabs_);
}

void main_window::build_wire_log() {
    wire_stack_ = new QStackedWidget(this);
    idle_wire_  = new wire_log_panel(wire_stack_);
    wire_stack_->addWidget(idle_wire_);

    wire_log_dock_ = new QDockWidget(tr("Wire Log"), this);
    wire_log_dock_->setObjectName(QStringLiteral("WireLogDock"));
    wire_log_dock_->setWidget(wire_stack_);
    wire_log_dock_->setWindowIcon(ico("layout_content"));
    wire_log_dock_->setAllowedAreas(Qt::BottomDockWidgetArea | Qt::RightDockWidgetArea);
    addDockWidget(Qt::BottomDockWidgetArea, wire_log_dock_);
    if (auto* view = findChild<QMenu*>(QStringLiteral("ViewMenu"))) {
        view->addAction(wire_log_dock_->toggleViewAction());
    }

    connect(load_controller_, &load_controller::log_message, this, &main_window::log_wire);
}

void main_window::build_console() {
    console_stack_ = new QStackedWidget(this);
    idle_console_  = new console_panel(console_stack_);
    console_stack_->addWidget(idle_console_);

    console_dock_ = new QDockWidget(tr("Console"), this);
    console_dock_->setObjectName(QStringLiteral("ConsoleDock"));
    console_dock_->setWidget(console_stack_);
    console_dock_->setWindowIcon(ico("application_view_list"));
    console_dock_->setAllowedAreas(Qt::AllDockWidgetAreas);
    addDockWidget(Qt::BottomDockWidgetArea, console_dock_);

    if (wire_log_dock_) {
        tabifyDockWidget(console_dock_, wire_log_dock_);
        console_dock_->raise();
    }
    resizeDocks({ console_dock_ }, { 260 }, Qt::Vertical);
    if (auto* view = findChild<QMenu*>(QStringLiteral("ViewMenu"))) {
        view->addAction(console_dock_->toggleViewAction());
    }
}

void main_window::build_status_indicator() {
    state_indicator_ = new QLabel(this);
    state_indicator_->setMinimumWidth(180);
    state_indicator_->setContentsMargins(8, 0, 8, 0);
    state_indicator_->setTextFormat(Qt::RichText);
    statusBar()->addPermanentWidget(state_indicator_);
}

void main_window::update_state_indicator(opentm::tm_core::tcp_connection::state s) {
    if (!state_indicator_) return;
    using state = opentm::tm_core::tcp_connection::state;
    QString color, text;
    switch (s) {
    case state::disconnected:      color = "#888888"; text = tr("Disconnected"); break;
    case state::tcp_connecting:    color = "#dca42c"; text = tr("Connecting"); break;
    case state::awaiting_greeting: color = "#dca42c"; text = tr("Handshake"); break;
    case state::ready: color = "#3ea65a"; text = (session_ && session_->is_session_ready()) ? tr("Ready (session)") : tr("Ready");
    break;
    case state::error_state:       color = "#c43c3c"; text = tr("Error"); break;
    }
    state_indicator_->setText(QStringLiteral("<span style='color:%1;font-size:14pt;'>&#9679;</span> %2").arg(color, text));
}

void main_window::log_wire(const QString& line) {
    if (auto* w = active_wire()) w->append(line);
}

void main_window::stub_status(const QString& msg) {
    statusBar()->showMessage(msg, 4000);
}

void main_window::set_status_for_current(const QString& s) {
    if (target_panel_ && active_target_.isValid()) {
        target_panel_->set_status_for(active_target_, s);
    }
    statusBar()->showMessage(s, 4000);
}


void main_window::on_connect_target() {
    target_record r;
    QPersistentModelIndex idx;
    if (!target_panel_->current_target(r, &idx)) return;

    auto* slot = slot_for(r, idx);
    if (!slot) return;
    activate(slot);

    using state = opentm::tm_core::tcp_connection::state;
    const auto st = slot->session->connection_state();
    if (st != state::disconnected && st != state::error_state) {
        stub_status(tr("%1 is already connected. Disconnect first.").arg(r.name));
        return;
    }
    session_->set_target(r);
    set_status_for_current(tr("Connecting to %1...").arg(r.host));
    log_wire(tr(">>> connect %1:%2 (%3)").arg(r.host).arg(r.port).arg(QString::fromUtf8(opentm::tm_core::target_type_string(r.type).data())));

    session_->connect_to_target();
}

void main_window::on_disconnect_target() {
    if (!session_) return;
    if (probing_ && !probe_initiated_disconnect_ && !probe_queue_.isEmpty()) {
        log_wire(tr("    -- aborting %1 queued probe(s)").arg(probe_queue_.size()));
        probe_queue_.clear();
    }
    probe_initiated_disconnect_ = false;
    log_wire(tr(">>> disconnect (waiting for kit FIN-ACK)"));
    set_status_for_current(tr("Disconnecting..."));
    session_->disconnect_from_target();
    QCoreApplication::processEvents();
    QThread::msleep(50);
    log_wire(tr("    >> disconnect complete"));
}

void main_window::on_install_package() {
    const QString path = QFileDialog::getOpenFileName(this, tr("Install Package"), QString(),tr("PS3 packages (*.pkg);;All files (*.*)"));
    if (path.isEmpty() || !session_) return;
    session_->install_package(path);
    show_install_progress(QFileInfo(path).fileName());
}

void main_window::show_install_progress(const QString& file_name) {
    close_install_progress();
    install_result_path_.clear();

    install_dialog_ = new QProgressDialog(tr("Installing %1...").arg(file_name), QString(), 0, 100, this);
    install_dialog_->setCancelButton(nullptr);
    install_dialog_->setWindowTitle(tr("Install Package"));
    install_dialog_->setWindowModality(Qt::NonModal);
    install_dialog_->setAutoClose(false);
    install_dialog_->setAutoReset(false);
    install_dialog_->setMinimumDuration(0);
    auto* label = new QLabel(tr("Installing %1...").arg(file_name));
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    label->setWordWrap(true);
    install_dialog_->setLabel(label);

    install_dialog_->setValue(0);
    install_dialog_->show();

    if (install_action_) install_action_->setEnabled(false);
}

void main_window::finish_install_progress(std::uint32_t lv2_status) {
    update_action_states();
    if (!install_dialog_) return;

    install_dialog_->setValue(100);
    if (lv2_status != 0) {
        install_dialog_->setLabelText(tr("Install failed: LV2 status 0x%1").arg(lv2_status, 8, 16, QChar('0')));
    } else if (!install_result_path_.isEmpty()) {
        install_dialog_->setLabelText(tr("Installed to:\n%1").arg(install_result_path_));
    } else {
        install_dialog_->setLabelText(tr("Install complete."));
    }

    auto* close_button = new QPushButton(tr("Close"), install_dialog_);
    install_dialog_->setCancelButton(close_button);
    connect(install_dialog_, &QProgressDialog::canceled, this, &main_window::close_install_progress);
}

void main_window::close_install_progress() {
    if (auto* dialog = install_dialog_) {
        install_dialog_ = nullptr;
        dialog->disconnect(this);
        dialog->close();
        dialog->deleteLater();
    }
    update_action_states();
}

void main_window::on_target_selection_changed() {
    target_record r;
    QPersistentModelIndex idx;
    if (!target_panel_ || !target_panel_->current_target(r, &idx)) {
        activate(nullptr);
        if (load_controller_) {
            load_controller_->set_active_target_defaults({}, {});
            load_controller_->set_base_load_options({});
        }
        return;
    }

    auto* slot = slot_for(r, idx);
    activate(slot);

    if (slot) slot->session->set_target(r);

    update_action_states();

    if (load_controller_) {
        load_controller_->set_active_target_defaults(r.file_server_dir, r.home_dir);
        load_controller_->set_base_load_options(r.load);
    }
    if (reset_mode_group_) {
        for (auto* a : reset_mode_group_->actions()) {
            a->setChecked(a->data().toInt() == r.reset_mode);
        }
    }
}

void main_window::on_target_context_menu(QPoint global_pos, bool has_target) {
    QMenu menu(this);

    if (!has_target) {
        menu.addAction(add_action_);
        menu.exec(global_pos);
        return;
    }

    menu.addAction(connect_action_);
    menu.addAction(disconnect_action_);
    menu.addSeparator();

    auto* power_sub = menu.addMenu(tr("Power Control"));
    power_sub->addAction(power_on_action_);
    power_sub->addAction(power_off_action_);
    power_sub->addAction(power_off_force_action_);

    menu.addAction(reset_action_);
    auto* reset_sub = menu.addMenu(tr("Reset Mode"));
    if (reset_mode_group_) {
        for (auto* a : reset_mode_group_->actions()) reset_sub->addAction(a);
    }

    menu.addSeparator();
    menu.addAction(load_executable_action_);
    menu.addAction(install_action_);
    menu.addAction(wake_on_lan_action_);
    menu.addSeparator();
    menu.addAction(edit_action_);
    menu.addAction(remove_action_);
    menu.addSeparator();
    menu.addAction(import_props_action_);
    menu.addAction(export_props_action_);
    menu.addSeparator();
    menu.addAction(properties_action_);

    menu.exec(global_pos);
}

void main_window::update_action_states() {
    target_record dummy;
    const bool has = target_panel_ && target_panel_->current_target(dummy);
    for (QAction* a : { edit_action_, remove_action_, reset_action_, power_on_action_, power_off_action_, power_off_force_action_, wake_on_lan_action_, load_executable_action_, install_action_ }) {
        if (a) a->setEnabled(has);
    }

    using state = opentm::tm_core::tcp_connection::state;
    const auto st = session_ ? session_->connection_state() : state::disconnected;
    const bool busy = has && st != state::disconnected && st != state::error_state;
    if (connect_action_)    connect_action_->setEnabled(has && !busy);
    if (disconnect_action_) disconnect_action_->setEnabled(busy);
}


void main_window::on_connection_state(opentm::tm_core::tcp_connection::state s) {
    using state = opentm::tm_core::tcp_connection::state;
    update_state_indicator(s);
    update_action_states();
    switch (s) {
    case state::disconnected:
        set_status_for_current(tr("Disconnected"));
        log_wire(tr("    state=disconnected"));

        close_install_progress();

        if (probing_) {
            probing_ = false;
            QTimer::singleShot(0, this, &main_window::kick_next_probe);
        }
        break;
    case state::tcp_connecting:
        set_status_for_current(tr("Connecting..."));
        log_wire(tr("    state=tcp_connecting to %1").arg(session_ ? session_->peer_summary() : QString()));
        break;
    case state::awaiting_greeting:
        set_status_for_current(tr("TCP up, sending version probe..."));
        log_wire(tr("    state=awaiting_greeting - sending version probe"));
        break;
    case state::ready:
        set_status_for_current(tr("Ready"));
        log_wire(tr("    state=ready"));
        break;
    case state::error_state:
        set_status_for_current(tr("Error"));
        log_wire(tr("    state=error"));
        if (probing_) {
            probing_ = false;
            QTimer::singleShot(0, this, &main_window::kick_next_probe);
        }
        break;
    }
}

void main_window::on_greeting_received(QByteArray bytes) {
    log_wire(tr("    >> greeting (%1B): %2").arg(bytes.size()).arg(QString::fromLatin1(bytes.toHex(' '))));
}

void main_window::on_session_ready_changed(std::uint16_t /*token*/, std::uint16_t /*sub_token*/) {
    update_action_states();
}

void main_window::on_session_invalidated() {
    update_action_states();
}

void main_window::on_debug_agent_ready(target_slot* slot) {
    QTimer::singleShot(150, this, [this, slot]() {
        if (!slots_.values().contains(slot)) return;   // closed meanwhile
        if (!slot->session->is_session_ready()) return;
        slot->session->list_directory(slot->files->current_path());
        slot->session->refresh_process_list();
    });
}

void main_window::on_connection_error(QString msg) {
    log_wire(tr("    !! error: %1").arg(msg));
    set_status_for_current(tr("Error: %1").arg(msg));
}

void main_window::on_probe_handshake_acked() {
    if (!probing_) return;
    log_wire(tr("    >> probe complete - disconnecting"));
    QTimer::singleShot(200, this, [this]() {
        if (!probing_) return;
        probe_initiated_disconnect_ = true;
        on_disconnect_target();
    });
}

void main_window::kick_next_probe() {
    if (probing_) return;
    if (probe_queue_.isEmpty()) return;

    auto idx = probe_queue_.takeFirst();
    while (!idx.isValid() && !probe_queue_.isEmpty()) {
        idx = probe_queue_.takeFirst();
    }
    if (!idx.isValid()) return;

    probing_ = true;
    log_wire(tr(">>> auto-probe (Add Selected -> harvest SDK/CP -> disconnect)"));
    target_panel_->select(idx);
    on_connect_target();

    target_record probed;
    QPersistentModelIndex probed_idx;
    const int connect_ms = (target_panel_->current_target(probed, &probed_idx) && probed.timeouts.connect_ms > 0) ? probed.timeouts.connect_ms : opentm::tm_core::target_timeouts{}.connect_ms;

    QTimer::singleShot(connect_ms, this, [this, connect_ms]() {
        if (!probing_) return;
        log_wire(tr("    !! probe timeout (%1ms) - giving up on this target").arg(connect_ms));
        probe_initiated_disconnect_ = true;
        on_disconnect_target();
    });
}

}  // namespace opentm::tm_ui
