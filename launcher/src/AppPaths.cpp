#include "AppPaths.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QStandardPaths>

#include <cmath>

#include "GameData.h"

namespace {
QString g_config;
QString g_dataDir;
QSettings* g_settings = nullptr;
QSettings* g_defaults = nullptr;  // defaults.ini beside the launcher: what a package sets up (the server), read only
QMutex g_logMutex;

bool writableDir(const QString& dir) {
    QDir().mkpath(dir);
    QFile probe(QDir(dir).filePath(QStringLiteral(".write-test")));
    if (!probe.open(QIODevice::WriteOnly)) return false;
    probe.close();
    probe.remove();
    return true;
}

QString exeDir() { return QCoreApplication::applicationDirPath(); }

// a setting, or what it is by default where the launcher runs: in a player package, in a source checkout
QString pathSetting(const QString& key, const QString& packaged, const QString& source) {
    const QString v = cfg::get(key).toString();
    if (!v.isEmpty()) return v;
    return paths::isPackage() ? packaged : source;
}
}  // namespace

namespace paths {

void init(const QString& configOverride) {
    if (!configOverride.isEmpty()) {
        g_config = QFileInfo(configOverride).absoluteFilePath();
    } else {
        // beside the exe, or %LOCALAPPDATA%\MK9\MK9 Online when that folder is read-only
        const QString dir = writableDir(exeDir()) ? exeDir()
                                                  : QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
        g_config = QDir(dir).filePath(QStringLiteral("mk9-launcher.ini"));
    }
    g_dataDir = QFileInfo(g_config).absolutePath();
    QDir().mkpath(g_dataDir);
    QDir().mkpath(sessionsDir());
    g_settings = new QSettings(g_config, QSettings::IniFormat);
    const QString defaults = QDir(exeDir()).filePath(QStringLiteral("defaults.ini"));
    if (QFileInfo::exists(defaults)) g_defaults = new QSettings(defaults, QSettings::IniFormat);
}

QString configFile() { return g_config; }
QString dataDir() { return g_dataDir; }
QString sessionsDir() { return QDir(g_dataDir).filePath(QStringLiteral("sessions")); }
QString cacheDir() { return QDir(g_dataDir).filePath(QStringLiteral("cache")); }
QString logFile() { return QDir(g_dataDir).filePath(QStringLiteral("mk9-launcher.log")); }

QString detectedRepoRoot() {
    QDir dir(exeDir());
    for (int i = 0; i < 8; ++i) {
        if (QFileInfo::exists(dir.filePath(QStringLiteral("mk9/mk9_manifest.toml"))) &&
            QFileInfo::exists(dir.filePath(QStringLiteral("launcher/CMakeLists.txt"))))
            return dir.absolutePath();
        if (!dir.cdUp()) break;
    }
    return exeDir();
}

bool isPackage() { return QFileInfo(QDir(exeDir()).filePath(QStringLiteral("games"))).isDir(); }

QString packagedGameFolder(const QString& gameId) {
    const QString dir = QDir(exeDir()).filePath(QStringLiteral("games/") + gameId);
    return QFileInfo(dir).isDir() ? dir : QString();
}

}  // namespace paths

namespace cfg {

QSettings& store() { return *g_settings; }
QVariant get(const QString& key, const QVariant& fallback) {
    return g_settings->value(key, g_defaults ? g_defaults->value(key, fallback) : fallback);
}
void set(const QString& key, const QVariant& value) {
    g_settings->setValue(key, value);
    g_settings->sync();
}

QString serverAddress() { return get(QStringLiteral("server/address"), QStringLiteral("127.0.0.1:7777")).toString(); }
QString fingerprint() { return get(QStringLiteral("server/fingerprint")).toString(); }

QString repoRoot() {
    const QString v = get(QStringLiteral("tools/repo_root")).toString();
    return v.isEmpty() ? paths::detectedRepoRoot() : v;
}

QString python() { return get(QStringLiteral("tools/python"), QStringLiteral("python")).toString(); }

QString gameFolder(const QString& gameId) {
    const QString v = get(QStringLiteral("games/%1/folder").arg(gameId)).toString();
    if (!v.isEmpty()) return v;
    const QString packaged = paths::packagedGameFolder(gameId);
    return packaged.isEmpty() ? QDir(repoRoot()).filePath(gameId) : packaged;
}

QString gameExe(const QString& gameId) {
    const QString v = get(QStringLiteral("games/%1/exe").arg(gameId)).toString();
    if (!v.isEmpty()) return v;
    const Game* g = GameData::instance().game(gameId);
    const QString exe = g && !g->defaultExe.isEmpty() ? g->defaultExe : gameId + QStringLiteral(".exe");
    // a package keeps the executable in the game folder, a source checkout in its build output
    const QDir folder(gameFolder(gameId));
    if (QFileInfo::exists(folder.filePath(exe))) return folder.filePath(exe);
    return folder.filePath(QStringLiteral("out/build/win-amd64-release/") + exe);
}

QString gameDataRoot(const QString& gameId) {
    return pathSetting(QStringLiteral("games/%1/data").arg(gameId), QDir(gameFolder(gameId)).filePath(QStringLiteral("game")),
                       QDir(repoRoot()).filePath(QStringLiteral("game/disc")));
}

QString saveFolder(const QString& gameId) {
    return pathSetting(QStringLiteral("games/%1/save").arg(gameId), QDir(gameFolder(gameId)).filePath(QStringLiteral("user")),
                       QDir(repoRoot()).filePath(QStringLiteral("game/user")));
}

QString buildScript(const QString& gameId) {
    return pathSetting(QStringLiteral("games/%1/build_script").arg(gameId),
                       QDir(gameFolder(gameId)).filePath(QStringLiteral("build-game.cmd")),
                       QDir(gameFolder(gameId)).filePath(QStringLiteral("build.cmd")));
}

QString buildArgs(const QString& gameId) {
    const QVariant v = get(QStringLiteral("games/%1/build_args").arg(gameId));
    if (v.isValid()) return v.toString();
    return paths::isPackage() ? QString() : QStringLiteral("release");
}

QString region() { return get(QStringLiteral("online/region"), QStringLiteral("eu")).toString(); }
int maxPing() { return get(QStringLiteral("online/max_ping"), 150).toInt(); }
bool strict() { return get(QStringLiteral("online/strict"), false).toBool(); }
bool pickInGame() { return get(QStringLiteral("online/pick_in_game"), true).toBool(); }
bool soloWhileWaiting() { return get(QStringLiteral("online/solo_while_waiting"), false).toBool(); }
int inputDelay() {
    const QVariant v = get(QStringLiteral("online/delay"), QStringLiteral("auto"));
    bool number = false;
    const int d = v.toInt(&number);
    return number ? qBound(0, d, 6) : kAutoDelay;
}
QJsonValue inputDelayValue(int delay) { return delay < 0 ? QJsonValue(QStringLiteral("auto")) : QJsonValue(delay); }
QString inputDelayText(int delay) {
    return delay < 0 ? QStringLiteral("auto (from the ping)") : QStringLiteral("%1 frame(s)").arg(delay);
}
int autoDelay(int rttMs) { return qBound(2, int(std::ceil((rttMs / 2.0 + 8.0) / 16.67)), 8); }
QString netcode() {
    return get(QStringLiteral("online/netcode")).toString() == QLatin1String("rollback") ? QStringLiteral("rollback")
                                                                                         : QStringLiteral("lockstep");
}
int gamePort() { return get(QStringLiteral("online/game_port"), 7001).toInt(); }
bool fullscreen() { return get(QStringLiteral("display/fullscreen"), true).toBool(); }
bool udpLoopback() { return get(QStringLiteral("dev/udp_loopback"), false).toBool(); }
bool upnp() { return get(QStringLiteral("online/upnp"), true).toBool(); }
int localTestPort(int instance) {
    return get(QStringLiteral("dev/local_port%1").arg(instance), instance == 1 ? 7001 : 7002).toInt();
}

}  // namespace cfg

namespace applog {

void write(const QString& line) {
    QMutexLocker lock(&g_logMutex);
    QFile f(paths::logFile());
    if (!f.open(QIODevice::Append | QIODevice::Text)) return;
    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd hh:mm:ss.zzz"));
    f.write((stamp + QLatin1Char(' ') + line + QLatin1Char('\n')).toUtf8());
}

}  // namespace applog
