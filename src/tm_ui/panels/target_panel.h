#pragma once

#include "../add_target_dialog.h"

#include <tm_core/tcp_connection.h>

#include <QPersistentModelIndex>
#include <QString>
#include <QWidget>

class QPoint;
class QStandardItem;
class QStandardItemModel;
class QTreeView;

namespace opentm::tm_ui {

class target_panel : public QWidget {
    Q_OBJECT
public:
    explicit target_panel(QWidget* parent = nullptr);
    ~target_panel() override;

    bool current_target(target_record& out, QPersistentModelIndex* idx_out = nullptr) const;
    void set_status_for(const QPersistentModelIndex& idx, const QString& status);
    void set_state_icon_for(const QPersistentModelIndex& idx, opentm::tm_core::tcp_connection::state s);
    void set_property_for(const QPersistentModelIndex& idx, const QString& key, const QString& value);

    QPersistentModelIndex append_discovered(const target_record& r);
    // A console is the same console whatever it is called: match on MAC when
    // one is known, and fall back to the endpoint.
    bool already_present(const target_record& r) const;

    void select(const QPersistentModelIndex& idx);

public slots:
    void add_target();
    void edit_target();
    void edit_properties();
    void import_properties();
    void export_properties();
    void resolve_mac_for_current();
    void set_reset_mode_for_current(int mode);
    void remove_target();

signals:
    void log_message(QString line);
    void selection_changed();
    void connect_requested();
    void search_requested();
    void context_menu_requested(QPoint global_pos, bool has_target);
    void target_removed(QString id);
    void target_record_changed(target_record r);
    void xmb_apply_requested(QString host_path, quint32 file_size);
    void xmb_refresh_requested();

private slots:
    void on_tree_selection_changed();
    void on_tree_context_menu(const QPoint& pos);
    void on_tree_double_clicked(const QModelIndex& idx);

private:
    void ensure_search_row();
    void load_targets();
    void save_targets() const;
    QPersistentModelIndex append_target_row(const target_record& r);
    void set_target_row(const QPersistentModelIndex& idx, const target_record& r);
    void populate_properties(QStandardItem* name_item, const target_record& r);
    QStandardItem* find_or_create_group(opentm::tm_core::target_type t);

    QStandardItemModel* model_ = nullptr;
    QTreeView*          tree_  = nullptr;
};

} // namespace opentm::tm_ui
