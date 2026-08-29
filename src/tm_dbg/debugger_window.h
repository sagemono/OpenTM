#pragma once

#include <tm_session/session_api.h>
#include <tm_session/session_factory.h>
#include <tm_session/target_record.h>

#include <QMainWindow>
#include <QString>

#include <cstdint>

class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;

namespace opentm::tm_ui {

class debugger_panel;

class debugger_window : public QMainWindow {
    Q_OBJECT
public:
    explicit debugger_window(const session_backend& backend, const target_record& target, QWidget* parent = nullptr);
    ~debugger_window() override;

    void attach();
    // resume the process rather than leaving it stopped behind 
    void closeEvent(QCloseEvent* e) override;
    // ask the session server which consoles it already has open
    void discover_targets();

private:
    void build_ui();
    target_record selected_target() const;
    void wire_session();
    void ask_for_process_list();
    void try_symbols_for(const QString& kit_path);
    void append_log(const QString& line);
    void set_state(const QString& text);

    session_backend  backend_;
    target_record    target_;
    session_api*     session_ = nullptr;
    bool             ready_        = false;
    bool             asked_for_processes_ = false;
    std::uint32_t    pid_          = 0;
    QString          self_path_;
    bool             warned_stale_ = false;

    debugger_panel*  panel_    = nullptr;
    QPlainTextEdit*  log_      = nullptr;
    QLabel*          state_    = nullptr;
    QComboBox*       target_box_  = nullptr;
    QPushButton*     attach_btn_  = nullptr;
};

} // namespace opentm::tm_ui
