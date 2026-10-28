#include "MatchController.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QPointer>
#include <QRandomGenerator>
#include <QTimer>

#include "AppPaths.h"
#include "CrashReports.h"
#include "FeedLink.h"
#include "GameData.h"
#include "GameProcess.h"
#include "ServerClient.h"
#include "UdpRendezvous.h"
#include "WinUtil.h"

namespace {
QString str(const QJsonObject& o, const char* key) { return o.value(QLatin1String(key)).toString(); }

QString reasonText(const QString& reason) {
    static const QHash<QString, QString> texts = {
        {QStringLiteral("declined"), QStringLiteral("a player declined")},
        {QStringLiteral("accept_timeout"), QStringLiteral("not accepted in time")},
        {QStringLiteral("no_udp"), QStringLiteral("no UDP contact with the server, check the game port")},
        {QStringLiteral("punch_failed"), QStringLiteral("could not connect to the opponent")},
        {QStringLiteral("ping_too_high"), QStringLiteral("ping above the limit")},
        {QStringLiteral("ping_timeout"), QStringLiteral("connection test timed out")},
        {QStringLiteral("peer_left"), QStringLiteral("the opponent left")},
        {QStringLiteral("series_over"), QStringLiteral("the series is over")},
    };
    return texts.value(reason, reason);
}

QString pick(const QJsonObject& o, const char* key) {
    const QString v = str(o, key);
    return v.isEmpty() ? QStringLiteral("-") : v;
}

// what a match plays with: what the server decided for both (rollback only when both asked for it); a server that does
// not say gets lockstep, which every game has
QString netcodeOf(const QJsonObject& o) {
    return str(o, "netcode") == QLatin1String("rollback") ? QStringLiteral("rollback") : QStringLiteral("lockstep");
}

constexpr int kParkMs = 30000;          // a park from the game's CONTINUE? menu back to its main menu, at most
constexpr int kWatchFeedCheckMs = 10000;  // a spectator's game connects to its feed within this once it plays
}  // namespace

MatchController::MatchController(ServerClient* server, QObject* parent)
    : QObject(parent), server_(server), udp_(new UdpRendezvous(this)) {
    connect(server_, &ServerClient::message, this, &MatchController::onServerMessage);
    connect(server_, &ServerClient::loggedIn, this, [this] {
        if (!pendingResult_.isEmpty()) {
            server_->send(pendingResult_);
            log(tr("Sent the stored result of match %1").arg(str(pendingResult_, "match")));
            pendingResult_ = {};
        }
    });
    connect(server_, &ServerClient::stateChanged, this, [this](ServerClient::State s) {
        if (s != ServerClient::State::Offline) return;
        if (phase_ == Phase::Searching || phase_ == Phase::Found || phase_ == Phase::Connecting) {
            releaseUdp();
            found_ = {};
            setPhase(Phase::Idle, tr("Lost the server connection"));
        }
    });
    connect(udp_, &UdpRendezvous::punchDone, this, &MatchController::onPunchDone);
    connect(udp_, &UdpRendezvous::helloAcked, this, [this] {
        udpAcked_ = true;
        udpBlocked_ = false;
    });
    connect(udp_, &UdpRendezvous::helloTimedOut, this, [this] {
        udpBlocked_ = true;
        if ((phase_ == Phase::Found || phase_ == Phase::Connecting) && !tunnel_ && !server_->canTunnel())
            log(tr("The server did not answer on UDP; is UDP port %1 blocked by a firewall?").arg(cfg::gamePort()));
    });
    // a tunnel's punches and pings go out through the server connection, and what comes back from it goes on
    udp_->setTunnelSender([this](const QByteArray& datagram) { server_->sendTunnel({datagram}); });
    connect(server_, &ServerClient::tunneled, this, &MatchController::onTunneled);
    tunnelAsk_.setSingleShot(true);
    connect(&tunnelAsk_, &QTimer::timeout, this, &MatchController::askForTunnel);

    parkTimer_.setSingleShot(true);
    connect(&parkTimer_, &QTimer::timeout, this, [this] {
        if (phase_ == Phase::Reporting) finishMatch();
    });
    keepaliveTimer_.setInterval(15000);
    connect(&keepaliveTimer_, &QTimer::timeout, this, &MatchController::sendKeepaliveHello);
    // a park that did not bring the game back to its hold point: it is started again
    finishTimeout_.setSingleShot(true);
    connect(&finishTimeout_, &QTimer::timeout, this, [this] {
        if (phase_ != Phase::Finishing) return;
        closeFeed();
        closeWatchFeed();
        if (game_ && game_->isRunning() && !game_->isReady()) {
            restartGame(tr("it did not come back to its menu"));
            return;
        }
        finishDone();
    });
    // a watched match whose game never got into it (the players' feed never came): parked after this
    watchTimeout_.setSingleShot(true);
    connect(&watchTimeout_, &QTimer::timeout, this, [this] {
        if (phase_ == Phase::Watching) endWatch(tr("The watched match never came through"));
    });
    // a spectator's game that plays without reading its feed did not take the watch line: a game without spectating
    watchFeedCheck_.setSingleShot(true);
    connect(&watchFeedCheck_, &QTimer::timeout, this, [this] {
        if (phase_ == Phase::Watching && watchFeed_ && !watchFeed_->gameConnected())
            watchUnsupported(tr("the game never read the match's feed"));
    });
    // A start line the game took and still at its hold point: it turned it down (its "error ..." line may be gone by
    // the next poll, the game writes "ready" again at once)
    startCheck_.setSingleShot(true);
    connect(&startCheck_, &QTimer::timeout, this, [this] {
        if (!game_ || !game_->isRunning()) return;
        if (game_->commandWaiting()) {  // not taken yet (the game is busy or frozen): look again
            startCheck_.start(2000);
            return;
        }
        if (!game_->isReady()) return;  // it took it
        if (phase_ == Phase::Watching && !watchInMatch_) {
            watchUnsupported(tr("it turned the watch line down"));
        } else if (phase_ == Phase::Starting) {
            log(tr("The game turned the start line down (status %1).").arg(game_->status()));
            reportResult({{QStringLiteral("outcome"), QStringLiteral("error")}});
        }
    });
    continueTimer_.setSingleShot(true);
    connect(&continueTimer_, &QTimer::timeout, this, [this] {
        const QString state = str(series_, "state");
        if (atContinue_ && state != QLatin1String("between") && state != QLatin1String("playing")) parkFromContinue(true);
    });
    parkWatch_.setSingleShot(true);
    connect(&parkWatch_, &QTimer::timeout, this, [this] {
        if (!game_ || !game_->isRunning() || game_->isReady()) return;
        const QString st = game_->state();
        if (st != QLatin1String("in_match") && st != QLatin1String("result")) restartGame(tr("it did not come back to its menu"));
    });
    startTimeout_.setSingleShot(true);
    connect(&startTimeout_, &QTimer::timeout, this, [this] {
        if (phase_ == Phase::Starting) {
            log(tr("The game did not start the match within %1 s.").arg(startTimeout_.interval() / 1000));
            reportResult({{QStringLiteral("outcome"), QStringLiteral("error")}});
        }
    });
}

MatchController::~MatchController() = default;

void MatchController::log(const QString& text) {
    applog::write(QStringLiteral("match: ") + text);
    CrashReports::instance().setActivity(text);
    emit logLine(text);
}

void MatchController::setPhase(Phase p, const QString& text) {
    if (p == Phase::Idle && solo_ && !soloFrozen_) p = Phase::Solo;  // nothing online: the single-player game's
    phase_ = p;
    phaseText_ = text;
    if (p != Phase::Searching) keepaliveTimer_.stop();
    log(text);
    emit phaseChanged(p, text);
    if (p == Phase::Idle) sendSeriesReady();
    maybeBackToSolo();
    if (p == Phase::Searching) maybeStartSolo();
    if (p == Phase::Idle) startWatch();  // a lobby match waiting to be watched
}

void MatchController::freezeSolo() {
    if (!solo_ || soloFrozen_) return;
    soloHidden_.clear();
    soloFrozen_ = true;
    if (!winutil::freezeProcess(solo_->pid(), &soloHidden_)) log(tr("Standby: single player could not be frozen"));
    log(tr("Standby: single player set aside for the match"));
}

void MatchController::thawSolo() {
    if (!solo_ || !soloFrozen_) return;
    soloFrozen_ = false;
    winutil::thawProcess(solo_->pid(), soloHidden_);
    soloHidden_.clear();
}

void MatchController::freezeGame() {
    if (!game_ || gameFrozen_ || !game_->isRunning()) return;
    gameHidden_.clear();
    gameFrozen_ = true;
    winutil::freezeProcess(game_->pid(), &gameHidden_);
}

void MatchController::thawGame() {
    if (!game_ || !gameFrozen_) return;
    gameFrozen_ = false;
    winutil::thawProcess(game_->pid(), gameHidden_);
    gameHidden_.clear();
}

// Standby: once nothing more is to be played online (the match is over and no rematch or set's next match follows,
// or it was cancelled before it started), the match's game is frozen at its hold point for the next one and single
// player runs on where it stood.
void MatchController::maybeBackToSolo() {
    if (!soloFrozen_ || !solo_) return;
    if (phase_ != Phase::Idle && phase_ != Phase::Searching) return;
    const QString state = str(series_, "state");
    if (state == QLatin1String("between") || state == QLatin1String("playing")) return;
    thawSolo();
    log(tr("Standby: back to single player"));
    if (phase_ == Phase::Idle) setPhase(Phase::Solo, tr("Playing single player"));
    // in front while the match's game still runs: taking the foreground from a frozen window would wait on it for ever
    emit soloResumed();
    freezeGame();
}

void MatchController::seriesReady(bool ready, const QString& choice) {
    if (series_.isEmpty()) return;
    wantNext_ = ready;
    nextChoice_ = choice;
    if (!ready) {
        server_->send({{QStringLiteral("type"), QStringLiteral("series_ready")},
                       {QStringLiteral("series"), str(series_, "series")},
                       {QStringLiteral("ready"), false}});
        return;
    }
    sendSeriesReady();
}

// the next match of the series is asked for once this launcher is idle: the game is back at its hold point (or
// booting to it again) and has let go of the game port
void MatchController::sendSeriesReady() {
    if (!wantNext_ || phase_ != Phase::Idle || series_.isEmpty() || str(series_, "state") != QLatin1String("between") ||
        !server_->isLoggedIn())
        return;
    wantNext_ = false;
    QJsonObject msg{{QStringLiteral("type"), QStringLiteral("series_ready")},
                    {QStringLiteral("series"), str(series_, "series")},
                    {QStringLiteral("ready"), true},
                    {QStringLiteral("choice"), nextChoice_}};
    // the game still waits at its CONTINUE? menu with the other's: the next match may start there
    if (atContinue_ && game_ && game_->isRunning() && game_->state() == QLatin1String("result"))
        msg.insert(QStringLiteral("continue"), true);
    server_->send(msg);
}

int MatchController::searchSeconds() const {
    return searchClock_.isValid() ? static_cast<int>(searchClock_.elapsed() / 1000) : 0;
}

QString MatchController::gameStatus() const {
    if (localA_ || localB_)
        return tr("local test: 1 %1 / 2 %2").arg(localA_ ? localA_->status() : QStringLiteral("-"),
                                                 localB_ ? localB_->status() : QStringLiteral("-"));
    if (solo_) return tr("single player");
    return game_ ? game_->status() : tr("not running");
}

bool MatchController::openUdp(QString* error) {
    // every interface, so a player across the internet reaches this one, even when the server runs on this PC
    const QHostAddress bind = cfg::udpLoopback() ? QHostAddress(QHostAddress::LocalHost)
                                                 : QHostAddress(QHostAddress::AnyIPv4);
    udp_->setBlocked(cfg::get(QStringLiteral("dev/block_udp")).toBool());  // development: a network without UDP
    return udp_->open(static_cast<quint16>(cfg::gamePort()), bind, error);
}

void MatchController::openUdpThen(std::function<void()> next, int tries) {
    QString err;
    if (openUdp(&err)) {
        next();
        return;
    }
    if (tries <= 0) {
        log(err);
        phase_ = Phase::Found;
        decline();
        return;
    }
    const QString match = str(found_, "match");
    QTimer::singleShot(100, this, [this, next, tries, match] {
        if (str(found_, "match") != match) return;  // that match is gone meanwhile
        openUdpThen(next, tries - 1);
    });
}

QString MatchController::lanEndpoint() const {
    return QStringLiteral("%1:%2").arg(server_->localAddress().toString()).arg(cfg::gamePort());
}

// A player on the server's own network shows up with an address of that network (127.0.0.1 when it plays on the
// server's PC); from anywhere else it is reached at the server's address, on its own port.
QString MatchController::reachableFromHere(const QString& endpoint) const {
    const int colon = endpoint.lastIndexOf(QLatin1Char(':'));
    const QHostAddress addr(endpoint.left(colon));
    const QHostAddress server = server_->serverAddress();
    if (colon <= 0 || addr.isNull() || server.isLoopback()) return endpoint;  // on the server's PC: as the server sees it
    const auto local = [](const QHostAddress& a) { return a.isPrivateUse() || a.isLinkLocal(); };
    if (!addr.isLoopback() && !(local(addr) && !local(server))) return endpoint;
    return server.toString() + endpoint.mid(colon);
}

QString MatchController::opponentName() const {
    const QJsonObject opp = found_.value(QStringLiteral("opponent")).toObject();
    return opp.isEmpty() ? tr("opponent") : str(opp, "name");
}

GameProcess* MatchController::spawnGame(const QString& gameId, QString* error) {
    showWhenReady_ = soloFrozen_;
    auto* g = new GameProcess(gameId, 1, this);
    connect(g, &GameProcess::statusChanged, this, &MatchController::onGameStatus);
    connect(g, &GameProcess::exited, this, &MatchController::onGameExited);
    if (!g->start(cfg::gameExe(gameId), error)) {
        delete g;
        return nullptr;
    }
    game_ = g;
    gameFrozen_ = false;
    log(tr("Game %1 starting (session %2)").arg(gameId, g->sessionDir()));
    return game_;
}

GameProcess* MatchController::ensureGame(const QString& gameId, QString* error) {
    // something else than the series' next match from CONTINUE?: the game goes back to its main menu for it (a start
    // waits until it is there)
    if (atContinue_) parkFromContinue(false);
    // one game runs at a time: while single player runs, only an accepted match (which freezes it) brings this one
    if (solo_ && !soloFrozen_) {
        if (error) *error = tr("Close your single-player game first.");
        return nullptr;
    }
    if (phase_ == Phase::Finishing) {
        if (error) *error = tr("Your game is still on its way back from the last match: a moment.");
        return nullptr;
    }
    if (game_ && game_->isRunning() && game_->gameId() == gameId) {
        if (gameFrozen_) {
            thawGame();
            showWhenReady_ = soloFrozen_;
            if (game_->isReady()) emit gameShown();  // at its hold point: up at once
        }
        return game_;
    }
    if (game_) {
        thawGame();  // a frozen game would not see its quit command
        disconnect(game_, nullptr, this, nullptr);
        game_->quit();
        QTimer::singleShot(6000, game_, &QObject::deleteLater);
        game_ = nullptr;
    }
    return spawnGame(gameId, error);
}

void MatchController::restartGame(const QString& why) {
    if (!game_) {
        finishDone();
        return;
    }
    GameProcess* old = game_;
    const QString gameId = old->gameId();
    thawGame();  // a frozen game would not see its quit command
    game_ = nullptr;
    gameFrozen_ = false;
    disconnect(old, nullptr, this, nullptr);
    log(tr("Starting the game again for the next match: %1").arg(why));
    const auto next = [this, gameId] {
        if (game_) return;  // one was started meanwhile
        QString err;
        if (!spawnGame(gameId, &err)) log(err);
        if (phase_ == Phase::Finishing) finishDone();
    };
    if (!old->isRunning()) {
        old->deleteLater();
        next();
        return;
    }
    // once it is gone and has let go of the game port: a new session with a fresh save, booting to the hold point
    connect(old, &GameProcess::exited, this, [old, next] {
        old->deleteLater();
        next();
    });
    old->quit();
}

bool MatchController::prepareGame(const QString& gameId, QString* error) { return ensureGame(gameId, error) != nullptr; }

bool MatchController::findMatch(const QueueRequest& req, QString* error) {
    if (!server_->isLoggedIn()) {
        if (error) *error = tr("Sign in first.");
        return false;
    }
    if (phase_ != Phase::Idle && phase_ != Phase::Searching && phase_ != Phase::Solo) {
        if (error) *error = tr("Busy: %1").arg(phaseText_);
        return false;
    }
    // standby: single player runs on while this searches; the match's game starts once a match is accepted
    if (!solo_ && !ensureGame(req.game, error)) return false;
    request_ = req;
    QString uerr;
    if (!openUdp(&uerr)) {
        if (!game_ || !game_->isRunning() || game_->isReady()) {
            if (error) *error = uerr;
            return false;
        }
        searchClock_.start();
        setPhase(Phase::Searching, tr("Searching: the game lets go of its port first..."));
        queueWhenPortFree(50);
        return true;
    }
    sendQueueJoin();
    return true;
}

void MatchController::queueWhenPortFree(int tries) {
    QTimer::singleShot(100, this, [this, tries] {
        if (phase_ != Phase::Searching) return;  // cancelled meanwhile
        QString err;
        if (openUdp(&err)) {
            sendQueueJoin();
        } else if (tries > 0) {
            queueWhenPortFree(tries - 1);
        } else {
            log(err);
            setPhase(Phase::Idle, tr("Search not started: %1").arg(err));
        }
    });
}

void MatchController::sendQueueJoin() {
    const QueueRequest& req = request_;
    udp_->startHello(server_->serverAddress(), server_->serverUdpPort(), server_->udpToken(), QString(),
                     lanEndpoint(), 3000);
    QJsonObject msg{{QStringLiteral("type"), QStringLiteral("queue_join")},
                    {QStringLiteral("game"), req.game},
                    {QStringLiteral("mode"), req.mode},
                    {QStringLiteral("ranked"), req.ranked},
                    {QStringLiteral("region"), req.region},
                    {QStringLiteral("max_ping"), req.maxPing},
                    {QStringLiteral("strict"), req.strict},
                    {QStringLiteral("character"), req.character},
                    {QStringLiteral("delay"), cfg::inputDelayValue(req.delay)},
                    {QStringLiteral("netcode"), req.netcode}};
    if (udp_->serverRttMs() >= 0) msg.insert(QStringLiteral("server_rtt"), udp_->serverRttMs());
    server_->send(msg);
    searchClock_.start();
    const Game* g = GameData::instance().game(req.game);
    const GameMode* m = g ? g->mode(req.mode) : nullptr;
    setPhase(Phase::Searching, tr("Searching for a %1 %2 opponent...")
                                   .arg(req.ranked ? tr("ranked") : tr("Kasual"), m ? m->name : req.mode));
    keepaliveTimer_.start();
}

void MatchController::sendKeepaliveHello() {
    if (phase_ != Phase::Searching || !server_->isLoggedIn()) return;
    QString err;
    if (!udp_->isOpen() && !openUdp(&err)) return;
    udp_->startHello(server_->serverAddress(), server_->serverUdpPort(), server_->udpToken(), QString(),
                     lanEndpoint(), 2000);
}

void MatchController::cancelSearch() {
    if (phase_ == Phase::Found || phase_ == Phase::Connecting) {
        decline();
        return;
    }
    if (phase_ != Phase::Searching) return;
    server_->send({{QStringLiteral("type"), QStringLiteral("queue_leave")}});
    releaseUdp();
    setPhase(Phase::Idle, tr("Search cancelled"));
}

void MatchController::accept() {
    if (phase_ != Phase::Found) return;
    if (solo_ && !soloFrozen_) {  // standby: single player steps aside, the match's game comes up
        freezeSolo();
        QString err;
        if (!ensureGame(str(found_, "game"), &err)) {
            log(err);
            thawSolo();
            decline();
            return;
        }
    }
    server_->send({{QStringLiteral("type"), QStringLiteral("match_accept")}, {QStringLiteral("match"), str(found_, "match")}});
    setPhase(Phase::Connecting, tr("Accepted; waiting for the opponent..."));
}

void MatchController::decline() {
    if (phase_ != Phase::Found && phase_ != Phase::Connecting) return;
    server_->send({{QStringLiteral("type"), QStringLiteral("match_decline")}, {QStringLiteral("match"), str(found_, "match")}});
    found_ = {};
    releaseUdp();
    setPhase(Phase::Idle, tr("Match declined"));
}

bool MatchController::canLeaveMatch() const {
    return (phase_ == Phase::Starting || phase_ == Phase::Playing) && !go_.isEmpty() && !resultSent_;
}

bool MatchController::leavingCosts() const { return canLeaveMatch() && go_.value(QStringLiteral("ranked")).toBool(); }

void MatchController::leaveMatch() {
    if (!canLeaveMatch()) return;
    if (leavingCosts()) {
        log(tr("You left the ranked match against %1").arg(opponentName()));
        reportResult({{QStringLiteral("outcome"), QStringLiteral("left")}});
        return;
    }
    log(tr("You ended the match against %1 (no result: it does not count)").arg(opponentName()));
    reportResult({{QStringLiteral("outcome"), QStringLiteral("ended")}});
}

void MatchController::releaseUdp() { udp_->close(); }

void MatchController::onServerMessage(const QJsonObject& msg) {
    const QString type = str(msg, "type");
    const QString match = str(msg, "match");
    if (type == QLatin1String("queued")) {
        if (phase_ == Phase::Idle) {
            searchClock_.start();
            setPhase(Phase::Searching, tr("Searching..."));
        }
        if (phase_ == Phase::Searching) keepaliveTimer_.start();
    } else if (type == QLatin1String("queue_left")) {
        if (phase_ == Phase::Searching) {
            releaseUdp();
            setPhase(Phase::Idle, str(msg, "reason") == QLatin1String("removed") ? tr("Removed from the queue")
                                                                                 : tr("Search stopped"));
        }
    } else if (type == QLatin1String("match_found")) {
        found_ = msg;
        go_ = {};
        resultSent_ = false;
        closeFeed();
        closeBridge();
        tunnel_ = false;
        udpAcked_ = false;
        QString err;
        // standby: the match's game waits for the accept, which sets single player aside
        const bool standby = solo_ && !soloFrozen_ && msg.value(QStringLiteral("accept_required")).toBool();
        if (!standby && !ensureGame(str(msg, "game"), &err)) {
            log(err);
            phase_ = Phase::Found;
            decline();
            return;
        }
        openUdpThen([this, msg, match] {
            udp_->startHello(server_->serverAddress(), server_->serverUdpPort(), server_->udpToken(), match,
                             lanEndpoint(), 10000);
            // sooner when the hellos while searching went unanswered already
            if (server_->canTunnel()) tunnelAsk_.start(udpBlocked_ ? 1000 : 3000);
            if (msg.value(QStringLiteral("accept_required")).toBool()) {
                setPhase(Phase::Found, tr("Match found"));
                emit matchFound(msg);
            } else {
                setPhase(Phase::Connecting, msg.contains(QStringLiteral("series"))
                                                ? tr("Next match: connecting to %1...").arg(opponentName())
                                                : tr("Lobby match: connecting to %1...").arg(opponentName()));
                emit matchFound(msg);
            }
        }, 30);
    } else if (type == QLatin1String("match_peer")) {
        if (match != str(found_, "match")) return;
        found_.insert(QStringLiteral("opponent"), msg.value(QStringLiteral("opponent")));
        const int punchMs = msg.value(QStringLiteral("punch_timeout")).toInt(10) * 1000;
        tunnel_ = msg.value(QStringLiteral("tunnel")).toBool();
        if (tunnel_) {
            // UDP gets nowhere from here: the opponent's packets come and go through the server connection
            tunnelAsk_.stop();
            setPhase(Phase::Connecting, tr("Connecting to %1 through the server connection (no UDP here)...")
                                            .arg(opponentName()));
            udp_->startTunnelPunch(match, str(msg, "key"), msg.value(QStringLiteral("side")).toInt(), punchMs);
            return;
        }
        if (msg.value(QStringLiteral("relay")).toBool()) {
            // no direct route between the two: the server passes the packets on, its UDP port is the opponent now
            setPhase(Phase::Connecting, tr("Connecting to %1 through the server...").arg(opponentName()));
            udp_->startRelayPunch(match, str(msg, "key"), msg.value(QStringLiteral("side")).toInt(), punchMs);
            return;
        }
        setPhase(Phase::Connecting, tr("Connecting to %1...").arg(opponentName()));
        const QJsonObject peer = msg.value(QStringLiteral("peer")).toObject();
        const QString pub = reachableFromHere(str(peer, "public"));
        if (pub != str(peer, "public")) log(tr("%1 plays beside the server: reaching it at %2").arg(opponentName(), pub));
        udp_->startPunch(match, str(msg, "key"), msg.value(QStringLiteral("side")).toInt(), str(peer, "lan"), pub,
                         punchMs);
    } else if (type == QLatin1String("match_go")) {
        if (!str(msg, "rematch").isEmpty()) {
            startRematch(msg);
            return;
        }
        if (match != str(found_, "match")) return;
        go_ = msg;
        tunnelAsk_.stop();
        releaseUdp();  // the game binds the same port right away
        openFeed(msg);
        if (tunnel_) {
            // the game's peer is on this PC: what it sends goes through the server connection
            auto* bridge = new TunnelBridge(static_cast<quint16>(cfg::gamePort()), this);
            QString err;
            if (bridge->open(&err)) {
                connect(bridge, &TunnelBridge::outgoing, server_, &ServerClient::sendTunnel);
                bridge_ = bridge;
            } else {
                log(err);
                delete bridge;
            }
        }
        pendingStart_ = startLine(go_);
        setPhase(Phase::Starting, tr("Starting the match against %1 (%2 ms, %3, delay %4%5)...")
                                      .arg(opponentName()).arg(msg.value(QStringLiteral("rtt_ms")).toInt())
                                      .arg(netcodeOf(msg)).arg(msg.value(QStringLiteral("delay")).toInt())
                                      .arg(msg.value(QStringLiteral("delay_auto")).toBool() ? tr(" auto") : QString()));
        emit matchStarted(msg);
        // the game may still be booting to its hold point (started again after the last match), and the players
        // then pick on its select screen: the start waits for that
        const bool picksInGame = str(msg, "p1").isEmpty() || str(msg, "p1") == QLatin1String("-") ||
                                 str(msg, "p2").isEmpty() || str(msg, "p2") == QLatin1String("-");
        startTimeout_.start(picksInGame ? 300000 : 120000);
        tryStart();
    } else if (type == QLatin1String("match_cancelled")) {
        if (match != str(found_, "match")) return;
        const bool requeued = msg.value(QStringLiteral("requeued")).toBool();
        const QString reason = str(msg, "reason");
        found_ = {};
        tunnelAsk_.stop();
        tunnel_ = false;
        releaseUdp();
        emit matchCancelled(reason, requeued);
        if (requeued) {
            setPhase(Phase::Searching, tr("Back in the queue: %1").arg(reasonText(reason)));
            keepaliveTimer_.start();
            sendKeepaliveHello();
        } else {
            setPhase(Phase::Idle, tr("Match cancelled: %1").arg(reasonText(reason)));
        }
    } else if (type == QLatin1String("match_result")) {
        if (match != str(go_, "match")) return;
        QJsonObject result = msg;
        const int side = go_.value(QStringLiteral("side")).toInt();
        const int winner = msg.value(QStringLiteral("winner")).toInt();
        result.insert(QStringLiteral("side"), side);
        result.insert(QStringLiteral("opponent"), side == 1 ? str(go_, "p2_name") : str(go_, "p1_name"));
        result.insert(QStringLiteral("lobby"), go_.value(QStringLiteral("lobby")));
        if (winner == 1 || winner == 2) {
            const int mine = msg.value(side == 1 ? QStringLiteral("p1_rounds") : QStringLiteral("p2_rounds")).toInt();
            const int theirs = msg.value(side == 1 ? QStringLiteral("p2_rounds") : QStringLiteral("p1_rounds")).toInt();
            result.insert(QStringLiteral("you_won"), winner == side);
            result.insert(QStringLiteral("score"), QStringLiteral("%1-%2").arg(mine).arg(theirs));
        }
        emit matchFinished(result);
        finishMatch(str(msg, "status"));
    } else if (type == QLatin1String("series")) {
        const QString state = str(msg, "state");
        series_ = msg;
        if (state == QLatin1String("between") || state == QLatin1String("playing")) continueTimer_.stop();
        if (state == QLatin1String("over") && atContinue_) parkFromContinue(true);  // no next match: back to the menu
        // a ranked set goes on by itself: its next match once this game is back at its hold point
        if (state == QLatin1String("between") && msg.value(QStringLiteral("ranked")).toBool() &&
            !msg.value(QStringLiteral("you_ready")).toBool())
            wantNext_ = true;
        if (state == QLatin1String("over")) wantNext_ = false;
        emit seriesChanged(msg);
        if (state == QLatin1String("over")) series_ = {};
        sendSeriesReady();
        maybeBackToSolo();
    } else if (type == QLatin1String("result_ack")) {
        log(tr("Result of %1 acknowledged by the server").arg(match));
    } else if (type == QLatin1String("spectate")) {
        // a lobby match this player watches: its game plays it from both players' feeds, once it is free
        if (match == str(watch_, "match")) return;
        for (const QJsonObject& q : std::as_const(watchQueue_))
            if (str(q, "match") == match) return;
        while (watchQueue_.size() >= 2) watchData_.remove(str(watchQueue_.takeFirst(), "match"));  // long missed
        watchQueue_.append(msg);
        watchData_.insert(match, QByteArray());
        log(tr("Lobby match %1 vs %2 to watch").arg(str(msg, "p1_name"), str(msg, "p2_name")));
        startWatch();
    } else if (type == QLatin1String("spectate_feed")) {
        const QByteArray data = QByteArray::fromBase64(str(msg, "data").toLatin1());
        if (watchFeed_ && match == watchFeedMatch_) watchFeed_->deliver(data);
        else if (watchData_.contains(match)) watchData_[match] += data;
    } else if (type == QLatin1String("spectate_end")) {
        // the players are done; this game, behind them, plays on to the same end with what it has
        if (match == str(watch_, "match"))
            log(tr("The players finished the match being watched (%1)").arg(str(msg, "status")));
    }
}

// --- spectators (docs/mk3d-reference/PROTOCOL.md 1.10.1)

void MatchController::openFeed(const QJsonObject& go) {
    closeFeed();
    if (!go.value(QStringLiteral("feed")).toBool()) return;
    auto* feed = new FeedLink(FeedLink::Role::Player, this);
    QString err;
    if (!feed->open(&err)) {
        log(err);
        delete feed;
        return;
    }
    const QString match = str(go, "match");
    connect(feed, &FeedLink::outgoing, this, [this, match](const QByteArray& packets) {
        if (server_->isLoggedIn())
            server_->send({{QStringLiteral("type"), QStringLiteral("feed")},
                           {QStringLiteral("match"), match},
                           {QStringLiteral("data"), QString::fromLatin1(packets.toBase64())}});
    });
    feed_ = feed;
}

// once the game is back at its hold point: what it wrote last (the match's end) goes out first
void MatchController::closeFeed() {
    if (!feed_) return;
    QPointer<FeedLink> feed = feed_;
    feed_ = nullptr;
    QTimer::singleShot(1500, this, [this, feed] {
        if (!feed) return;
        feed->flush();
        log(tr("Spectator feed: %1 KB passed on").arg(feed->bytes() / 1024));
        feed->deleteLater();
    });
}

// a spectator's start line: no slot or peers, every player's inputs come from the feed (the game's netplay plays the
// match in lockstep with no player of its own)
QString MatchController::watchStartLine(const QJsonObject& info) const {
    const QStringList tokens = {
        QStringLiteral("start"),
        QStringLiteral("mode=") + str(info, "mode"),
        QStringLiteral("watch=1"),
        QStringLiteral("feed=") + (watchFeed_ ? watchFeed_->endpoint() : QString()),
        QStringLiteral("players=2"),
        QStringLiteral("netcode=") + netcodeOf(info),
        QStringLiteral("seed=%1").arg(info.value(QStringLiteral("seed")).toInteger()),
        QStringLiteral("match=") + str(info, "match"),
        QStringLiteral("p1=") + pick(info, "p1"),
        QStringLiteral("p2=") + pick(info, "p2"),
        QStringLiteral("rounds=%1").arg(info.value(QStringLiteral("rounds")).toInt(3)),
        QStringLiteral("ranked=0"),
        QStringLiteral("p1_name=") + str(info, "p1_name"),
        QStringLiteral("p2_name=") + str(info, "p2_name"),
    };
    return tokens.join(QLatin1Char(' '));
}

// the next lobby match to watch, once nothing else runs here (a search, a match, single player, the last match's end)
void MatchController::startWatch() {
    if (!watch_.isEmpty() || watchQueue_.isEmpty() || phase_ != Phase::Idle) return;
    const QJsonObject info = watchQueue_.takeFirst();
    const QString match = str(info, "match");
    const QByteArray data = watchData_.take(match);
    QString err;
    if (!ensureGame(str(info, "game"), &err)) {
        log(tr("Cannot watch the lobby's match: %1").arg(err));
        return;
    }
    auto* feed = new FeedLink(FeedLink::Role::Watcher, this);
    if (!feed->open(&err)) {
        log(err);
        delete feed;
        return;
    }
    feed->deliver(data);
    closeWatchFeed();
    watchFeed_ = feed;
    watchFeedMatch_ = match;
    watch_ = info;
    watchInMatch_ = false;
    finishText_ = tr("Done watching");
    pendingStart_ = watchStartLine(info);
    setPhase(Phase::Watching, tr("Watching %1 vs %2: the match is being set up...")
                                  .arg(str(info, "p1_name"), str(info, "p2_name")));
    // the players pick on the select screen, and the game may start behind them
    watchTimeout_.start(6 * 60 * 1000);
    tryStart();
    emit gameShown();  // the watched match comes to the game
}

void MatchController::onWatchStatus(const QString& state, const QString& line) {
    if (state == QLatin1String("ready")) {
        tryStart();
    } else if (state == QLatin1String("in_match")) {
        if (!watchInMatch_) {
            watchInMatch_ = true;
            watchFeedCheck_.start(kWatchFeedCheckMs);
        }
        watchTimeout_.stop();
        setPhase(Phase::Watching, tr("Watching %1 vs %2").arg(str(watch_, "p1_name"), str(watch_, "p2_name")));
        emit gameShown();
    } else if (state == QLatin1String("result")) {
        endWatch(tr("Done watching %1 vs %2").arg(str(watch_, "p1_name"), str(watch_, "p2_name")));
    } else if (state == QLatin1String("desync")) {
        endWatch(tr("The watched match went out of step with the players' games; stopped watching"));
    } else if (state == QLatin1String("error") && !watchInMatch_) {
        watchUnsupported(line.section(QLatin1Char(' '), 1));  // the game turned the watch line down
    } else if (state == QLatin1String("disconnected") || state == QLatin1String("error")) {
        endWatch(tr("The watched match ended before its end (a player left)"));
    }
}

// the game could not watch: it turned the watch line down (error) or plays without reading the feed
void MatchController::watchUnsupported(const QString& why) {
    const QString text = tr("Your game could not watch the lobby's match (%1); it starts again for the next one.").arg(why);
    endWatch(text, true);
    watchQueue_.clear();
    watchData_.clear();
    emit notice(text);
}

// the game parks once the watched match's end has played, from the rest of the feed, which goes on to it until then;
// then the next one may come
void MatchController::endWatch(const QString& text, bool restart) {
    watchTimeout_.stop();
    watchFeedCheck_.stop();
    startCheck_.stop();
    watch_ = {};
    watchInMatch_ = false;
    pendingStart_.clear();
    finishText_ = text;
    if (game_ && game_->isRunning() && !game_->isReady()) {
        setPhase(Phase::Finishing, text);
        if (restart) {
            closeWatchFeed();
            restartGame(tr("it did not take the watched match"));
            return;
        }
        game_->command(QStringLiteral("park"));
        finishTimeout_.start(45000);
        return;
    }
    closeWatchFeed();
    setPhase(Phase::Idle, text);
}

void MatchController::closeWatchFeed() {
    if (!watchFeed_) return;
    watchFeed_->deleteLater();
    watchFeed_ = nullptr;
    watchFeedMatch_.clear();
}

void MatchController::stopWatching() {
    watchQueue_.clear();
    watchData_.clear();
    if (phase_ == Phase::Watching) endWatch(tr("Stopped watching"));
}

void MatchController::askForTunnel() {
    if (udpAcked_ || tunnel_ || found_.isEmpty() || (phase_ != Phase::Found && phase_ != Phase::Connecting)) return;
    server_->send({{QStringLiteral("type"), QStringLiteral("tunnel")}, {QStringLiteral("match"), str(found_, "match")}});
    log(tr("The server does not answer over UDP (this network may block it): the match goes through the server "
           "connection instead"));
}

void MatchController::onTunneled(const QList<QByteArray>& datagrams) {
    for (const QByteArray& d : datagrams) {
        if (bridge_) bridge_->toGame(d);
        else udp_->deliverTunneled(d);
    }
}

void MatchController::closeBridge() {
    if (!bridge_) return;
    log(tr("Through the server connection: %1 packets went out, %2 came in").arg(bridge_->sent()).arg(bridge_->received()));
    bridge_->deleteLater();
    bridge_ = nullptr;
}

void MatchController::onPunchDone(bool ok, int rtt, const QString& remote, const QString& via) {
    if (found_.isEmpty()) return;
    // the attempt's key: a report the server gets after it moved on to another attempt is left aside there
    QJsonObject report{{QStringLiteral("type"), QStringLiteral("ping_report")},
                       {QStringLiteral("match"), str(found_, "match")},
                       {QStringLiteral("ok"), ok},
                       {QStringLiteral("rtt_ms"), rtt},
                       {QStringLiteral("key"), udp_->key()}};
    if (ok) {
        report.insert(QStringLiteral("remote"), remote);
        report.insert(QStringLiteral("via"), via);
    }
    server_->send(report);
    log(ok ? tr("Connected to %1 via %2 (%3), %4 ms").arg(opponentName(), via, remote).arg(rtt)
           : tr("Could not reach %1 this way").arg(opponentName()));
}

// MK9's start line (docs/LAUNCHER.md, the game contract): side 1 is slot 0 (player 1, on the left), side 2 slot 1; the
// other player is the one peer. rounds, ranked and the names are for the game to show or use, the rest it plays by.
QString MatchController::startLine(const QJsonObject& go) const {
    const int slot = go.value(QStringLiteral("side")).toInt() == 2 ? 1 : 0;
    const QStringList tokens = {
        QStringLiteral("start"),
        QStringLiteral("mode=") + str(go, "mode"),
        QStringLiteral("slot=%1").arg(slot),
        QStringLiteral("players=2"),
        QStringLiteral("local_port=%1").arg(cfg::gamePort()),
        QStringLiteral("peers=%1@%2").arg(1 - slot).arg(bridge_ ? bridge_->endpoint() : str(go, "remote")),
        QStringLiteral("delay=%1").arg(go.value(QStringLiteral("delay")).toInt()),
        QStringLiteral("netcode=") + netcodeOf(go),
        QStringLiteral("seed=%1").arg(go.value(QStringLiteral("seed")).toInteger()),
        QStringLiteral("match=") + str(go, "match"),
        QStringLiteral("p1=") + pick(go, "p1"),
        QStringLiteral("p2=") + pick(go, "p2"),
        QStringLiteral("rounds=%1").arg(go.value(QStringLiteral("rounds")).toInt(3)),
        QStringLiteral("ranked=%1").arg(go.value(QStringLiteral("ranked")).toBool() ? 1 : 0),
        QStringLiteral("p1_name=") + str(go, "p1_name"),
        QStringLiteral("p2_name=") + str(go, "p2_name"),
    };
    QString line = tokens.join(QLatin1Char(' '));
    if (feed_) line += QStringLiteral(" feed=") + feed_->endpoint();
    return line;
}

void MatchController::tryStart() {
    if (pendingStart_.isEmpty() || !game_) return;
    const bool can = pendingRematch_ ? game_->state() == QLatin1String("result") : game_->isReady();
    if (!can) {
        log(tr("Waiting for the game to reach its hold point (status: %1)").arg(game_->status()));
        return;
    }
    game_->command(pendingStart_);
    pendingStart_.clear();
    pendingRematch_ = false;
    startCheck_.start(5000);
}

void MatchController::startRematch(const QJsonObject& go) {
    const int side = go.value(QStringLiteral("side")).toInt();
    found_ = go;  // no match_found for it: the match is the one the go names
    found_.insert(QStringLiteral("opponent"),
                  QJsonObject{{QStringLiteral("name"), side == 1 ? str(go, "p2_name") : str(go, "p1_name")}});
    go_ = go;
    resultSent_ = false;
    const bool there = atContinue_ && game_ && game_->isRunning() && game_->state() == QLatin1String("result");
    atContinue_ = false;
    continueTimer_.stop();
    const bool select = str(go, "rematch") == QLatin1String("select");
    setPhase(Phase::Starting, select ? tr("Next match against %1: both pick again...").arg(opponentName())
                                     : tr("Rematch against %1: the same fighters...").arg(opponentName()));
    emit matchStarted(go);
    if (!there) {
        log(tr("The game is no longer at its CONTINUE? menu: the match cannot start there"));
        reportResult({{QStringLiteral("outcome"), QStringLiteral("error")}});
        return;
    }
    pendingStart_ = QStringLiteral("rematch match=%1 seed=%2%3")
                        .arg(str(go, "match"))
                        .arg(go.value(QStringLiteral("seed")).toInteger())
                        .arg(select ? QStringLiteral(" select=1") : QString());
    pendingRematch_ = true;
    startTimeout_.start(select ? 300000 : 120000);
    tryStart();
}

void MatchController::parkFromContinue(bool finishing) {
    if (!atContinue_) return;
    atContinue_ = false;
    continueTimer_.stop();
    if (!game_ || !game_->isRunning() || game_->state() != QLatin1String("result")) return;  // not there any more
    game_->command(QStringLiteral("park"));
    if (finishing && phase_ == Phase::Idle) {
        finishText_ = tr("Back at the menu");
        finishTimeout_.start(kParkMs);
        setPhase(Phase::Finishing, tr("The game goes back to its menu"));
    } else {
        parkWatch_.start(kParkMs);
    }
}

void MatchController::onGameStatus(const QString& line) {
    emit gameStatusChanged(line);
    const QString st = line.section(QLatin1Char(' '), 0, 0);
    if (phase_ == Phase::Watching) {
        onWatchStatus(st, line);
        return;
    }
    const QHash<QString, QString> keys = GameProcess::parseKeys(line);
    const QString goMatch = str(go_, "match");
    const bool forThisMatch = !keys.contains(QStringLiteral("match")) || keys.value(QStringLiteral("match")) == goMatch;
    const bool inOnline = (phase_ == Phase::Starting || phase_ == Phase::Playing) && !go_.isEmpty();
    if (st == QLatin1String("ready")) {
        if (showWhenReady_) {  // standby: started for an accepted match, its window is up now
            showWhenReady_ = false;
            emit gameShown();
        }
        if (phase_ == Phase::Finishing) {
            finishTimeout_.stop();
            closeFeed();
            closeWatchFeed();
            finishDone();
        }
        tryStart();
        maybeStartSolo();
    } else if (st == QLatin1String("in_match")) {
        if (phase_ == Phase::Starting && forThisMatch) {
            startTimeout_.stop();
            setPhase(Phase::Playing, tr("In match against %1").arg(opponentName()));
            server_->send({{QStringLiteral("type"), QStringLiteral("match_state")},
                           {QStringLiteral("match"), goMatch},
                           {QStringLiteral("state"), QStringLiteral("in_match")}});
        }
    } else if (st == QLatin1String("result") && inOnline && forThisMatch) {
        QJsonObject result{{QStringLiteral("winner"), keys.value(QStringLiteral("winner")).toInt()},
                           {QStringLiteral("p1_rounds"), keys.value(QStringLiteral("p1_rounds")).toInt()},
                           {QStringLiteral("p2_rounds"), keys.value(QStringLiteral("p2_rounds")).toInt()}};
        // the fighters the game saw, what a rematch with the same fighters starts with
        for (const QString k : {QStringLiteral("p1_char"), QStringLiteral("p2_char")})
            if (keys.contains(k)) result.insert(k, keys.value(k));
        reportResult(result);
    } else if (st == QLatin1String("desync") && inOnline && forThisMatch) {
        reportResult({{QStringLiteral("outcome"), QStringLiteral("desync")},
                      {QStringLiteral("frame"), keys.value(QStringLiteral("frame")).toInt()}});
    } else if (st == QLatin1String("disconnected") && inOnline && forThisMatch) {
        reportResult({{QStringLiteral("outcome"), QStringLiteral("disconnected")}});
    } else if (st == QLatin1String("error") && inOnline) {
        // "error exited": GameProcess's word for a game that closed by itself (Alt+F4, a crash), which ends the match
        // for both: the player left it (onGameExited)
        reportResult({{QStringLiteral("outcome"),
                       line.startsWith(QLatin1String("error exited")) ? QStringLiteral("left") : QStringLiteral("error")}});
    }
}

void MatchController::onGameExited() {
    switch (phase_) {
    case Phase::Searching:
        log(tr("The game closed; leaving the queue."));
        cancelSearch();
        break;
    case Phase::Found:
    case Phase::Connecting:
        decline();
        break;
    case Phase::Starting:
    case Phase::Playing:
        // the player closed their game (or it crashed) during the match: they left it, which ends the series (and
        // loses a ranked set) instead of offering a rematch
        reportResult({{QStringLiteral("outcome"), QStringLiteral("left")}});
        break;
    case Phase::Finishing:
        finishTimeout_.stop();
        setPhase(Phase::Idle, tr("Match over"));
        break;
    case Phase::Watching:
        endWatch(tr("The game closed while watching"));
        break;
    default:
        break;
    }
}

void MatchController::reportResult(QJsonObject fields) {
    if (resultSent_ || go_.isEmpty()) return;
    resultSent_ = true;
    startTimeout_.stop();
    startCheck_.stop();
    pendingStart_.clear();
    pendingRematch_ = false;
    fields.insert(QStringLiteral("type"), QStringLiteral("result"));
    fields.insert(QStringLiteral("match"), str(go_, "match"));
    if (server_->isLoggedIn())
        server_->send(fields);
    else
        pendingResult_ = fields;
    const QString what = fields.contains(QStringLiteral("winner"))
                             ? tr("player %1 won %2-%3").arg(fields.value(QStringLiteral("winner")).toInt())
                                   .arg(fields.value(QStringLiteral("p1_rounds")).toInt())
                                   .arg(fields.value(QStringLiteral("p2_rounds")).toInt())
                             : fields.value(QStringLiteral("outcome")).toString();
    setPhase(Phase::Reporting, tr("Reporting the result (%1)...").arg(what));
    parkTimer_.start(10000);
}

// The match is over (its result is in, or 10 s went by). A queue or series match that ended with a result keeps its
// game at the CONTINUE? menu, together with the opponent's, for the series' next match (both choose a rematch, or a
// ranked set goes on); otherwise the game goes back to its hold point: from its CONTINUE? menu a park takes it back by
// itself, anywhere else (a match ended in the middle, the games lost each other) it cannot, and is started again: a
// new session with a fresh save boots to the hold point.
void MatchController::finishMatch(const QString& status) {
    parkTimer_.stop();
    startTimeout_.stop();
    if (bridge_) {  // the games' last packets of the match (their goodbyes) still go through it for a while
        QPointer<TunnelBridge> bridge = bridge_;
        QTimer::singleShot(20000, this, [this, bridge] {
            if (bridge && bridge == bridge_) closeBridge();
        });
    }
    const bool fromLobby = go_.value(QStringLiteral("lobby")).isString();
    const bool keep = !fromLobby && status == QLatin1String("agreed") && game_ && game_->isRunning() &&
                      game_->state() == QLatin1String("result");
    go_ = {};
    found_ = {};
    pendingStart_.clear();
    pendingRematch_ = false;
    finishText_.clear();
    if (keep) {
        atContinue_ = true;
        continueTimer_.start(8000);  // the series' state comes right after the result
        closeFeed();
        setPhase(Phase::Idle, tr("Match over: the game waits at its CONTINUE? menu for the next match"));
        return;
    }
    if (game_ && game_->isRunning() && !game_->isReady()) {
        if (game_->state() == QLatin1String("result")) {
            game_->command(QStringLiteral("park"));
            finishTimeout_.start(kParkMs);
            setPhase(Phase::Finishing, tr("Match over: the game goes back to its menu"));
        } else {
            setPhase(Phase::Finishing, tr("Match over: the game starts again for the next match"));
            restartGame(tr("the match ended away from the game's CONTINUE? menu"));
        }
        return;
    }
    closeFeed();
    setPhase(Phase::Idle, tr("Match over"));
}

void MatchController::finishDone() {
    finishTimeout_.stop();
    setPhase(Phase::Idle, finishText_.isEmpty() ? tr("Match over") : finishText_);
}

// The whole game started plainly, its menus as on the console, with the player's saves: no hold point and no match.
// One game runs at a time: the game waiting in the background for a match closes first, or, while a search runs
// (standby), is frozen until a match is accepted.
bool MatchController::startSolo(const QString& gameId, QString* error, bool whileWaiting) {
    if (solo_) {
        if (error) *error = tr("Your single-player game is already open.");
        return false;
    }
    if (phase_ != Phase::Idle && phase_ != Phase::Searching) {
        if (error) *error = tr("Busy: %1").arg(phaseText_);
        return false;
    }
    if (atContinue_) parkFromContinue(false);
    if (game_ && phase_ == Phase::Searching) {
        freezeGame();
    } else if (game_) {
        GameProcess* waiting = game_;
        game_ = nullptr;
        gameFrozen_ = false;
        disconnect(waiting, nullptr, this, nullptr);
        waiting->quit();
        QElapsedTimer t;
        t.start();
        while (waiting->isRunning() && t.elapsed() < 3000) QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        waiting->deleteLater();
    }
    auto* g = new GameProcess(gameId, 1, this);
    if (!g->start(cfg::gameExe(gameId), error, false)) {
        delete g;
        thawGame();
        return false;
    }
    solo_ = g;
    soloWaiting_ = whileWaiting;
    soloFrozen_ = false;
    connect(g, &GameProcess::exited, this, [this, g] {
        if (solo_ != g) return;
        const QString closed = tr("Single player closed");
        solo_ = nullptr;
        soloWaiting_ = false;
        soloFrozen_ = false;
        soloHidden_.clear();
        g->deleteLater();
        if (phase_ == Phase::Solo) {
            setPhase(Phase::Idle, closed);
        } else {
            log(closed);
            // still searching: the match's game waits for it in the open again
            QString err;
            if (phase_ == Phase::Searching && !ensureGame(request_.game, &err)) log(err);
        }
        emit gameStatusChanged(gameStatus());
    });
    log(tr("Game %1 started for single player (session %2, saves %3)").arg(gameId, g->sessionDir(), g->saveDir()));
    if (phase_ == Phase::Idle) setPhase(Phase::Solo, tr("Playing single player"));
    else log(tr("Playing single player while the search runs: a match found comes up over it"));
    emit gameStatusChanged(gameStatus());
    emit soloStarted();
    return true;
}

// Single player while a Ranked or Kasual search waits: once the match's game is at its hold point, ready for a match, it
// is frozen there (a match found thaws it at once) and the game starts again plainly (standby takes it from there)
void MatchController::maybeStartSolo() {
    if (phase_ != Phase::Searching || solo_ || !game_ || !game_->isReady() || !cfg::soloWhileWaiting()) return;
    QString err;
    if (!startSolo(request_.game, &err, true)) log(tr("Single player while you wait: %1").arg(err));
}

void MatchController::stopSolo() {
    if (solo_) solo_->quit();  // its window closes; the exit makes the controller idle
}

bool MatchController::startLocalTest(const QString& gameId, const QString& mode, const QString& p1,
                                     const QString& p2, QString* error) {
    if (phase_ != Phase::Idle) {
        if (error) *error = tr("Busy: %1").arg(phaseText_);
        return false;
    }
    stopLocalTest();
    const QString exe = cfg::gameExe(gameId);
    localA_ = new GameProcess(gameId, 1, this);
    localB_ = new GameProcess(gameId, 2, this);
    for (GameProcess* g : {localA_, localB_}) g->setWindowed(true);  // two fullscreen games would fight for the screen
    if (!localA_->start(exe, error) || !localB_->start(exe, error)) {
        delete localA_;
        delete localB_;
        localA_ = localB_ = nullptr;
        return false;
    }
    for (GameProcess* g : {localA_, localB_}) {
        connect(g, &GameProcess::statusChanged, this, &MatchController::onLocalStatus);
        connect(g, &GameProcess::exited, this, &MatchController::onLocalStatus);
    }
    const int portA = cfg::localTestPort(1), portB = cfg::localTestPort(2);
    const quint32 seed = QRandomGenerator::global()->generate();
    // Auto: what a match over loopback (no round trip to speak of) gets
    const int localDelay = cfg::inputDelay() < 0 ? cfg::autoDelay(0) : cfg::inputDelay();
    const QString match = QStringLiteral("local-") + QDateTime::currentDateTime().toString(QStringLiteral("hhmmss"));
    const auto line = [&](int slot, int local, int remote) {
        return QStringList{
            QStringLiteral("start"),
            QStringLiteral("mode=") + mode,
            QStringLiteral("slot=%1").arg(slot),
            QStringLiteral("players=2"),
            QStringLiteral("local_port=%1").arg(local),
            QStringLiteral("peers=%1@127.0.0.1:%2").arg(1 - slot).arg(remote),
            QStringLiteral("delay=%1").arg(localDelay),
            QStringLiteral("netcode=") + cfg::netcode(),
            QStringLiteral("seed=%1").arg(seed),
            QStringLiteral("match=") + match,
            QStringLiteral("p1=") + (p1.isEmpty() ? QStringLiteral("-") : p1),
            QStringLiteral("p2=") + (p2.isEmpty() ? QStringLiteral("-") : p2),
            QStringLiteral("rounds=3"),
            QStringLiteral("ranked=0"),
        }.join(QLatin1Char(' '));
    };
    localStartA_ = line(0, portA, portB);
    localStartB_ = line(1, portB, portA);
    localResultA_.clear();
    localResultB_.clear();
    setPhase(Phase::LocalTest, tr("Local test: two games on ports %1/%2 (%3), waiting for both to be ready")
                                   .arg(portA).arg(portB).arg(cfg::netcode()));
    return true;
}

void MatchController::onLocalStatus() {
    if (!localA_ || !localB_) return;
    emit gameStatusChanged(gameStatus());
    if (!localStartA_.isEmpty() && localA_->isReady() && localB_->isReady()) {
        localA_->command(localStartA_);
        localB_->command(localStartB_);
        setPhase(Phase::LocalTest, tr("Local test: start lines written, both games starting"));
        localStartA_.clear();
        localStartB_.clear();
    }
    for (GameProcess* g : {localA_, localB_})
        if (g->state() == QLatin1String("error") || g->state() == QLatin1String("desync") ||
            g->state() == QLatin1String("disconnected"))
            log(tr("Local test: game %1 says %2").arg(g->instance()).arg(g->status()));
    const auto strip = [](const QString& s) { return s.section(QStringLiteral(" match="), 0, 0); };
    if (localA_->state() == QLatin1String("result")) localResultA_ = strip(localA_->status());
    if (localB_->state() == QLatin1String("result")) localResultB_ = strip(localB_->status());
    if (!localResultA_.isEmpty() && !localResultB_.isEmpty()) {
        setPhase(Phase::LocalTest,
                 localResultA_ == localResultB_ ? tr("Local test: both games agree: %1").arg(localResultA_)
                                                : tr("Local test: results differ: %1 / %2").arg(localResultA_, localResultB_));
        localResultA_.clear();
        localResultB_.clear();
        localA_->command(QStringLiteral("park"));
        localB_->command(QStringLiteral("park"));
    }
    if (!localA_->isRunning() && !localB_->isRunning()) {
        log(tr("Local test: both games exited"));
        localA_->deleteLater();
        localB_->deleteLater();
        localA_ = localB_ = nullptr;
        if (phase_ == Phase::LocalTest) setPhase(Phase::Idle, tr("Local test finished"));
    }
}

void MatchController::stopLocalTest() {
    if (!localA_ && !localB_) return;
    for (GameProcess* g : {localA_, localB_})
        if (g) {
            disconnect(g, nullptr, this, nullptr);
            g->quit();
            QTimer::singleShot(6000, g, &QObject::deleteLater);
        }
    localA_ = localB_ = nullptr;
    if (phase_ == Phase::LocalTest) setPhase(Phase::Idle, tr("Local test stopped"));
}

void MatchController::stopGame() {
    thawSolo();  // frozen games would not see their quit
    stopSolo();
    thawGame();
    if (game_) game_->quit();
}

void MatchController::shutdown() {
    closeBridge();
    stopLocalTest();
    if (phase_ == Phase::Searching) cancelSearch();
    thawSolo();  // frozen games would not see their quit
    thawGame();
    // guarded: an exit handler run by the wait below may delete its game (single player's does)
    QList<QPointer<GameProcess>> procs;
    for (GameProcess* g : findChildren<GameProcess*>()) procs.append(g);
    for (const QPointer<GameProcess>& g : procs)
        if (g->isRunning()) g->quit();
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < 2500) {
        bool any = false;
        for (const QPointer<GameProcess>& g : procs) any = any || (g && g->isRunning());
        if (!any) break;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    }
    for (const QPointer<GameProcess>& g : procs)
        if (g) g->kill();
}
