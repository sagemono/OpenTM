#include "target_panel.h"

#include <tm_core/arp_lookup.h>
#include <tm_core/target_type.h>
#include <tm_core/wol.h>

#include <QAbstractItemView>
#include <QFileDialog>
#include <QFont>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QItemSelectionModel>
#include <QList>
#include <QMenu>
#include <QPoint>
#include "../target_properties_dialog.h"
#include "../target_properties_xml.h"

#include <QFile>
#include <QMessageBox>

#include <QSettings>
#include <QStandardItem>
#include <QStandardItemModel>
#include <QTreeView>
#include <QUuid>
#include <QVBoxLayout>
#include <QVariantMap>

namespace opentm::tm_ui {

namespace {

using opentm::tm_core::target_type;
using opentm::tm_core::target_type_string;
using opentm::tm_core::target_type_from_string;

constexpr int col_name        = 0;
constexpr int col_status      = 1;
constexpr int column_count    = 2;

constexpr int record_role     = Qt::UserRole + 1;
constexpr int group_type_role = Qt::UserRole + 2;
constexpr int kind_role       = Qt::UserRole + 3;
constexpr int prop_key_role   = Qt::UserRole + 4;

constexpr int kind_search    = 1;
constexpr int kind_group     = 2;
constexpr int kind_target    = 3;
constexpr int kind_property  = 4;

QString group_label(target_type t) {
    switch (t) {
    case target_type::decr_tcp:
        return QObject::tr("PS3_DEH_TCP  (DECR-1000/1400)");
    case target_type::cfw_dex:
        return QObject::tr("PS3_DBG_DEX  (CFW/DEX)");
    case target_type::core_dump:
        return QObject::tr("PS3_CORE_DUMP");
    case target_type::unknown:
        break;
    }
    return QString::fromUtf8(target_type_string(t).data());
}

QString new_target_id() {
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

QVariant to_variant(const target_record& r) {
    QVariantMap m;
    m["id"]              = r.id;
    m["name"]            = r.name;
    m["type"]            = QString::fromUtf8(target_type_string(r.type).data());
    m["host"]            = r.host;
    m["port"]            = r.port;
    m["mac"]             = r.mac;
    m["file_server_dir"] = r.file_server_dir;
    m["home_dir"]        = r.home_dir;
    m["to_default"]      = r.timeouts.default_ms;
    m["to_reset"]        = r.timeouts.reset_ms;
    m["to_connect"]      = r.timeouts.connect_ms;
    m["to_load"]         = r.timeouts.load_ms;
    m["to_status"]       = r.timeouts.status_ms;
    m["to_reconnect"]    = r.timeouts.reconnect_ms;
    m["to_game_port"]    = r.timeouts.game_port_ms;
    m["to_game_exit"]    = r.timeouts.game_exit_ms;
    m["lo_use_elf_stack"]  = r.load.use_elf_stack;
    m["lo_stack_size"]     = static_cast<uint>(r.load.stack_size);
    m["lo_use_elf_prio"]   = r.load.use_elf_priority;
    m["lo_priority"]       = static_cast<uint>(r.load.priority);
    m["lo_wait_bdvd"]      = r.load.wait_for_bdvd;
    m["lo_dbg_module"]     = r.load.enable_debug_module;
    m["lo_ppu_dis"]        = r.load.disable_ppu_debug;
    m["lo_spu_dis"]        = r.load.disable_spu_debug;
    m["lo_extra"]          = r.load.enable_extra_options;
    m["lo_lv2_except"]     = r.load.lv2_exception_handler;
    m["lo_remote_play"]    = r.load.remote_play;
    m["lo_remote_avc"]     = r.load.remote_play_avc;
    m["lo_libprof"]        = r.load.load_libprof;
    m["lo_gcm_debug"]      = r.load.gcm_debug;
    m["lo_smart_capture"]  = r.load.smart_image_capture;
    m["lo_gcm_capture"]    = r.load.gcm_capture_mode;
    m["lo_rsx_prof"]       = r.load.rsx_profiling_tool;
    m["lo_high_mem"]       = r.load.high_memory_footprint;
    m["lo_core_dump"]      = r.load.core_dump;
    m["lo_dump_loc"]       = static_cast<qulonglong>(r.load.core_dump_location);
    m["lo_mat"]            = r.load.memory_access_trap;
    m["lo_game_attr"]      = static_cast<uint>(r.load.game_attribute);
    m["lo_patch_boot"]     = r.load.patch_boot;
    m["lo_paramsfo_map"]   = r.load.paramsfo_mapping;
    m["lo_paramsfo_elfdir"]= r.load.paramsfo_use_elf_dir;
    m["lo_paramsfo_path"]  = r.load.paramsfo_path;
    m["lo_rsx_hud"]        = r.load.rsx_hud;
    m["lo_rsx_hud_on"]     = r.load.rsx_hud_start_on;
    m["lo_trig_ppu"]       = r.load.trig_disable_ppu_exc;
    m["lo_trig_spu"]       = r.load.trig_disable_spu_exc;
    m["lo_trig_rsx"]       = r.load.trig_disable_rsx_exc;
    m["lo_trig_foot"]      = r.load.trig_disable_footswitch;
    m["lo_core_nomem"]     = r.load.corefile_disable_memdump;
    m["lo_exec_restart"]   = r.load.exec_restart_after_dump;
    m["lo_exec_dumpfn"]    = r.load.exec_dump_fn_after_dump;
    m["lo_bootmsg_map"]    = r.load.bootable_msg_mapping;
    m["lo_bootmsg_elfdir"] = r.load.bootable_msg_use_elf_dir;
    m["lo_bootmsg_dir"]    = r.load.bootable_msg_dir;
    m["fs_case"]        = r.force_case_sensitive;
    m["fs_envexp"]      = r.env_var_expansion;
    m["fs_events"]      = r.events_to_log;
    m["fs_logsize"]     = r.file_serving_log_size;
    m["ft_logsize"]     = r.file_trace_log_size;
    m["co_cache_kb"]    = r.console_cache_kb;
    m["ic_auto"]        = r.image_capture_auto;
    m["ic_dir"]         = r.image_capture_dir;
    m["rs_boot_val"]    = static_cast<qulonglong>(r.reset_boot_value);
    m["rs_boot_mask"]   = static_cast<qulonglong>(r.reset_boot_mask);
    m["rs_sys_val"]     = static_cast<qulonglong>(r.reset_system_value);
    m["rs_sys_mask"]    = static_cast<qulonglong>(r.reset_system_mask);
    m["rs_display"]     = r.display_reset_settings;
    m["rs_mode"]        = r.reset_mode;
    return m;
}

target_record from_variant(const QVariant& v) {
    target_record r;
    const auto m = v.toMap();
    r.id   = m.value("id").toString();
    if (r.id.isEmpty()) r.id = new_target_id();   // migrate pre-id settings
    r.name = m.value("name").toString();
    const auto type_str = m.value("type").toString().toStdString();
    if (auto t = target_type_from_string(type_str)) r.type = *t;
    r.host            = m.value("host").toString();
    r.port            = static_cast<quint16>(m.value("port").toUInt());
    r.mac             = m.value("mac").toString();
    r.file_server_dir = m.value("file_server_dir").toString();
    r.home_dir        = m.value("home_dir").toString();
    auto to_int = [&m](const char* k, int fallback) {
        const auto v = m.value(k);
        return v.isValid() ? v.toInt() : fallback;
    };
    r.timeouts.default_ms   = to_int("to_default",   r.timeouts.default_ms);
    r.timeouts.reset_ms     = to_int("to_reset",     r.timeouts.reset_ms);
    r.timeouts.connect_ms   = to_int("to_connect",   r.timeouts.connect_ms);
    r.timeouts.load_ms      = to_int("to_load",      r.timeouts.load_ms);
    r.timeouts.status_ms    = to_int("to_status",    r.timeouts.status_ms);
    r.timeouts.reconnect_ms = to_int("to_reconnect", r.timeouts.reconnect_ms);
    r.timeouts.game_port_ms = to_int("to_game_port", r.timeouts.game_port_ms);
    r.timeouts.game_exit_ms = to_int("to_game_exit", r.timeouts.game_exit_ms);
    auto lo_b = [&m](const char* k, bool d) {const auto v = m.value(k); return v.isValid() ? v.toBool() : d; };
    auto lo_u = [&m](const char* k, quint64 d) {const auto v = m.value(k); return v.isValid() ? v.toULongLong() : d; };
    auto& L = r.load;
    L.use_elf_stack        = lo_b("lo_use_elf_stack", L.use_elf_stack);
    L.stack_size           = static_cast<std::uint32_t>(lo_u("lo_stack_size", L.stack_size));
    L.use_elf_priority     = lo_b("lo_use_elf_prio",  L.use_elf_priority);
    L.priority             = static_cast<std::uint32_t>(lo_u("lo_priority", L.priority));
    L.wait_for_bdvd        = lo_b("lo_wait_bdvd",     L.wait_for_bdvd);
    L.enable_debug_module  = lo_b("lo_dbg_module",    L.enable_debug_module);
    L.disable_ppu_debug    = lo_b("lo_ppu_dis",       L.disable_ppu_debug);
    L.disable_spu_debug    = lo_b("lo_spu_dis",       L.disable_spu_debug);
    L.enable_extra_options = lo_b("lo_extra",         L.enable_extra_options);
    L.lv2_exception_handler= lo_b("lo_lv2_except",    L.lv2_exception_handler);
    L.remote_play          = lo_b("lo_remote_play",   L.remote_play);
    L.remote_play_avc      = lo_b("lo_remote_avc",    L.remote_play_avc);
    L.load_libprof         = lo_b("lo_libprof",       L.load_libprof);
    L.gcm_debug            = lo_b("lo_gcm_debug",     L.gcm_debug);
    L.smart_image_capture  = lo_b("lo_smart_capture", L.smart_image_capture);
    L.gcm_capture_mode     = lo_b("lo_gcm_capture",   L.gcm_capture_mode);
    L.rsx_profiling_tool   = lo_b("lo_rsx_prof",      L.rsx_profiling_tool);
    L.high_memory_footprint= lo_b("lo_high_mem",      L.high_memory_footprint);
    L.core_dump            = lo_b("lo_core_dump",     L.core_dump);
    L.core_dump_location   = lo_u("lo_dump_loc",      L.core_dump_location);
    L.memory_access_trap   = lo_b("lo_mat",           L.memory_access_trap);
    L.game_attribute       = static_cast<std::uint8_t>(lo_u("lo_game_attr", L.game_attribute));
    L.patch_boot           = lo_b("lo_patch_boot",    L.patch_boot);
    auto lo_s = [&m](const char* k, const QString& d) {const auto v = m.value(k); return v.isValid() ? v.toString() : d; };
    L.paramsfo_mapping         = lo_b("lo_paramsfo_map",    L.paramsfo_mapping);
    L.paramsfo_use_elf_dir     = lo_b("lo_paramsfo_elfdir", L.paramsfo_use_elf_dir);
    L.paramsfo_path            = lo_s("lo_paramsfo_path",   L.paramsfo_path);
    L.rsx_hud                  = lo_b("lo_rsx_hud",         L.rsx_hud);
    L.rsx_hud_start_on         = lo_b("lo_rsx_hud_on",      L.rsx_hud_start_on);
    L.trig_disable_ppu_exc     = lo_b("lo_trig_ppu",        L.trig_disable_ppu_exc);
    L.trig_disable_spu_exc     = lo_b("lo_trig_spu",        L.trig_disable_spu_exc);
    L.trig_disable_rsx_exc     = lo_b("lo_trig_rsx",        L.trig_disable_rsx_exc);
    L.trig_disable_footswitch  = lo_b("lo_trig_foot",       L.trig_disable_footswitch);
    L.corefile_disable_memdump = lo_b("lo_core_nomem",      L.corefile_disable_memdump);
    L.exec_restart_after_dump  = lo_b("lo_exec_restart",    L.exec_restart_after_dump);
    L.exec_dump_fn_after_dump  = lo_b("lo_exec_dumpfn",     L.exec_dump_fn_after_dump);
    L.bootable_msg_mapping     = lo_b("lo_bootmsg_map",     L.bootable_msg_mapping);
    L.bootable_msg_use_elf_dir = lo_b("lo_bootmsg_elfdir",  L.bootable_msg_use_elf_dir);
    L.bootable_msg_dir         = lo_s("lo_bootmsg_dir",     L.bootable_msg_dir);
    r.force_case_sensitive  = lo_b("fs_case",    r.force_case_sensitive);
    r.env_var_expansion     = lo_b("fs_envexp",  r.env_var_expansion);
    r.events_to_log         = lo_s("fs_events",  r.events_to_log);
    r.file_serving_log_size = static_cast<int>(lo_u("fs_logsize",  r.file_serving_log_size));
    r.file_trace_log_size   = static_cast<int>(lo_u("ft_logsize",  r.file_trace_log_size));
    r.console_cache_kb      = static_cast<int>(lo_u("co_cache_kb", r.console_cache_kb));
    r.image_capture_auto    = lo_b("ic_auto", r.image_capture_auto);
    r.image_capture_dir     = lo_s("ic_dir",  r.image_capture_dir);
    r.reset_boot_value      = lo_u("rs_boot_val",  r.reset_boot_value);
    r.reset_boot_mask       = lo_u("rs_boot_mask", r.reset_boot_mask);
    r.reset_system_value    = lo_u("rs_sys_val",   r.reset_system_value);
    r.reset_system_mask     = lo_u("rs_sys_mask",  r.reset_system_mask);
    r.display_reset_settings= lo_b("rs_display",   r.display_reset_settings);
    r.reset_mode            = static_cast<int>(lo_u("rs_mode", r.reset_mode));
    return r;
}

const QStringList& property_keys() {
    static const QStringList keys = {
        QStringLiteral("address"),
        QStringLiteral("sdk"),
        QStringLiteral("cp"),
        QStringLiteral("boot mode"),
        QStringLiteral("app_home/"),
        QStringLiteral("home dir"),
    };
    return keys;
}

QString initial_property_value(const QString& key, const target_record& r) {
    if (key == QStringLiteral("address"))   return QString("%1:%2").arg(r.host).arg(r.port);
    if (key == QStringLiteral("app_home/")) return r.file_server_dir;
    if (key == QStringLiteral("home dir"))  return r.home_dir;
    return {}; // sdk is blank until live data arrives
}

QString state_icon_name(opentm::tm_core::tcp_connection::state s) {
    using state = opentm::tm_core::tcp_connection::state;
    switch (s) {
    case state::tcp_connecting:
    case state::awaiting_greeting: return QStringLiteral("clock_go");
    case state::ready:             return QStringLiteral("server_connect");
    case state::error_state:       return QStringLiteral("computer_delete");
    case state::disconnected:      break;
    }
    return QStringLiteral("computer");
}

QList<QStandardItem*> spanned_row(QStandardItem* item) {
    QList<QStandardItem*> row{item};
    for (int c = 1; c < column_count; ++c) {
        auto* spacer = new QStandardItem();
        spacer->setEditable(false);
        spacer->setSelectable(false);
        row.append(spacer);
    }
    return row;
}

} // namespace

target_panel::target_panel(QWidget* parent) : QWidget(parent) {
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    model_ = new QStandardItemModel(0, column_count, this);
    model_->setHeaderData(col_name,   Qt::Horizontal, tr("Name"));
    model_->setHeaderData(col_status, Qt::Horizontal, tr("Status"));

    tree_ = new QTreeView(this);
    tree_->setModel(model_);
    tree_->setRootIsDecorated(true);
    tree_->setItemsExpandable(true);
    tree_->setExpandsOnDoubleClick(false);
    tree_->setSelectionBehavior(QAbstractItemView::SelectRows);
    tree_->setSelectionMode(QAbstractItemView::SingleSelection);
    tree_->setAlternatingRowColors(true);
    tree_->header()->setSectionResizeMode(QHeaderView::Interactive);
    tree_->setColumnWidth(col_name,   180);
    tree_->setColumnWidth(col_status, 110);
    outer->addWidget(tree_);

    connect(tree_->selectionModel(), &QItemSelectionModel::selectionChanged, this, &target_panel::on_tree_selection_changed);
    connect(tree_, &QTreeView::doubleClicked, this, &target_panel::on_tree_double_clicked);

    tree_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(tree_, &QTreeView::customContextMenuRequested, this, &target_panel::on_tree_context_menu);

    ensure_search_row();
    load_targets();
}

target_panel::~target_panel() = default;

void target_panel::ensure_search_row() {
    auto* root = model_->invisibleRootItem();
    // already there?
    if (root->rowCount() > 0) {
        auto* first = root->child(0, col_name);
        if (first && first->data(kind_role).toInt() == kind_search) return;
    }
    auto* item = new QStandardItem(tr("Search for Targets..."));
    item->setEditable(false);
    item->setIcon(QIcon(QStringLiteral(":/icons/folder_explore.bmp")));
    item->setData(kind_search, kind_role);
    QFont f = item->font();
    f.setItalic(true);
    item->setFont(f);
    root->insertRow(0, spanned_row(item));
    tree_->setFirstColumnSpanned(0, QModelIndex(), true);
}

bool target_panel::current_target(target_record& out, QPersistentModelIndex* idx_out) const
{
    auto idx = tree_->selectionModel()->currentIndex();
    if (!idx.isValid()) return false;
    // wa;lk up to the target row if the user clicked a property child
    auto name_idx = idx.sibling(idx.row(), col_name);
    auto* it = model_->itemFromIndex(name_idx);
    if (!it) return false;
    if (it->data(kind_role).toInt() == kind_property) {
        auto parent = name_idx.parent();
        if (!parent.isValid()) return false;
        name_idx = parent;
        it = model_->itemFromIndex(name_idx);
        if (!it) return false;
    }
    if (it->data(kind_role).toInt() != kind_target) return false;
    const auto v = it->data(record_role);
    if (!v.isValid()) return false;
    out = from_variant(v);
    if (idx_out) *idx_out = QPersistentModelIndex(name_idx);
    return true;
}

void target_panel::set_status_for(const QPersistentModelIndex& idx, const QString& status)
{
    if (!idx.isValid()) return;
    const auto status_idx = idx.sibling(idx.row(), col_status);
    if (auto* it = model_->itemFromIndex(status_idx)) it->setText(status);
}

void target_panel::set_state_icon_for(const QPersistentModelIndex& idx, opentm::tm_core::tcp_connection::state s)
{
    if (!idx.isValid()) return;
    auto* it = model_->itemFromIndex(idx.sibling(idx.row(), col_name));
    if (!it || it->data(kind_role).toInt() != kind_target) return;
    it->setIcon(QIcon(QStringLiteral(":/icons/%1.bmp").arg(state_icon_name(s))));
}

void target_panel::populate_properties(QStandardItem* name_item, const target_record& r)
{
    if (!name_item) return;
    // clear and rebuild as its simpler than diffing
    name_item->removeRows(0, name_item->rowCount());
    for (const auto& key : property_keys()) {
        auto* k = new QStandardItem(key);
        k->setEditable(false);
        k->setSelectable(false);
        k->setData(kind_property, kind_role);
        k->setData(key, prop_key_role);
        QFont f = k->font();
        f.setItalic(true);
        k->setFont(f);
        auto* v = new QStandardItem(initial_property_value(key, r));
        v->setEditable(false);
        v->setSelectable(false);

        name_item->appendRow({ k, v });
    }
}

void target_panel::set_property_for(const QPersistentModelIndex& idx,const QString& key, const QString& value)
{
    if (!idx.isValid()) return;
    auto* parent = model_->itemFromIndex(idx);
    if (!parent || parent->data(kind_role).toInt() != kind_target) return;
    for (int row = 0; row < parent->rowCount(); ++row) {
        auto* k = parent->child(row, col_name);
        if (!k || k->data(kind_role).toInt() != kind_property) continue;
        if (k->data(prop_key_role).toString() != key) continue;
        if (auto* v = parent->child(row, col_status)) v->setText(value);
        return;
    }
}

bool target_panel::already_present(const target_record& r) const {
    const auto mac = r.mac.trimmed().toLower();
    for (int row = 0; row < model_->rowCount(); ++row) {
        auto* item = model_->item(row, col_name);
        if (!item || item->data(kind_role).toInt() != kind_target) continue;
        const auto existing = from_variant(item->data(record_role));
        if (!mac.isEmpty() && existing.mac.trimmed().toLower() == mac) return true;
        if (existing.host == r.host && existing.port == r.port) return true;
    }
    return false;
}

QPersistentModelIndex target_panel::append_discovered(const target_record& r) {
    auto idx = append_target_row(r);
    save_targets();
    return idx;
}

void target_panel::select(const QPersistentModelIndex& idx) {
    if (!idx.isValid()) return;
    tree_->selectionModel()->setCurrentIndex(idx, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    tree_->scrollTo(idx);
}

void target_panel::add_target() {
    add_target_dialog dlg(this);
    if (dlg.exec() != QDialog::Accepted) return;
    append_target_row(dlg.record());
    save_targets();
}

void target_panel::edit_target() {
    target_record cur;
    QPersistentModelIndex idx;
    if (!current_target(cur, &idx)) return;
    add_target_dialog dlg(this);
    dlg.setWindowTitle(tr("Edit Target"));
    dlg.set_record(cur);
    if (dlg.exec() != QDialog::Accepted) return;
    set_target_row(idx, dlg.record());
    save_targets();
}

void target_panel::import_properties() {
    target_record cur;
    QPersistentModelIndex idx;
    if (!current_target(cur, &idx)) {
        QMessageBox::information(this, tr("Import Properties"), tr("Select a target first - import overwrites the settings of " "the selected target."));
        return;
    }

    const QString path = QFileDialog::getOpenFileName(this, tr("Import Properties"), QString(), tr("Target properties (*.xml);;All files (*.*)"));
    if (path.isEmpty()) return;

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, tr("Import Properties"), tr("Could not read %1").arg(path));
        return;
    }

    QString error;
    if (!target_properties_from_xml(f.readAll(), cur, &error)) {
        QMessageBox::warning(this, tr("Import Properties"), tr("%1 is not a valid properties file:\n%2").arg(path, error));
        return;
    }

    if (!idx.isValid()) return;
    set_target_row(idx, cur);
    save_targets();
}

void target_panel::export_properties() {
    target_record cur;
    if (!current_target(cur)) {
        QMessageBox::information(this, tr("Export Properties"), tr("Select a target first."));
        return;
    }

    QString path = QFileDialog::getSaveFileName(this, tr("Export Properties"), cur.name.isEmpty() ? QStringLiteral("target.xml") : cur.name + QStringLiteral(".xml"), tr("Target properties (*.xml);;All files (*.*)"));
    if (path.isEmpty()) return;
    if (!path.contains(QLatin1Char('.'))) path += QStringLiteral(".xml");

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QMessageBox::warning(this, tr("Export Properties"), tr("Could not write %1").arg(path));
        return;
    }
    f.write(target_properties_to_xml(cur));
}

void target_panel::edit_properties() {
    target_record cur;
    QPersistentModelIndex idx;
    if (!current_target(cur, &idx)) return;
    auto* dlg = new target_properties_dialog(cur, this);
    connect(dlg, &target_properties_dialog::applied, this, [this, idx](const target_record& r) {
        if (!idx.isValid()) return;   // row removed while the dialog was open
        set_target_row(idx, r);
        save_targets();
    });
    connect(dlg, &target_properties_dialog::xmb_apply_requested, this, &target_panel::xmb_apply_requested);
    connect(dlg, &target_properties_dialog::xmb_refresh_requested, this, &target_panel::xmb_refresh_requested);
    dlg->show();
    dlg->raise();
    dlg->activateWindow();
}

void target_panel::resolve_mac_for_current() {
    target_record cur;
    QPersistentModelIndex idx;
    if (!current_target(cur, &idx)) return;

    const auto mac = opentm::tm_core::arp_lookup(cur.host);
    if (!mac) {
        QMessageBox::information(this, tr("Resolve MAC"), tr("No ARP entry for %1.\n\n""The host only caches addresses it has recently talked to on the ""same subnet - connect to the target first, and note that ARP ""does not cross a router.").arg(cur.host));
        return;
    }

    cur.mac = QString::fromStdString(opentm::tm_core::format_mac(*mac));
    set_target_row(idx, cur);
    save_targets();
    emit selection_changed();
    emit log_message(tr("    -- ARP: %1 is at %2").arg(cur.host, cur.mac));
}

void target_panel::set_reset_mode_for_current(int mode) {
    target_record cur;
    QPersistentModelIndex idx;
    if (!current_target(cur, &idx)) return;
    cur.reset_mode = mode;
    set_target_row(idx, cur);
    save_targets();
}

void target_panel::remove_target() {
    target_record cur;
    QPersistentModelIndex idx;
    if (!current_target(cur, &idx)) return;
    auto* parent_item = model_->itemFromIndex(idx.parent());
    if (!parent_item) return;
    parent_item->removeRow(idx.row());
    if (parent_item->rowCount() == 0
        && parent_item != model_->invisibleRootItem()) {
        model_->invisibleRootItem()->removeRow(parent_item->row());
    }
    save_targets();
    emit target_removed(cur.id);
    emit selection_changed();
}

void target_panel::on_tree_selection_changed() {
    emit selection_changed();
}

void target_panel::on_tree_double_clicked(const QModelIndex& idx) {
    if (!idx.isValid()) return;
    const auto name_idx = idx.sibling(idx.row(), col_name);
    auto* it = model_->itemFromIndex(name_idx);
    if (!it) return;
    const int kind = it->data(kind_role).toInt();
    if (kind == kind_search) {
        emit search_requested();
        return;
    }
    if (kind == kind_target) {
        edit_target();
        return;
    }
    if (kind == kind_property) {
        const auto key = it->data(prop_key_role).toString();
        if (key != QStringLiteral("app_home/")
            && key != QStringLiteral("home dir"))
        {
            return;
        }
        const auto parent_idx = name_idx.parent();
        if (!parent_idx.isValid()) return;
        auto* target_item = model_->itemFromIndex(parent_idx);
        if (!target_item || target_item->data(kind_role).toInt() != kind_target) return;
        auto r = from_variant(target_item->data(record_role));
        const auto seed = (key == QStringLiteral("app_home/")) ? r.file_server_dir : r.home_dir;
        const auto picked = QFileDialog::getExistingDirectory(this, tr("Select %1 for %2").arg(key, r.name), seed);
        if (picked.isEmpty()) return;
        if (key == QStringLiteral("app_home/")) {
            r.file_server_dir = picked;
        } else {
            r.home_dir = picked;
        }
        target_item->setData(to_variant(r), record_role);
        set_property_for(QPersistentModelIndex(parent_idx), key, picked);
        save_targets();
        emit selection_changed();
    }
}

void target_panel::on_tree_context_menu(const QPoint& pos) {
    const auto idx = tree_->indexAt(pos);
    const auto global_pos = tree_->viewport()->mapToGlobal(pos);
    bool has_target = false;

    if (idx.isValid()) {
        auto* it = model_->itemFromIndex(idx.sibling(idx.row(), col_name));
        if (it) {
            const int kind = it->data(kind_role).toInt();
            if (kind == kind_target) {
                has_target = true;
                tree_->selectionModel()->setCurrentIndex(idx, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
            } else if (kind == kind_property) {
                const auto parent = idx.parent();
                if (parent.isValid()) {
                    tree_->selectionModel()->setCurrentIndex(parent, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
                    has_target = true;
                }
            }
        }
    }
    emit context_menu_requested(global_pos, has_target);
}

QStandardItem* target_panel::find_or_create_group(target_type t) {
    auto* root = model_->invisibleRootItem();
    for (int i = 0; i < root->rowCount(); ++i) {
        auto* g = root->child(i, col_name);
        if (g && g->data(kind_role).toInt() == kind_group
              && g->data(group_type_role).toInt() == static_cast<int>(t))
        {
            return g;
        }
    }
    auto* g = new QStandardItem(group_label(t));
    g->setData(static_cast<int>(t), group_type_role);
    g->setData(kind_group, kind_role);
    g->setEditable(false);
    g->setSelectable(false);
    QFont f = g->font();
    f.setBold(true);
    g->setFont(f);
    root->appendRow(spanned_row(g));
    tree_->setFirstColumnSpanned(g->row(), QModelIndex(), true);
    return g;
}

QPersistentModelIndex target_panel::append_target_row(const target_record& in) {
    target_record r = in;
    if (r.id.isEmpty()) r.id = new_target_id();
    auto* group = find_or_create_group(r.type);
    auto* name = new QStandardItem(r.name);
    name->setIcon(QIcon(QStringLiteral(":/icons/computer.bmp")));
    name->setData(to_variant(r), record_role);
    name->setData(kind_target, kind_role);
    auto* st   = new QStandardItem(tr("Disconnected"));
    for (auto* it : { name, st }) it->setEditable(false);
    group->appendRow({ name, st });
    populate_properties(name, r);
    tree_->expand(group->index());
    emit selection_changed();
    return QPersistentModelIndex(name->index());
}

void target_panel::set_target_row(const QPersistentModelIndex& idx, const target_record& in)
{
    if (!idx.isValid()) return;
    auto* parent_item = idx.parent().isValid() ? model_->itemFromIndex(idx.parent()) : model_->invisibleRootItem();
    if (!parent_item) return;
    const int row = idx.row();
    auto* name = parent_item->child(row, col_name);
    if (!name) return;
    // identity is never taken from the incoming record: an edit or an imported
    // properties file must not be able to re-point the row at another session
    target_record r = in;
    r.id = from_variant(name->data(record_role)).id;
    if (r.id.isEmpty()) r.id = new_target_id();
    name->setText(r.name);
    name->setData(to_variant(r), record_role);
    set_property_for(idx, QStringLiteral("address"), QString("%1:%2").arg(r.host).arg(r.port));
    set_property_for(idx, QStringLiteral("app_home/"), r.file_server_dir);
    set_property_for(idx, QStringLiteral("home dir"),  r.home_dir);

    // if the user changed the type, migrate the row to the matching group
    const auto parent_type = parent_item->data(group_type_role);
    if (parent_type.isValid() && parent_type.toInt() != static_cast<int>(r.type)) {
        auto taken = parent_item->takeRow(row);
        auto* new_group = find_or_create_group(r.type);
        new_group->appendRow(taken);
        tree_->expand(new_group->index());
        if (!taken.isEmpty() && taken.first()) {
            tree_->selectionModel()->setCurrentIndex(taken.first()->index(), QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        }
        if (parent_item->rowCount() == 0
            && parent_item != model_->invisibleRootItem()) {
            model_->invisibleRootItem()->removeRow(parent_item->row());
        }
    }

    emit target_record_changed(r);
}

void target_panel::load_targets() {
    QSettings s;
    bool migrated = false;
    const int n = s.beginReadArray("targets");
    for (int i = 0; i < n; ++i) {
        s.setArrayIndex(i);
        QVariantMap m;
        for (const auto& key : s.childKeys()) m[key] = s.value(key);
        if (m.value("id").toString().isEmpty()) migrated = true;
        append_target_row(from_variant(m));
    }
    s.endArray();
    if (migrated) save_targets();   // persist ids minted for pre-id targets
}

void target_panel::save_targets() const {
    QSettings s;
    s.beginWriteArray("targets");
    auto* root = model_->invisibleRootItem();
    int i = 0;
    for (int g = 0; g < root->rowCount(); ++g) {
        auto* group = root->child(g, col_name);
        if (!group) continue;
        if (group->data(kind_role).toInt() != kind_group) continue;
        for (int c = 0; c < group->rowCount(); ++c) {
            auto* name = group->child(c, col_name);
            if (!name) continue;
            if (name->data(kind_role).toInt() != kind_target) continue;
            const auto v = name->data(record_role);
            if (!v.isValid()) continue;
            const auto r = from_variant(v);
            s.setArrayIndex(i++);
            const auto m = to_variant(r).toMap();
            for (auto it = m.constBegin(); it != m.constEnd(); ++it) {
                s.setValue(it.key(), it.value());
            }
        }
    }
    s.endArray();
}

} // namespace opentm::tm_ui
