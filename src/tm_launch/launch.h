#pragma once

#include <QByteArray>
#include <QString>

namespace opentm::tm_launch {

inline constexpr char supervisor_socket[] = "opentm-supervisor";
inline constexpr char gui_socket[]        = "opentm-gui";

QString sibling_executable(const QString& base_name);
bool ensure_supervisor(QString* err, int wait_ms = 6000);
quint16 ensure_server(QString* err);

quint16 restart_server(QString* err);

} // namespace opentm::tm_launch
