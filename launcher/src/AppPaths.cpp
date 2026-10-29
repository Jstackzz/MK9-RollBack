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
    // Launchers before 0.3.1 came without a server: the built-in one was this PC's (127.0.0.1:7777), saved once
    // Settings were, and a certificate trusted for it was saved alone (the address being the default). The package's
    // server takes its place once, with its own fingerprint (someone who wants this PC's again sets it). 0.3.1 kept a
    // fingerprint saved alone, and package_server_taken marks those settings: they are gone over once more.
    const QString moved = QStringLiteral("server/package_fingerprint_taken");
    if (g_defaults && g_defaults->contains(QStringLiteral("server/address")) && !g_settings->value(moved).toBool()) {
        const QString address = g_settings->value(QStringLiteral("server/address")).toString();
        if (address.isEmpty() || address == QLatin1String("127.0.0.1:7777")) {
            g_settings->remove(QStringLiteral("server/address"));
            g_settings->remove(QStringLiteral("server/fingerprint"));
        }
        g_settings->remove(QStringLiteral("server/package_server_taken"));
        g_settings->setValue(moved, true);
        g_settings->sync();
    }
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
void remove(const QString& key) {
    g_settings->remove(key);
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
    const Game* g = GameData::instance().game(gameId);
    const QString exe = g && !g->defaultExe.isEmpty() ? g->defaultExe : gameId + QStringLiteral(".exe");
    // a package keeps the executable in the game folder, a source checkout in its build output
    const QDir folder(gameFolder(gameId));
    const QString built = QFileInfo::exists(folder.filePath(exe))
                              ? folder.filePath(exe)
                              : folder.filePath(QStringLiteral("out/build/win-amd64-release/") + exe);
    // the one set in Settings, unless it is gone and the built game is there (the folder moved, or the path was
    // saved before the game was built)
    const QString v = get(QStringLiteral("games/%1/exe").arg(gameId)).toString();
    if (!v.isEmpty() && (QFileInfo::exists(v) || !QFileInfo::exists(built))) return v;
    return built;
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
    return get(QStringLiteral("online/netcode")).toString() == QLatin1String("lockstep") ? QStringLiteral("lockstep")
                                                                                         : QStringLiteral("rollback");
}
int gamePort() { return get(QStringLiteral("online/game_port"), 7001).toInt(); }
bool fullscreen() { return get(QStringLiteral("display/fullscreen"), true).toBool(); }
bool vsync() { return get(QStringLiteral("display/vsync"), true).toBool(); }
QSize windowSize() {
    const QStringList wh = get(QStringLiteral("display/window_size")).toString().split(QLatin1Char('x'));
    const QSize size = wh.size() == 2 ? QSize(wh[0].toInt(), wh[1].toInt()) : QSize();
    return size.width() >= 320 && size.height() >= 180 ? size : QSize();
}
int renderScale() { return qBound(1, get(QStringLiteral("display/render_scale"), 1).toInt(), 3); }
bool udpLoopback() { return get(QStringLiteral("dev/udp_loopback"), false).toBool(); }
bool upnp() { return get(QStringLiteral("online/upnp"), true).toBool(); }
bool matchSound() { return get(QStringLiteral("online/match_sound"), true).toBool(); }
bool hideIp() { return get(QStringLiteral("online/hide_ip"), false).toBool(); }
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
