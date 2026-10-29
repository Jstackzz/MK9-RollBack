#pragma once
// Where the launcher keeps its settings, logs and per-session folders, where the game is, and typed settings access.

#include <QJsonValue>
#include <QSettings>
#include <QSize>
#include <QString>
#include <QVariant>

namespace paths {
void init(const QString& configOverride);  // call once, before anything else
QString configFile();                        // mk9-launcher.ini
QString dataDir();                           // folder of the ini: sessions\, cache\, mk9-launcher.log
QString sessionsDir();
QString cacheDir();                          // the online games' shader caches (cache\<game>-online-<instance>)
QString logFile();
QString detectedRepoRoot();                  // walks up from the exe looking for mk9\mk9_manifest.toml
bool isPackage();                            // a player package: games\ beside the launcher
QString packagedGameFolder(const QString& gameId);  // games\<id> beside the launcher (a player package), or empty
}  // namespace paths

namespace cfg {
QSettings& store();
QVariant get(const QString& key, const QVariant& fallback = {});
void set(const QString& key, const QVariant& value);
void remove(const QString& key);  // back to the default

QString serverAddress();
QString fingerprint();
QString repoRoot();
QString python();
// the game's folders and files; each is a setting (games/<id>/...) whose default depends on where the launcher runs:
//   a player package: games\mk9\mk9.exe, games\mk9\game (the disc), games\mk9\user (saves), games\mk9\build-game.cmd
//   a source build:   <repo>\mk9\out\build\win-amd64-release\mk9.exe, <repo>\game\disc, <repo>\game\user,
//                     <repo>\mk9\build.cmd release
QString gameFolder(const QString& gameId);
QString gameExe(const QString& gameId);
QString gameDataRoot(const QString& gameId);  // the extracted disc: default.xex and the Asset folder
QString saveFolder(const QString& gameId);    // single player's saves (an online session has a fresh one)
QString buildScript(const QString& gameId);   // what Build game runs to compile the game
QString buildArgs(const QString& gameId);
QString region();
int maxPing();
bool strict();
// the player picks their fighter on the game's own select screen in online matches, not in the launcher
bool pickInGame();
// a Ranked or Kasual search starts single player to play while it waits (MatchController::maybeStartSolo)
bool soloWhileWaiting();
// the input delay in frames, 0-6, or kAutoDelay (the default): from the match's measured ping, the server's
// match_go delay (autoDelay); as the protocol writes it: "auto" or the number
constexpr int kAutoDelay = -1;
int inputDelay();
QJsonValue inputDelayValue(int delay);
QString inputDelayText(int delay);
// what Auto comes to for a round trip: clamp(ceil((rtt / 2 + 8) / 16.67), 2, 8) frames, the one-way latency and a frame
// on its way (the server's protocol.auto_delay)
int autoDelay(int rttMs);
// what the games keep a match in step with: "rollback" (the default) or "lockstep"; a match plays rollback only when
// both players chose it (the server decides, match_go's netcode)
QString netcode();
int gamePort();
bool fullscreen();  // the game's window: fullscreen (the game's own default) or a window
// V-sync (the default): no frame of the game ever tears, each refresh shows its newest frame whole. Off: each frame shows
// the moment it is ready, which a G-SYNC or FreeSync monitor follows and any other may tear across
bool vsync();
QSize windowSize();  // the game's window when it opens in one, in pixels ({}: the game's own, 1280x720)
int renderScale();   // what the game draws at: 1280x720 times this (1, the Xbox 360's, to 3)
int localTestPort(int instance);
bool udpLoopback();  // development: matches on loopback only (one-PC tests, no firewall prompt)
bool upnp();         // open the game UDP port on the router (UPnP)
bool matchSound();   // a chime when a Ranked or Kasual match is found (the MATCH FOUND window comes up)
bool hideIp();       // matches through the server: no opponent learns this player's address ("relay" to the server)
}  // namespace cfg

namespace applog {
void write(const QString& line);  // timestamped line in mk9-launcher.log
}
