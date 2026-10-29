#pragma once

#include <functional>
// Drives one player's online match from queue_join to match_result (docs/mk3d-reference/PROTOCOL.md 1.7-1.9 and
// section 2, docs/PROTOCOL.md for MK9's changes) and owns the game processes: the match's game, which boots only once
// its match is on its way and closes when nothing wants it (no game waits for a match), single player, and the local
// test pair.

#include <QElapsedTimer>
#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QTimer>

class ServerClient;
class UdpRendezvous;
class TunnelBridge;
class GameProcess;
class FeedLink;

struct QueueRequest {
    QString game;
    QString mode;
    QString region;
    QString character;  // empty: picked on the game's select screen
    bool ranked = false;
    bool strict = false;
    int maxPing = 0;
    int delay = -1;  // frames, or cfg::kAutoDelay: from the match's measured ping (the server decides)
    QString netcode = QStringLiteral("lockstep");  // what this player wants; rollback only when both do
};

class MatchController : public QObject {
    Q_OBJECT
public:
    // Watching: the game plays a lobby match this player watches, from both players' feeds
    // (docs/mk3d-reference/PROTOCOL.md 1.10.1).
    enum class Phase { Idle, Searching, Found, Connecting, Starting, Playing, Reporting, Solo, LocalTest, Watching };
    Q_ENUM(Phase)

    explicit MatchController(ServerClient* server, QObject* parent = nullptr);
    ~MatchController() override;

    Phase phase() const { return phase_; }
    // a match on its way, played or watched: the game has the player's controller
    bool inMatch() const {
        return phase_ == Phase::Connecting || phase_ == Phase::Starting || phase_ == Phase::Playing ||
               phase_ == Phase::Reporting || phase_ == Phase::Watching;
    }
    QString phaseText() const { return phaseText_; }
    QueueRequest lastRequest() const { return request_; }
    QJsonObject currentFound() const { return found_; }
    int searchSeconds() const;
    // the max ping the server raised this search's to, waiting for a match (0: not raised): the rule it gave with
    // `queued`, from the time searched
    int raisedMaxPing() const;
    // this PC's round trip to the server, ms (the rendezvous' UDP one, else the connection's; -1 unknown)
    int serverRttMs() const;
    // match_found's `connection` in words (the log), and its ping part alone (beside the link's icon)
    static QString connectionText(const QJsonObject& found);
    static QString pingText(const QJsonObject& found);
    QString gameStatus() const;
    GameProcess* game() const { return game_; }

    bool findMatch(const QueueRequest& req, QString* error);
    void cancelSearch();
    void accept();
    void decline();
    // The player ends the match that runs (the game writes a result at its CONTINUE? menu; this is for a match that
    // will not get there, or a player who is done). Kasual and lobby matches: it does not count (result outcome
    // "ended"), both games close and a series goes on (its next match with new games). Ranked: as closing the game
    // ("left"), which loses the set once a match of it counted (leavingCosts, for the UI to ask first).
    bool canLeaveMatch() const;
    bool leavingCosts() const;
    void leaveMatch();
    // After a queue match the two players stay together in a series (docs/mk3d-reference/PROTOCOL.md 1.9.1): a
    // casual rematch whenever both choose one, a ranked set until one has won its matches. Its last state ({} when
    // none).
    QJsonObject series() const { return series_; }
    // ready for the series' next match (sent once this launcher is idle again: its game at its CONTINUE? menu, or
    // closed, a new one then booting for the next match), or leaving it (which loses a ranked set once a match of it
    // counted). Casual, the choice: "rematch" (the same fighters, when both choose it) or "select" (both pick again on
    // the select screen); a ranked set ignores it.
    void seriesReady(bool ready, const QString& choice = QStringLiteral("rematch"));
    // a lobby's Ready: whether this PC can play its match (the game built, the disc extracted); nothing starts yet
    bool prepareGame(const QString& gameId, QString* error);
    // the lobby's countdown for this player's match began: its game boots now, on both PCs at once; called off (a
    // player no longer ready), a game started for it closes
    void lobbyMatchComing(const QString& gameId);
    void lobbyMatchCalledOff();
    // the whole game with its own menus and the player's saves, no match. One game runs at a time, and a search waits
    // in it (standby): a match found pauses it under MATCH FOUND (winutil::freezeProcess); accepting closes it and
    // starts the match's game, a match not accepted lets it run on where it stood; CANCEL SEARCH closes it.
    // `whileWaiting`: started by a Ranked or Kasual search (cfg::soloWhileWaiting).
    bool startSolo(const QString& gameId, QString* error, bool whileWaiting = false);
    void stopSolo();
    bool soloRunning() const { return solo_ != nullptr; }
    bool soloWhileWaiting() const { return solo_ && soloWaiting_; }
    GameProcess* solo() const { return solo_; }
    // two games on this PC, one per slot, with matching start lines (p1/p2: roster ids or "-")
    bool startLocalTest(const QString& gameId, const QString& mode, const QString& p1, const QString& p2,
                        QString* error);
    void stopLocalTest();
    void stopGame();
    // a lobby left: a game started for its countdown closes (one playing or watching its match closes after it)
    void closeGameWhenIdle();
    void shutdown();
    // a spectator in a lobby: each of its matches plays in a game booted for it as the server passes it on (one at a
    // time, the next once that game is gone); stopWatching drops what waits and closes a match being watched
    bool watching() const { return !watch_.isEmpty(); }
    QJsonObject watchedMatch() const { return watch_; }
    void stopWatching();

signals:
    void phaseChanged(MatchController::Phase phase, const QString& text);
    void matchFound(const QJsonObject& found);
    void matchCancelled(const QString& reason, bool requeued);
    void matchStarted(const QJsonObject& go);
    void matchFinished(const QJsonObject& result);
    void seriesChanged(const QJsonObject& series);
    void gameStatusChanged(const QString& text);
    void logLine(const QString& text);
    void notice(const QString& text);  // for the player, over every page (a watched match the game could not play)
    void soloResumed();  // standby: single player runs again (a match found not accepted), its window wants the front
    void soloStarted();  // single player's game was started, its window (once it has one) wants the front
    void gameShown();    // the match's game started for a match on its way is up at its hold point

private:
    void setPhase(Phase p, const QString& text);
    void onServerMessage(const QJsonObject& msg);
    void onGameStatus(const QString& line);
    void onGameExited();
    void onPunchDone(bool ok, int rtt, const QString& remote, const QString& via);
    void onLocalStatus();
    GameProcess* ensureGame(const QString& gameId, QString* error);
    GameProcess* spawnGame(const QString& gameId, QString* error);
    // the match's game for a match on its way (false: it cannot start, `error` says why); bootPending once a game that
    // had to go first has gone; wantsGame: what keeps it running (else it closes, maybeCloseIdleGame)
    bool bootGame(const QString& gameId, QString* error);
    void bootPending();
    bool wantsGame() const;
    QString startLine(const QJsonObject& go) const;
    void tryStart();
    void reportResult(QJsonObject fields);
    void reportLeft();             // the game ended during the match: "left", with how it ended
    void reportDesync(int frame);  // the match went out of step: the game's log of it to the server
    // `status`: the server's match_result status ("" when it did not come)
    void finishMatch(const QString& status = {});
    // the series' next match from the games' CONTINUE? menu (match_go with "rematch"): the game's own rematch command
    void startRematch(const QJsonObject& go);
    void sendSeriesReady();
    void releaseUdp();
    void sendKeepaliveHello();
    QString lanEndpoint() const;
    QString reachableFromHere(const QString& endpoint) const;
    bool openUdp(QString* error);
    void sendQueueJoin();
    // the game port still held (by the last match's game as it closes): the search goes out once it is free
    void queueWhenPortFree(int tries);
    // openUdp, tried again every 0.1 s for a while when a game still holds the port (as it closes); then `next` runs,
    // or the match is declined
    void openUdpThen(std::function<void()> next, int tries);
    QString opponentName() const;
    void log(const QString& text);
    void freezeSolo();
    void thawSolo();
    void maybeBackToSolo();  // standby: a match found not accepted, single player again
    void closeGame(const QString& why);  // the match's game, once nothing wants it
    void closeSolo(const QString& why);  // single player: a match on its way, the search it waited in cancelled
    void maybeCloseIdleGame();           // the match's game once nothing wants it (wantsGame)
    // a tunnel (docs/PROTOCOL.md): no hello_ack for the match, so the server is asked to carry it through the
    // connection; its packets come and go there, to the punch and then to the game (bridge_)
    void askForTunnel();
    void onTunneled(const QList<QByteArray>& datagrams);
    void closeBridge();
    void openFeed(const QJsonObject& go);  // a lobby match: what its spectators need, from the game to the server
    void closeFeed();
    void closeWatchFeed();
    void startWatch();
    void endWatch(const QString& text);
    void onWatchStatus(const QString& state, const QString& line);
    void watchUnsupported(const QString& why);
    QString watchStartLine(const QJsonObject& info) const;

    ServerClient* server_;
    UdpRendezvous* udp_;
    GameProcess* game_ = nullptr;
    GameProcess* solo_ = nullptr;
    bool soloWaiting_ = false;  // solo_ is the game a search waits in (opened for it or open already)
    bool soloFrozen_ = false;   // paused under a match found
    QList<quintptr> soloHidden_;  // the windows freezing hid, shown again by thawing
    bool showWhenReady_ = false;  // the match's game started for a match on its way: to the front once it is up
    bool lobbyComing_ = false;    // the lobby's countdown for this player's match: its game boots meanwhile
    QString pendingBoot_;         // a match's game to start once the games closing first have gone (bootGame)
    QPointer<GameProcess> closingSolo_, closingGame_;  // the last single player and match's game closed, while they exit
    GameProcess* localA_ = nullptr;
    GameProcess* localB_ = nullptr;
    Phase phase_ = Phase::Idle;
    QString phaseText_;
    QueueRequest request_;
    QJsonObject found_, go_, pendingResult_, series_;
    QString pendingStart_;
    bool resultSent_ = false;
    bool wantNext_ = false;  // ready for the series' next match, not sent yet (the game still plays the last one's end)
    QString nextChoice_ = QStringLiteral("rematch");
    QElapsedTimer searchClock_;
    QJsonObject pingWiden_;  // the server's rule for raising the max ping ({} when it is not raised)
    QTimer parkTimer_, keepaliveTimer_, startTimeout_, watchTimeout_, watchFeedCheck_, startCheck_;
    QTimer watchEndTimeout_;  // the watched match ended early: the watching game's while to be in it, else it closes
    QTimer continueTimer_;  // no series' next match comes for the game held at CONTINUE?: it closes
    QTimer tunnelAsk_;      // the match's hellos unanswered this long: through the server connection
    bool udpAcked_ = false;    // a hello_ack came for this match
    bool udpBlocked_ = false;  // the last hellos to the server went unanswered
    bool tunnel_ = false;      // this match goes through the server connection
    TunnelBridge* bridge_ = nullptr;  // the game's peer on this PC in a match through the server connection
    // after a queue or series match with a result, the game waits at its CONTINUE? menu with the opponent's, so that
    // the series' next match can start there (the server's "rematch"); closed once nothing more comes
    bool atContinue_ = false;
    bool pendingRematch_ = false;  // pendingStart_ is a rematch command, for a game at CONTINUE? (not at the hold point)
    QString localStartA_, localStartB_, localResultA_, localResultB_;
    FeedLink* feed_ = nullptr;       // this player's lobby match's feed
    FeedLink* watchFeed_ = nullptr;  // the watched match's, to the game, until it has played the match's end
    QString watchFeedMatch_;
    QJsonObject watch_;              // the lobby match being watched (its spectate message), {} when none
    bool watchInMatch_ = false;      // the game took the watched match (in_match)
    QList<QJsonObject> watchQueue_;  // lobby matches to watch next
    QHash<QString, QByteArray> watchData_;  // their feeds so far, by match
};
