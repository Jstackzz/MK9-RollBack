#pragma once
// F3 reports (docs/PROTOCOL.md): a player who sees something drawn wrong presses F3 in the game, which saves what is on
// the screen and the next 20 frames' pictures under the session's captures folder (mk9/src/mk9/report.h). This finds a
// new one in a running game's session, makes it small (pictures as JPEG, the end of the game's log compressed) and sends
// it to the server, which keeps it for the developers, a little at a time so the match's own messages pass.

#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QSet>
#include <QTimer>

#include <functional>

class GameProcess;
class ServerClient;

class ReportSender : public QObject {
    Q_OBJECT
public:
    // games: the games running now (their sessions are looked at every two seconds)
    ReportSender(ServerClient* server, std::function<QList<GameProcess*>()> games, QObject* parent = nullptr);

signals:
    void logLine(const QString& text);

private:
    struct Capture {
        QString gameId, sessionDir, dir;
        qint64 firstSeen = 0;
    };
    void poll();
    bool complete(const Capture& c) const;
    void build(const Capture& c);
    void sendSome();
    void onMessage(const QJsonObject& msg);
    void finish(const QString& text);

    ServerClient* server_;
    std::function<QList<GameProcess*>()> games_;
    QTimer pollTimer_, sendTimer_;
    QSet<QString> seen_;         // capture folders found (sent, being sent or given up on)
    QList<Capture> waiting_;     // found, their next frames' pictures still being saved
    QList<QJsonObject> outbox_;  // the report being sent: its parts, then report_done
    QString sending_;            // its id
    QString sendingDir_;         // its folder, deleted once the server has it
    int files_ = 0;
    qint64 bytes_ = 0;
};
