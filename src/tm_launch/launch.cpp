#include "launch.h"

#include "single_instance.h"

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>
#include <QThread>

namespace opentm::tm_launch {

namespace {

QByteArray verb(const char* name) {
    return QJsonDocument(QJsonObject{{"method", QLatin1String(name)}}).toJson(QJsonDocument::Compact);
}

} // namespace

QString sibling_executable(const QString& base_name) {
#ifdef Q_OS_WIN
    const QString exe = base_name + QStringLiteral(".exe");
#else
    const QString exe = base_name;
#endif
    QStringList dirs{QCoreApplication::applicationDirPath()};
#ifdef Q_OS_MACOS
    dirs << QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(QStringLiteral("../../.."));
#endif
    for (const auto& d : dirs) {
        const QFileInfo fi(QDir(d).absoluteFilePath(exe));
        if (fi.isExecutable() && fi.isFile()) return fi.absoluteFilePath();
    }
    return QStandardPaths::findExecutable(base_name);
}

bool ensure_supervisor(QString* err, int wait_ms) {
    if (!single_instance::request(QLatin1String(supervisor_socket), verb("status"), 800).isEmpty()) {
        return true;   // already up
    }

    const auto exe = sibling_executable(QStringLiteral("opentm_tray"));
    if (exe.isEmpty()) {
        if (err) {
            *err = QStringLiteral("opentm_tray is not in %1 (running %2). All three " "binaries must live in the same directory.").arg(QCoreApplication::applicationDirPath(), QCoreApplication::applicationFilePath());
        }
        return false;
    }

    if (!QProcess::startDetached(exe, {QStringLiteral("--no-gui")})) {
        if (err) *err = QStringLiteral("could not start %1").arg(exe);
        return false;
    }

    QDeadlineTimer deadline(wait_ms);
    while (!deadline.hasExpired()) {
        if (!single_instance::request(QLatin1String(supervisor_socket), verb("status"), 400).isEmpty()) {
            return true;
        }
        QThread::msleep(100);
    }
    if (err) {
        *err = QStringLiteral("%1 started but did not answer within %2ms").arg(exe).arg(wait_ms);
    }
    return false;
}

quint16 ensure_server(QString* err) {
    const auto reply = single_instance::request(
        QLatin1String(supervisor_socket), verb("ensure_server"), 15000);
    if (reply.isEmpty()) {
        if (err) *err = QStringLiteral("supervisor did not answer");
        return 0;
    }
    const auto obj = QJsonDocument::fromJson(reply).object();
    if (!obj.value("ok").toBool()) {
        if (err) {
            *err = obj.value("error").toString(QStringLiteral("supervisor could not start a server"));
        }
        return 0;
    }
    return static_cast<quint16>(obj.value("port").toInt());
}

quint16 restart_server(QString* err) {
    const auto stopped = single_instance::request(
        QLatin1String(supervisor_socket), verb("stop_server"), 15000);
    if (stopped.isEmpty()) {
        if (err) *err = QStringLiteral("supervisor did not answer");
        return 0;
    }
    return ensure_server(err);
}

} // namespace opentm::tm_launch
