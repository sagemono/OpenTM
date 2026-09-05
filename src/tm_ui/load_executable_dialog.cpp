#include "load_executable_dialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QVBoxLayout>

namespace {
constexpr auto k_settings_group = "LoadExecutableDialog";
}

namespace opentm::tm_ui {

load_executable_dialog::load_executable_dialog(const QString& default_app_home, const QString& default_home, QWidget* parent) : QDialog(parent)
{
    setWindowTitle(tr("Load and Run Executable"));
    setModal(true);
    resize(640, 480);

    auto* outer = new QVBoxLayout(this);

    {
        host_file_row_ = new QWidget(this);
        auto* row = new QHBoxLayout(host_file_row_);
        row->setContentsMargins(0, 0, 0, 0);
        file_path_edit_ = new QLineEdit(this);
        file_path_edit_->setPlaceholderText(tr("Path to .self / .elf / .bin on this machine"));
        browse_btn_ = new QPushButton(tr("&Browse..."), this);
        row->addWidget(new QLabel(tr("File name:"), this), 0);
        row->addWidget(file_path_edit_, 1);
        row->addWidget(browse_btn_, 0);
        outer->addWidget(host_file_row_);
    }

    {
        auto* grp = new QGroupBox(tr("Options"), this);
        auto* g = new QVBoxLayout(grp);

        auto* devrow = new QHBoxLayout;
        device_check_ = new QCheckBox(tr("&Load from device"), this);
        device_combo_ = new QComboBox(this);
        for (const auto* d : { "/dev_bdvd/", "/app_home/", "/host_root/", "/dev_hdd0/", "/dev_ms/", "/dev_usb000/" })
        {
            device_combo_->addItem(QString::fromUtf8(d));
        }
        device_combo_->setEnabled(false);
        device_path_edit_ = new QLineEdit(this);
        device_path_edit_->setPlaceholderText(tr("remainder of kit path"));
        device_path_edit_->setEnabled(false);
        devrow->addWidget(device_check_, 0);
        devrow->addWidget(device_combo_, 0);
        devrow->addWidget(device_path_edit_, 1);
        g->addLayout(devrow);

        host_dirs_ = new QWidget(this);
        auto* hostdirs = new QVBoxLayout(host_dirs_);
        hostdirs->setContentsMargins(0, 0, 0, 0);
        {
            auto* row = new QHBoxLayout;
            app_home_check_ = new QCheckBox(tr("Set file-serving directory (&app_home/)"), this);
            app_home_edit_ = new QLineEdit(this);
            app_home_edit_->setText(default_app_home);
            app_home_edit_->setEnabled(false);
            app_home_browse_ = new QPushButton(tr("..."), this);
            app_home_browse_->setMaximumWidth(32);
            app_home_browse_->setEnabled(false);
            app_home_use_elf_ = new QCheckBox(tr("Use ELF directory"), this);
            app_home_use_elf_->setEnabled(false);
            row->addWidget(app_home_check_, 0);
            row->addWidget(app_home_edit_, 1);
            row->addWidget(app_home_browse_, 0);
            row->addWidget(app_home_use_elf_, 0);
            hostdirs->addLayout(row);
        }

        {
            auto* row = new QHBoxLayout;
            home_check_ = new QCheckBox(tr("Set home directory (~/)"), this);
            home_edit_ = new QLineEdit(this);
            home_edit_->setText(default_home);
            home_edit_->setEnabled(false);
            home_browse_ = new QPushButton(tr("..."), this);
            home_browse_->setMaximumWidth(32);
            home_browse_->setEnabled(false);
            home_use_elf_ = new QCheckBox(tr("Use ELF directory"), this);
            home_use_elf_->setEnabled(false);
            row->addWidget(home_check_, 0);
            row->addWidget(home_edit_, 1);
            row->addWidget(home_browse_, 0);
            row->addWidget(home_use_elf_, 0);
            hostdirs->addLayout(row);
        }
        g->addWidget(host_dirs_);

        {
            auto* row = new QHBoxLayout;
            row->addWidget(new QLabel(tr("Command-line parameters:"), this), 0);
            cmdline_edit_ = new QLineEdit(this);
            row->addWidget(cmdline_edit_, 1);
            g->addLayout(row);
        }

        {
            auto* row = new QHBoxLayout;
            reset_check_ = new QCheckBox(tr("&Reset target"), this);
            clear_streams_check_ = new QCheckBox(tr("&Clear console output streams"), this);
            clear_streams_check_->setChecked(true);
            row->addWidget(reset_check_, 0);
            row->addWidget(clear_streams_check_, 0);
            row->addStretch(1);
            g->addLayout(row);
        }
        {
            auto* row = new QHBoxLayout;
            enable_debug_check_ = new QCheckBox(tr("&Enable debugging of module"), this);
            disable_ppu_check_ = new QCheckBox(tr("Disable PPU debugging"), this);
            disable_spu_check_ = new QCheckBox(tr("Disable SPU debugging"), this);
            row->addWidget(enable_debug_check_, 0);
            row->addWidget(disable_ppu_check_, 0);
            row->addWidget(disable_spu_check_, 0);
            row->addStretch(1);
            g->addLayout(row);
        }

        outer->addWidget(grp);
    }

    auto* btns = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    btns->button(QDialogButtonBox::Ok)->setText(tr("Open"));
    outer->addWidget(btns);

    connect(btns, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(btns, &QDialogButtonBox::rejected, this, &QDialog::reject);

    connect(device_check_, &QCheckBox::toggled, this, &load_executable_dialog::on_device_toggled);
    connect(app_home_check_, &QCheckBox::toggled, this, [this](bool on) {
        app_home_edit_->setEnabled(on && !app_home_use_elf_->isChecked());
        app_home_browse_->setEnabled(on && !app_home_use_elf_->isChecked());
        app_home_use_elf_->setEnabled(on);
    });
    connect(home_check_, &QCheckBox::toggled, this, [this](bool on) {
        home_edit_->setEnabled(on && !home_use_elf_->isChecked());
        home_browse_->setEnabled(on && !home_use_elf_->isChecked());
        home_use_elf_->setEnabled(on);
    });
    connect(app_home_use_elf_, &QCheckBox::toggled, this, &load_executable_dialog::on_app_home_use_elf_toggled);
    connect(home_use_elf_, &QCheckBox::toggled, this, &load_executable_dialog::on_home_use_elf_toggled);
    connect(browse_btn_, &QPushButton::clicked, this, &load_executable_dialog::on_browse_file);
    connect(app_home_browse_, &QPushButton::clicked, this, &load_executable_dialog::on_browse_app_home);
    connect(home_browse_, &QPushButton::clicked, this, &load_executable_dialog::on_browse_home);

    restore_state();
}

void load_executable_dialog::done(int result) {
    save_state();
    QDialog::done(result);
}

void load_executable_dialog::save_state() const {
    QSettings s;
    s.beginGroup(k_settings_group);
    s.setValue("file_path",            file_path_edit_->text());
    s.setValue("device_checked",       device_check_->isChecked());
    s.setValue("device_prefix",        device_combo_->currentText());
    s.setValue("device_path",          device_path_edit_->text());
    s.setValue("app_home_checked",     app_home_check_->isChecked());
    s.setValue("app_home_path",        app_home_edit_->text());
    s.setValue("app_home_use_elf",     app_home_use_elf_->isChecked());
    s.setValue("home_checked",         home_check_->isChecked());
    s.setValue("home_path",            home_edit_->text());
    s.setValue("home_use_elf",         home_use_elf_->isChecked());
    s.setValue("cmdline",              cmdline_edit_->text());
    s.setValue("reset_target",         reset_check_->isChecked());
    s.setValue("clear_streams",        clear_streams_check_->isChecked());
    s.setValue("enable_debug_module",  enable_debug_check_->isChecked());
    s.setValue("disable_ppu_debug",    disable_ppu_check_->isChecked());
    s.setValue("disable_spu_debug",    disable_spu_check_->isChecked());
    s.setValue("geometry",             const_cast<load_executable_dialog*>(this)->saveGeometry());
    s.endGroup();
}

void load_executable_dialog::restore_state() {
    QSettings s;
    s.beginGroup(k_settings_group);

    if (s.contains("file_path"))
        file_path_edit_->setText(s.value("file_path").toString());

    if (s.contains("device_prefix")) {
        const auto prefix = s.value("device_prefix").toString();
        const int idx = device_combo_->findText(prefix);
        if (idx >= 0) device_combo_->setCurrentIndex(idx);
    }
    if (s.contains("device_path"))
        device_path_edit_->setText(s.value("device_path").toString());
    if (s.contains("device_checked"))
        device_check_->setChecked(s.value("device_checked").toBool());

    if (s.contains("app_home_path"))
        app_home_edit_->setText(s.value("app_home_path").toString());
    if (s.contains("app_home_checked"))
        app_home_check_->setChecked(s.value("app_home_checked").toBool());
    if (s.contains("app_home_use_elf"))
        app_home_use_elf_->setChecked(s.value("app_home_use_elf").toBool());

    if (s.contains("home_path"))
        home_edit_->setText(s.value("home_path").toString());
    if (s.contains("home_checked"))
        home_check_->setChecked(s.value("home_checked").toBool());
    if (s.contains("home_use_elf"))
        home_use_elf_->setChecked(s.value("home_use_elf").toBool());

    if (s.contains("cmdline"))
        cmdline_edit_->setText(s.value("cmdline").toString());
    if (s.contains("reset_target"))
        reset_check_->setChecked(s.value("reset_target").toBool());
    if (s.contains("clear_streams"))
        clear_streams_check_->setChecked(s.value("clear_streams").toBool());
    if (s.contains("enable_debug_module"))
        enable_debug_check_->setChecked(s.value("enable_debug_module").toBool());
    if (s.contains("disable_ppu_debug"))
        disable_ppu_check_->setChecked(s.value("disable_ppu_debug").toBool());
    if (s.contains("disable_spu_debug"))
        disable_spu_check_->setChecked(s.value("disable_spu_debug").toBool());

    if (s.contains("geometry"))
        restoreGeometry(s.value("geometry").toByteArray());

    s.endGroup();
}

void load_executable_dialog::on_device_toggled(bool checked) {
    device_combo_->setEnabled(checked);
    device_path_edit_->setEnabled(checked);
    host_file_row_->setVisible(!checked);
    host_dirs_->setVisible(!checked);
    adjustSize();
}

void load_executable_dialog::on_app_home_use_elf_toggled(bool checked) {
    app_home_edit_->setEnabled(app_home_check_->isChecked() && !checked);
    app_home_browse_->setEnabled(app_home_check_->isChecked() && !checked);
}

void load_executable_dialog::on_home_use_elf_toggled(bool checked) {
    home_edit_->setEnabled(home_check_->isChecked() && !checked);
    home_browse_->setEnabled(home_check_->isChecked() && !checked);
}

void load_executable_dialog::on_browse_file() {
    const auto file = QFileDialog::getOpenFileName(this, tr("Load and Run Executable"), file_path_edit_->text(), tr("SELF Files (*.self);;ELF Files (*.elf);;EBOOT Files (EBOOT.BIN eboot.bin);;All Files (*)"));
    if (!file.isEmpty()) file_path_edit_->setText(file);
}

void load_executable_dialog::on_browse_app_home() {
    const auto dir = QFileDialog::getExistingDirectory(this,
        tr("Select file-serving directory"), app_home_edit_->text());
    if (!dir.isEmpty()) app_home_edit_->setText(dir);
}

void load_executable_dialog::on_browse_home() {
    const auto dir = QFileDialog::getExistingDirectory(this, tr("Select home directory"), home_edit_->text());
    if (!dir.isEmpty()) home_edit_->setText(dir);
}

load_executable_dialog::result load_executable_dialog::selection() const {
    result r;
    r.reset_target        = reset_check_->isChecked();
    r.clear_streams       = clear_streams_check_->isChecked();
    r.enable_debug_module = enable_debug_check_->isChecked();
    r.disable_ppu_debug   = disable_ppu_check_->isChecked();
    r.disable_spu_debug   = disable_spu_check_->isChecked();
    r.cmdline             = cmdline_edit_->text().trimmed();

    if (device_check_->isChecked()) {
        const auto prefix = device_combo_->currentText();
        auto tail = device_path_edit_->text().trimmed();
        while (tail.startsWith('/')) tail = tail.mid(1);
        r.path = prefix + tail;
        r.is_device_path = true;
    } else {
        r.path = file_path_edit_->text().trimmed();
        r.is_device_path = false;
    }

    if (app_home_check_->isChecked()) {
        if (app_home_use_elf_->isChecked() && !r.is_device_path && !r.path.isEmpty()) {
            r.file_serving_dir = QFileInfo(r.path).absolutePath();
        } else {
            r.file_serving_dir = app_home_edit_->text().trimmed();
        }
    }
    if (home_check_->isChecked()) {
        if (home_use_elf_->isChecked() && !r.is_device_path && !r.path.isEmpty()) {
            r.home_dir = QFileInfo(r.path).absolutePath();
        } else {
            r.home_dir = home_edit_->text().trimmed();
        }
    }
    return r;
}

} // namespace opentm::tm_ui
