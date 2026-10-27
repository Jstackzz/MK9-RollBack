#pragma once
// One game process and its session folder: environment, command line, cmd.txt (launcher -> game) and status.txt
// (game -> launcher), docs/LAUNCHER.md "The game contract" (docs/mk3d-reference/PROTOCOL.md section 3 for the file
// rules).

#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QProcess>
#include <QString>
#include <QTimer>

class GameProcess : public QObject {
    Q_OBJECT
public:
    GameProcess(const QString& gameId, int instance, QObject* parent = nullptr);
    ~GameProcess() override;

    // Starts `exe` (a .py file is run with the configured Python, for the development stand-in). `netMode` true: the
    // launcher owns the game (MK9_LINK=1, cmd.txt / status.txt), which boots to its hold point with a fresh save
    // folder of its own in the session (the autopilot counts on a fresh save's menus); false: started plainly with
    // the persistent save folder (single player), the game runs its own menus.
    bool start(const QString& exe, QString* error, bool netMode = true);
    // one more environment variable for the next start
    void setEnv(const QString& name, const QString& value) { extraEnv_.insert(name, value); }
    void setWindowed(bool on) { windowed_ = on; }  // a window whatever Settings say (the two of a local test)
    // "quit" command (a plain game: its window is closed, as the player would), then kill after 5 s
    void quit();
    void kill();
    bool isRunning() const;
    qint64 pid() const;  // 0 when not running

    QString gameId() const { return gameId_; }
    int instance() const { return instance_; }
    QString sessionDir() const { return sessionDir_; }
    QString saveDir() const { return saveDir_; }
    QString status() const { return status_; }  // first line of status.txt
    QString state() const;                     // first word of it
    bool isReady() const { return state() == QLatin1String("ready"); }

    // Write one command line; if the previous command has not been taken yet it waits up to 2 s
    // (a newer command replaces a waiting one).
    void command(const QString& line);
    bool commandWaiting() const;  // a command the game has not taken yet (cmd.txt is still there)

    static QHash<QString, QString> parseKeys(const QString& line);  // key=value tokens after the verb
    // the game's files where the game looks for them: default.xex in the game data folder
    static bool hasGameFiles(const QString& dataRoot);

    // what to tell a player whose Windows blocks the game (`blocked`: it just did): Smart App Control and how to
    // switch it off
    static QString smartAppControlHelp(bool blocked = true);

signals:
    void statusChanged(const QString& line);
    void outputLine(const QString& line);
    void exited(int exitCode, bool crashed);

private:
    void poll();
    void writeCommand(const QString& line);

    QString gameId_;
    int instance_;
    QProcess* proc_ = nullptr;
    QTimer poll_;
    QString sessionDir_, cmdPath_, statusPath_, saveDir_;
    QString status_;
    QString pendingCmd_;
    QElapsedTimer lastWrite_;
    bool netMode_ = true;
    bool windowed_ = false;
    bool quitting_ = false;
    QByteArray outBuf_;
    QHash<QString, QString> extraEnv_;
};
