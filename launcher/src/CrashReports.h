#pragma once
// Crash reports (docs/LAUNCHER.md, "Crash reports"). A game the launcher started that ends by itself with an error
// (an exit code other than 0: a crash, or a failure at start) gets a report in crashes\<time>-<game>\ beside the
// launcher's log: report.txt (what the game wrote about its crash, mk9/src/mk9/crash.h, the end of its log and of the
// launcher's, what the launcher was doing, this PC's Windows, processor, memory and graphics card), the game's
// minidump (crash.dmp) and its whole log. The newest 20 are kept. The player is shown it and may send report.txt to
// the MK9 Online server (crash_report, docs/PROTOCOL.md), with their user folder's path taken out first.

#include <QObject>
#include <QString>
#include <QStringList>

class ServerClient;

class CrashReports : public QObject {
    Q_OBJECT
public:
    static CrashReports& instance();
    static QString folder();  // crashes\ beside the launcher's log

    // what the launcher is doing, in its own words (the match controller's phase texts): the last few go in a report
    void setActivity(const QString& text);

    // a game that ended by itself with `exitCode` after `ranMs`: its report is written, and written() says where
    void gameEnded(const QString& gameId, const QString& sessionDir, int exitCode, qint64 ranMs);

    // a report's text as it goes to the server: the user folder's path taken out
    static QByteArray forSending(const QString& reportDir);
    // sends it (crash_report messages, zlib and base64 in parts); false when the server is not connected
    static bool send(ServerClient* server, const QString& reportDir);
    // the exit code in words ("an access violation"), empty when it is not one of the known ones
    static QString exitText(int exitCode);

signals:
    // `summary`: a line on what happened, for the player
    void written(const QString& reportDir, const QString& summary);

private:
    QStringList activity_;
};
