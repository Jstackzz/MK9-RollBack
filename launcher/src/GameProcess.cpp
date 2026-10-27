#include "GameProcess.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcessEnvironment>

#include <windows.h>

#include "AppPaths.h"
#include "Controls.h"
#include "WinUtil.h"

namespace {
QString native(const QString& path) { return QDir::toNativeSeparators(path); }
}  // namespace

GameProcess::GameProcess(const QString& gameId, int instance, QObject* parent)
    : QObject(parent), gameId_(gameId), instance_(instance) {
    poll_.setInterval(100);
    connect(&poll_, &QTimer::timeout, this, &GameProcess::poll);
}

GameProcess::~GameProcess() {
    if (isRunning()) {
        proc_->kill();
        proc_->waitForFinished(2000);
    }
}

bool GameProcess::isRunning() const { return proc_ && proc_->state() != QProcess::NotRunning; }

qint64 GameProcess::pid() const { return isRunning() ? proc_->processId() : 0; }

QString GameProcess::state() const { return status_.section(QLatin1Char(' '), 0, 0); }

QHash<QString, QString> GameProcess::parseKeys(const QString& line) {
    QHash<QString, QString> out;
    const QStringList tokens = line.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    for (int i = 1; i < tokens.size(); ++i) {
        const int eq = tokens[i].indexOf(QLatin1Char('='));
        if (eq > 0) out.insert(tokens[i].left(eq), tokens[i].mid(eq + 1));
    }
    return out;
}

bool GameProcess::hasGameFiles(const QString& dataRoot) {
    return !dataRoot.isEmpty() && QFileInfo::exists(QDir(dataRoot).filePath(QStringLiteral("default.xex")));
}

QString GameProcess::smartAppControlHelp(bool blocked) {
    return (blocked ? tr("Windows' Smart App Control blocked the game.")
                    : tr("Windows' Smart App Control is on, and it will block the game.")) +
           QLatin1Char(' ') +
           tr("It only runs programs signed by a publisher it knows, and this game is built on each PC from its own "
              "disc, so it can never be. To play, switch it off in Windows Security > App & browser control > Smart "
              "App Control settings. (On some versions of Windows 11 it can only be switched back on by resetting "
              "Windows.)");
}

bool GameProcess::start(const QString& exe, QString* error, bool netMode) {
    if (isRunning()) return true;
    const QFileInfo fi(exe);
    if (exe.isEmpty() || !fi.exists()) {
        if (error) *error = tr("Game executable not found: %1. Build the game first (Build game) or set its path in "
                               "Settings.").arg(native(exe));
        return false;
    }
    const bool standIn = fi.suffix().compare(QLatin1String("py"), Qt::CaseInsensitive) == 0;
    const QString dataRoot = cfg::gameDataRoot(gameId_);
    if (!standIn && !hasGameFiles(dataRoot)) {
        if (error) *error = tr("The game's files are not there yet (no default.xex in %1): open Build game and "
                               "extract your disc first.").arg(native(dataRoot));
        return false;
    }
    // the newest sessions are kept (an online match's netplay log is about a megabyte, a fresh save a few)
    constexpr int kKeepSessions = 30;
    const QFileInfoList old = QDir(paths::sessionsDir()).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Time);
    for (int i = kKeepSessions; i < old.size(); i++) QDir(old[i].absoluteFilePath()).removeRecursively();
    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-hhmmss"));
    sessionDir_ = QDir(paths::sessionsDir()).filePath(QStringLiteral("%1-%2-%3").arg(gameId_, stamp).arg(instance_));
    for (int n = 2; QFileInfo::exists(sessionDir_); n++)  // a game started again within the same second
        sessionDir_ = QDir(paths::sessionsDir()).filePath(QStringLiteral("%1-%2-%3-%4").arg(gameId_, stamp).arg(instance_).arg(n));
    QDir().mkpath(sessionDir_);
    cmdPath_ = QDir(sessionDir_).filePath(QStringLiteral("cmd.txt"));
    statusPath_ = QDir(sessionDir_).filePath(QStringLiteral("status.txt"));
    QFile::remove(cmdPath_);
    QFile::remove(statusPath_);
    // online: a fresh save for every game the launcher drives (the autopilot steers by a fresh save's menus); single
    // player keeps the player's saves
    saveDir_ = netMode ? QDir(sessionDir_).filePath(QStringLiteral("save")) : cfg::saveFolder(gameId_);
    QDir().mkpath(saveDir_);

    netMode_ = netMode;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    for (const char* name : {"MK9_LINK", "MK9_CMD", "MK9_STATUS"}) env.remove(QLatin1String(name));
    if (netMode) {
        env.insert(QStringLiteral("MK9_LINK"), QStringLiteral("1"));
        env.insert(QStringLiteral("MK9_CMD"), native(cmdPath_));
        env.insert(QStringLiteral("MK9_STATUS"), native(statusPath_));
        // every online match's frames (both players' inputs, the state checks) beside the session's other files: what
        // to look at when a match went wrong ({match} and {slot} are the game's to fill in); a test that names its own
        // log keeps it
        if (!env.contains(QStringLiteral("MK9_NET_LOG")))
            env.insert(QStringLiteral("MK9_NET_LOG"), native(QDir(sessionDir_).filePath(QStringLiteral("net-{match}.log"))));
    }
    env.insert(QStringLiteral("MK9_SESSION_DIR"), native(sessionDir_));
    env.insert(QStringLiteral("MK9_INSTANCE"), QString::number(instance_));
    env.insert(QStringLiteral("MK9_CONTROLS"), native(controls::file()));  // read again when it changes
    for (auto it = extraEnv_.cbegin(); it != extraEnv_.cend(); ++it) env.insert(it.key(), it.value());

    // the game's paths and window as its command line takes them (the ReXGlue SDK's cvars)
    QStringList gameArgs = {
        QStringLiteral("--game_data_root=") + native(dataRoot),
        QStringLiteral("--user_data_root=") + native(saveDir_),
        QStringLiteral("--log_file=") + native(QDir(sessionDir_).filePath(QStringLiteral("game.log"))),
        windowed_ || !cfg::fullscreen() ? QStringLiteral("--no-fullscreen") : QStringLiteral("--fullscreen"),
    };
    // the shader cache outlives the fresh saves of online games (one per instance: two games must not share it)
    if (netMode) {
        const QString cache = QDir(paths::cacheDir()).filePath(QStringLiteral("%1-online-%2").arg(gameId_).arg(instance_));
        QDir().mkpath(cache);
        gameArgs << QStringLiteral("--cache_root=") + native(cache);
    }

    proc_ = new QProcess(this);
    proc_->setProcessEnvironment(env);
    proc_->setWorkingDirectory(fi.absolutePath());
    proc_->setProcessChannelMode(QProcess::MergedChannels);
    QString program = fi.absoluteFilePath();
    QStringList args = gameArgs;
    if (standIn) {
        program = cfg::python();
        args = QStringList{QStringLiteral("-u"), fi.absoluteFilePath()} + gameArgs;
        proc_->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* a) {
            a->flags |= CREATE_NO_WINDOW;  // the development stand-in has no window of its own
        });
    }
    connect(proc_, &QProcess::readyRead, this, [this] {
        outBuf_.append(proc_->readAll());
        qsizetype nl;
        while ((nl = outBuf_.indexOf('\n')) >= 0) {
            const QString line = QString::fromLocal8Bit(outBuf_.left(nl)).trimmed();
            outBuf_.remove(0, nl + 1);
            if (!line.isEmpty()) emit outputLine(line);
        }
    });
    connect(proc_, &QProcess::finished, this, [this](int code, QProcess::ExitStatus st) {
        poll();
        poll_.stop();
        applog::write(QStringLiteral("game %1/%2: exited with code %3%4")
                          .arg(gameId_).arg(instance_).arg(code).arg(st == QProcess::CrashExit ? " (crash)" : ""));
        const QString last = quitting_ ? QStringLiteral("exited") : QStringLiteral("error exited");
        status_ = last;
        emit statusChanged(last);
        emit exited(code, st == QProcess::CrashExit);
    });
    proc_->start(program, args);
    if (!proc_->waitForStarted(10000)) {
        if (error) {
            *error = tr("Could not start %1: %2").arg(native(program), proc_->errorString());
            // "An Application Control policy has blocked this file" (English Windows) or the policy reported on
            if (winutil::smartAppControl() == 1 || proc_->errorString().contains(QLatin1String("Application Control")))
                *error += QLatin1Char('\n') + smartAppControlHelp();
        }
        // one arg() call: Windows' messages can hold a %1 of their own, which a second call would fill
        applog::write(QStringLiteral("game %1/%2: could not start %3: %4 (Smart App Control %5)")
                          .arg(gameId_, QString::number(instance_), native(program), proc_->errorString(),
                               QString::number(winutil::smartAppControl())));
        delete proc_;
        proc_ = nullptr;
        return false;
    }
    winutil::gameJob().assign(proc_->processId());
    applog::write(QStringLiteral("game %1/%2: started %3 (pid %4, %5), session %6, saves %7")
                      .arg(gameId_, QString::number(instance_), native(fi.absoluteFilePath()),
                           QString::number(proc_->processId()), netMode ? QStringLiteral("launcher link") : QStringLiteral("plain"),
                           native(sessionDir_), native(saveDir_)));
    status_ = QStringLiteral("starting");
    emit statusChanged(status_);
    if (netMode) poll_.start();
    return true;
}

void GameProcess::poll() {
    bool exists = false;
    const QByteArray raw = winutil::readShared(statusPath_, &exists);
    if (exists && !raw.isEmpty()) {
        QString first = QString::fromLatin1(raw).section(QLatin1Char('\n'), 0, 0).trimmed();
        if (!first.isEmpty() && first != status_) {
            status_ = first;
            applog::write(QStringLiteral("game %1/%2: status %3").arg(gameId_).arg(instance_).arg(first));
            emit statusChanged(first);
        }
    }
    if (!pendingCmd_.isEmpty() && (!QFileInfo::exists(cmdPath_) || lastWrite_.elapsed() > 2000)) {
        const QString line = pendingCmd_;
        pendingCmd_.clear();
        writeCommand(line);
    }
}

void GameProcess::command(const QString& line) {
    if (cmdPath_.isEmpty()) return;
    if (QFileInfo::exists(cmdPath_) && lastWrite_.isValid() && lastWrite_.elapsed() < 2000) {
        pendingCmd_ = line;  // previous command not taken yet
        return;
    }
    writeCommand(line);
}

bool GameProcess::commandWaiting() const {
    return !pendingCmd_.isEmpty() || (!cmdPath_.isEmpty() && QFileInfo::exists(cmdPath_));
}

void GameProcess::writeCommand(const QString& line) {
    QString err;
    if (!winutil::writeFileAtomic(cmdPath_, line.toLatin1() + '\n', &err)) {
        applog::write(QStringLiteral("game %1/%2: %3").arg(gameId_).arg(instance_).arg(err));
        return;
    }
    lastWrite_.start();
    applog::write(QStringLiteral("game %1/%2: cmd %3").arg(gameId_).arg(instance_).arg(line));
}

void GameProcess::quit() {
    if (!isRunning() || quitting_) return;
    quitting_ = true;
    pendingCmd_.clear();
    if (netMode_) writeCommand(QStringLiteral("quit"));
    else proc_->terminate();  // WM_CLOSE to its windows: the game closes as when the player closes it
    QTimer::singleShot(5000, this, [this] {
        if (isRunning()) kill();
    });
}

void GameProcess::kill() {
    if (isRunning()) {
        quitting_ = true;
        proc_->kill();
    }
}
