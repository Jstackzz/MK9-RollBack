#pragma once
// The launcher's UDP side during match setup (docs/PROTOCOL.md section 2), on the game port:
// hello to the server's rendezvous, then punch both peer candidates and measure the round trip.
// A player whose network lets no UDP through does the same through the server connection (a tunnel,
// docs/PROTOCOL.md), and TunnelBridge is then the game's peer on this PC.

#include <QElapsedTimer>
#include <QHash>
#include <QHostAddress>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QTimer>

#include <functional>

class QUdpSocket;

class UdpRendezvous : public QObject {
    Q_OBJECT
public:
    explicit UdpRendezvous(QObject* parent = nullptr);
    ~UdpRendezvous() override;

    // `bind` is AnyIPv4 normally; loopback when the server itself is on this PC (no firewall prompt)
    bool open(quint16 port, const QHostAddress& bind, QString* error);
    void close();
    bool isOpen() const;
    quint16 port() const { return port_; }

    void startHello(const QHostAddress& server, quint16 serverPort, const QString& token, const QString& match,
                    const QString& lan, int timeoutMs = 10000);
    int serverRttMs() const { return serverRtt_; }

    void startPunch(const QString& match, const QString& key, int side, const QString& lan, const QString& pub,
                    int timeoutMs);
    // the same through the server's relay (the two could not reach each other): the opponent's packets come and go
    // through the server's UDP port, the one the hellos went to
    void startRelayPunch(const QString& match, const QString& key, int side, int timeoutMs);
    // and through the server connection (a tunnel): what it sends goes to `send`, what comes back to deliverTunneled
    void setTunnelSender(std::function<void(const QByteArray&)> send) { tunnelSend_ = std::move(send); }
    void startTunnelPunch(const QString& match, const QString& key, int side, int timeoutMs);
    void deliverTunneled(const QByteArray& datagram);
    QString key() const { return key_; }  // the attempt's, from its match_peer: ping_report sends it back
    // development: every datagram this sends goes nowhere (a network that lets no UDP through)
    void setBlocked(bool on) { blocked_ = on; }

signals:
    void helloAcked(const QString& publicEndpoint, int rttMs);
    void helloTimedOut();
    void punchDone(bool ok, int rttMs, const QString& remote, const QString& via);

private:
    struct Candidate {
        QString via;
        QHostAddress addr;
        quint16 port = 0;
    };
    enum class Phase { Idle, Punching, Pinging, Done };

    void onReadyRead();
    void handleDatagram(const QByteArray& data, const QHostAddress& from, quint16 fromPort, bool tunneled);
    void sendTo(const QJsonObject& obj, const Candidate& to);
    void onHelloTick();
    void beginPunch(int timeoutMs);
    void onPunchTick();
    void finishPunch(bool ok);
    void sendJson(const QJsonObject& obj, const QHostAddress& addr, quint16 port);
    void sendHello();

    QUdpSocket* sock_ = nullptr;
    quint16 port_ = 0;
    bool blocked_ = false;
    std::function<void(const QByteArray&)> tunnelSend_;
    QHostAddress bind_;
    QElapsedTimer clock_;
    QTimer helloTimer_, punchTimer_;

    QHostAddress server_;
    quint16 serverPort_ = 0;
    QString token_, helloMatch_, lan_;
    qint64 helloStarted_ = 0;
    int helloTimeoutMs_ = 10000;
    int serverRtt_ = -1;

    QString match_, key_;
    int side_ = 0;
    QList<Candidate> candidates_;
    Phase phase_ = Phase::Idle;
    qint64 punchStarted_ = 0;
    int punchTimeoutMs_ = 10000;
    Candidate working_;
    int pingsSent_ = 0;
    qint64 lastPingAt_ = 0;
    QHash<int, qint64> pingSentAt_;
    QList<qint64> rtts_;
};

// A tunnel's end on this PC: the game's peer (127.0.0.1:<port>, its start line's), which passes what the game sends to
// the server connection (outgoing) and what comes back from it to the game's port (toGame)
class TunnelBridge : public QObject {
    Q_OBJECT
public:
    explicit TunnelBridge(quint16 gamePort, QObject* parent = nullptr);
    bool open(QString* error);
    QString endpoint() const;
    void toGame(const QByteArray& datagram);
    qint64 sent() const { return sent_; }
    qint64 received() const { return received_; }

signals:
    void outgoing(const QList<QByteArray>& datagrams);

private:
    QUdpSocket* sock_;
    quint16 gamePort_;
    qint64 sent_ = 0, received_ = 0;
};
