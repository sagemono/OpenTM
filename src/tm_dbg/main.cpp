#include "debugger_window.h"

#include <tm_core/target_type.h>
#include <tm_launch/launch.h>
#include <tm_session/session_factory.h>
#include <tm_session/target_record.h>

#include <QApplication>
#include <QCommandLineParser>
#include <QTextStream>

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("OpenTM");
    QCoreApplication::setApplicationName("OpenTM Debugger");

    QTextStream out(stdout);

    QCommandLineParser p;
    p.setApplicationDescription(QStringLiteral(
        "PPU debugger for PS3 devkits and DEX consoles.\n\n"
        "By default this attaches to a session the OpenTM session server already "
        "owns, so the target manager and the debugger can hold the same console "
        "at once. A console only accepts one registration per connection, so "
        "--standalone will be refused a session while the target manager has one."));
    p.addHelpOption();

    const QCommandLineOption o_target(QStringLiteral("target"),
        QStringLiteral("Console to attach to."), QStringLiteral("host"));
    const QCommandLineOption o_port(QStringLiteral("port"),
        QStringLiteral("Console port. Defaults to the type's usual port."), QStringLiteral("n"));
    const QCommandLineOption o_type(QStringLiteral("type"),
        QStringLiteral("Target type: decr_tcp or cfw_dex."), QStringLiteral("name"),
        QStringLiteral("decr_tcp"));
    const QCommandLineOption o_standalone(QStringLiteral("standalone"),
        QStringLiteral("Own the session in this process instead of attaching through the server."));
    const QCommandLineOption o_server(QStringLiteral("server"),
        QStringLiteral("Session server to attach through."), QStringLiteral("host:port"),
        QStringLiteral("127.0.0.1:4300"));
    const QCommandLineOption o_server_local(QStringLiteral("server-local"),
        QStringLiteral("Same, over a local socket / named pipe."), QStringLiteral("name"));
    const QCommandLineOption o_restart(QStringLiteral("restart-server"),
        QStringLiteral("Restart the session server before attaching, so a rebuilt one is picked up. Drops the sessions the old server held."));
    p.addOption(o_target);
    p.addOption(o_port);
    p.addOption(o_type);
    p.addOption(o_standalone);
    p.addOption(o_server);
    p.addOption(o_server_local);
    p.addOption(o_restart);
    p.process(app);

    opentm::tm_ui::session_backend backend;
    if (p.isSet(o_standalone)) {
        // left in_process on purpose
    } else if (p.isSet(o_server_local)) {
        backend.k = opentm::tm_ui::session_backend::kind::remote_local;
        backend.local_name = p.value(o_server_local);
    } else if (p.isSet(o_server)) {
        backend.k = opentm::tm_ui::session_backend::kind::remote_tcp;
        const auto value = p.value(o_server);
        const int colon = value.lastIndexOf(QLatin1Char(':'));
        if (colon > 0) {
            backend.host = value.left(colon);
            backend.port = static_cast<quint16>(value.mid(colon + 1).toUShort());
        } else {
            backend.host = value;
        }
    } else {
        // the usual case: join whatever the target manager is already using
        QString err;
        quint16 port = 0;
        if (opentm::tm_launch::ensure_supervisor(&err)) {
            // the supervisor keeps the old server alive across rebuilds
            port = p.isSet(o_restart) ? opentm::tm_launch::restart_server(&err) : opentm::tm_launch::ensure_server(&err);
        }
        if (port != 0) {
            backend.k    = opentm::tm_ui::session_backend::kind::remote_tcp;
            backend.host = QStringLiteral("127.0.0.1");
            backend.port = port;
        } else {
            backend.fallback_reason =
                QStringLiteral("no session server (%1); holding the session here, which the console will refuse if the target manager has it").arg(err);
            out << "note: " << backend.fallback_reason << Qt::endl;
        }
    }

    opentm::tm_ui::target_record record;
    if (p.isSet(o_target)) {
        record.host = p.value(o_target); //remember connected target
        record.name = record.host;
    }
    if (const auto t = opentm::tm_core::target_type_from_string(p.value(o_type).toStdString())) {
        record.type = *t;
    }
    const quint16 usual = (record.type == opentm::tm_core::target_type::cfw_dex) ? 1000 : 8530;
    record.port = p.isSet(o_port) ? static_cast<quint16>(p.value(o_port).toUShort()) : usual;

    opentm::tm_ui::debugger_window w(backend, record);
    w.show();
    return app.exec();
}
