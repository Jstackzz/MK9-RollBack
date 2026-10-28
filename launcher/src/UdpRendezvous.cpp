#include "UdpRendezvous.h"

#include <QJsonDocument>
#include <QNetworkDatagram>
#include <QUdpSocket>

#include <algorithm>

#include "AppPaths.h"

namespace {
constexpr int kPings = 5;
constexpr int kMinPongs = 3;

QHostAddress plainV4(const QHostAddress& a) {
    bool ok = false;
    const quint32 v4 = a.toIPv4Address(&ok);
    return ok ? QHostAddress(v4) : a;
}

bool parseEndpoint(const QString& text, QHostAddress* addr, quint16* port) {
    const int colon = text.lastIndexOf(QLatin1Char(':'));
    if (colon <= 0) return false;
    QHostAddress a(text.left(colon));
    const uint p = text.mid(colon + 1).toUInt();
    if (a.isNull() || a.protocol() != QAbstractSocket::IPv4Protocol || p == 0 || p > 65535) return false;
    *addr = a;
    *port = static_cast<quint16>(p);
    return true;
}
}  // namespace

UdpRendezvous::UdpRendezvous(QObject* parent) : QObject(parent) {
    clock_.start();
    helloTimer_.setInterval(250);
    connect(&helloTimer_, &QTimer::timeout, this, &UdpRendezvous::onHelloTick);
    punchTimer_.setInterval(100);
    connect(&punchTimer_, &QTimer::timeout, this, &UdpRendezvous::onPunchTick);
}

UdpRendezvous::~UdpRendezvous() { close(); }

bool UdpRendezvous::open(quint16 port, const QHostAddress& bind, QString* error) {
    if (sock_ && port_ == port && bind_ == bind) return true;
    close();
    sock_ = new QUdpSocket(this);
    if (!sock_->bind(bind, port)) {
        if (error) *error = tr("Cannot use UDP port %1: %2").arg(port).arg(sock_->errorString());
        delete sock_;
        sock_ = nullptr;
        return false;
    }
    port_ = port;
    bind_ = bind;
    connect(sock_, &QUdpSocket::readyRead, this, &UdpRendezvous::onReadyRead);
    applog::write(QStringLiteral("udp: bound %1:%2").arg(bind.toString()).arg(port));
    return true;
}

void UdpRendezvous::close() {
    helloTimer_.stop();
    punchTimer_.stop();
    phase_ = Phase::Idle;
    match_.clear();
    key_.clear();
    if (sock_) {
        sock_->close();
        sock_->deleteLater();
        sock_ = nullptr;
        applog::write(QStringLiteral("udp: released port %1").arg(port_));
    }
}

bool UdpRendezvous::isOpen() const { return sock_ != nullptr; }

void UdpRendezvous::sendJson(const QJsonObject& obj, const QHostAddress& addr, quint16 port) {
    if (!sock_ || blocked_ || addr.isNull() || port == 0) return;
    sock_->writeDatagram(QJsonDocument(obj).toJson(QJsonDocument::Compact), addr, port);
}

void UdpRendezvous::sendTo(const QJsonObject& obj, const Candidate& to) {
    if (to.via == QLatin1String("tunnel")) {
        if (tunnelSend_) tunnelSend_(QJsonDocument(obj).toJson(QJsonDocument::Compact));
        return;
    }
    sendJson(obj, to.addr, to.port);
}

void UdpRendezvous::startHello(const QHostAddress& server, quint16 serverPort, const QString& token,
                               const QString& match, const QString& lan, int timeoutMs) {
    server_ = plainV4(server);
    serverPort_ = serverPort;
    token_ = token;
    helloMatch_ = match;
    lan_ = lan;
    helloTimeoutMs_ = timeoutMs;
    helloStarted_ = clock_.elapsed();
    sendHello();
    helloTimer_.start();
}

void UdpRendezvous::sendHello() {
    sendJson({{QStringLiteral("type"), QStringLiteral("hello")},
              {QStringLiteral("token"), token_},
              {QStringLiteral("match"), helloMatch_},
              {QStringLiteral("lan"), lan_},
              {QStringLiteral("t"), clock_.elapsed()}},
             server_, serverPort_);
}

void UdpRendezvous::onHelloTick() {
    if (clock_.elapsed() - helloStarted_ > helloTimeoutMs_) {
        helloTimer_.stop();
        applog::write(QStringLiteral("udp: no hello_ack from %1:%2").arg(server_.toString()).arg(serverPort_));
        emit helloTimedOut();
        return;
    }
    sendHello();
}

void UdpRendezvous::startPunch(const QString& match, const QString& key, int side, const QString& lan,
                               const QString& pub, int timeoutMs) {
    match_ = match;
    key_ = key;
    side_ = side;
    candidates_.clear();
    for (const auto& [via, text] : {std::pair{QStringLiteral("lan"), lan}, std::pair{QStringLiteral("public"), pub}}) {
        Candidate c;
        c.via = via;
        if (!parseEndpoint(text, &c.addr, &c.port)) continue;
        const bool dup = std::any_of(candidates_.cbegin(), candidates_.cend(),
                                     [&](const Candidate& o) { return o.addr == c.addr && o.port == c.port; });
        if (!dup) candidates_.append(c);
    }
    beginPunch(timeoutMs);
}

void UdpRendezvous::startRelayPunch(const QString& match, const QString& key, int side, int timeoutMs) {
    match_ = match;
    key_ = key;
    side_ = side;
    candidates_ = {Candidate{QStringLiteral("relay"), server_, serverPort_}};
    beginPunch(timeoutMs);
}

// the relay through the server connection: one candidate, the tunnel (its address, the server's UDP endpoint, is what
// the report names)
void UdpRendezvous::startTunnelPunch(const QString& match, const QString& key, int side, int timeoutMs) {
    match_ = match;
    key_ = key;
    side_ = side;
    candidates_ = {Candidate{QStringLiteral("tunnel"), server_, serverPort_}};
    beginPunch(timeoutMs);
}

void UdpRendezvous::deliverTunneled(const QByteArray& datagram) { handleDatagram(datagram, server_, serverPort_, true); }

void UdpRendezvous::beginPunch(int timeoutMs) {
    punchTimeoutMs_ = timeoutMs;
    punchStarted_ = clock_.elapsed();
    pingsSent_ = 0;
    pingSentAt_.clear();
    rtts_.clear();
    phase_ = candidates_.isEmpty() ? Phase::Done : Phase::Punching;
    if (phase_ == Phase::Done) {
        emit punchDone(false, 0, QString(), QString());
        return;
    }
    onPunchTick();
    punchTimer_.start();
}

void UdpRendezvous::onPunchTick() {
    const qint64 now = clock_.elapsed();
    if (phase_ == Phase::Punching) {
        if (now - punchStarted_ > punchTimeoutMs_) {
            finishPunch(false);
            return;
        }
        for (const auto& c : candidates_)
            sendTo({{QStringLiteral("type"), QStringLiteral("punch")}, {QStringLiteral("match"), match_},
                    {QStringLiteral("key"), key_}, {QStringLiteral("side"), side_}},
                   c);
    } else if (phase_ == Phase::Pinging) {
        if (pingsSent_ < kPings) {
            pingSentAt_.insert(pingsSent_, now);
            sendTo({{QStringLiteral("type"), QStringLiteral("ping")}, {QStringLiteral("match"), match_},
                    {QStringLiteral("key"), key_}, {QStringLiteral("seq"), pingsSent_}, {QStringLiteral("t"), now}},
                   working_);
            ++pingsSent_;
            lastPingAt_ = now;
        } else if (rtts_.size() >= kPings || now - lastPingAt_ > 500) {
            finishPunch(rtts_.size() >= kMinPongs);
        }
    }
}

void UdpRendezvous::finishPunch(bool ok) {
    phase_ = Phase::Done;
    punchTimer_.stop();
    int rtt = 0;
    if (ok && !rtts_.isEmpty()) {
        QList<qint64> sorted = rtts_;
        std::sort(sorted.begin(), sorted.end());
        rtt = static_cast<int>(sorted.at(sorted.size() / 2));
    }
    const QString remote = ok ? QStringLiteral("%1:%2").arg(working_.addr.toString()).arg(working_.port) : QString();
    applog::write(QStringLiteral("udp: punch %1, rtt %2 ms via %3 %4").arg(ok ? "ok" : "failed").arg(rtt).arg(working_.via, remote));
    emit punchDone(ok, rtt, remote, ok ? working_.via : QString());
}

void UdpRendezvous::onReadyRead() {
    while (sock_ && sock_->hasPendingDatagrams()) {
        const QNetworkDatagram d = sock_->receiveDatagram(2048);
        handleDatagram(d.data(), plainV4(d.senderAddress()), static_cast<quint16>(d.senderPort()), false);
    }
}

// `tunneled`: it came through the server connection (a tunnel's punch or ping), and the answer goes back that way
void UdpRendezvous::handleDatagram(const QByteArray& data, const QHostAddress& from, quint16 fromPort, bool tunneled) {
    QJsonParseError err{};
    const QJsonObject o = QJsonDocument::fromJson(data, &err).object();
    if (err.error != QJsonParseError::NoError) return;
    const QString type = o.value(QStringLiteral("type")).toString();
    if (type == QLatin1String("hello_ack")) {
        if (tunneled || !helloTimer_.isActive()) return;
        helloTimer_.stop();
        serverRtt_ = static_cast<int>(qMax<qint64>(0, clock_.elapsed() - o.value(QStringLiteral("t")).toInteger()));
        const QString pub = o.value(QStringLiteral("public")).toString();
        applog::write(QStringLiteral("udp: hello_ack public %1, server rtt %2 ms").arg(pub).arg(serverRtt_));
        emit helloAcked(pub, serverRtt_);
        return;
    }
    if (key_.isEmpty() || o.value(QStringLiteral("match")).toString() != match_ ||
        o.value(QStringLiteral("key")).toString() != key_)
        return;  // not our match (or an attempt before this one): ignore
    const Candidate source{tunneled ? QStringLiteral("tunnel") : QStringLiteral("public"), from, fromPort};
    if (!tunneled && type == QLatin1String("punch") && phase_ == Phase::Punching &&
        std::none_of(candidates_.cbegin(), candidates_.cend(),
                     [&](const Candidate& c) { return c.addr == from && c.port == fromPort; })) {
        // the peer's router sends it from another public port than the server saw: punch that one too, or
        // only one side would ever hear back
        candidates_.append(source);
        applog::write(QStringLiteral("udp: punch from %1:%2, added as a candidate").arg(from.toString()).arg(fromPort));
    }
    if (type == QLatin1String("punch") || type == QLatin1String("ping")) {
        QJsonObject reply = o;
        reply.insert(QStringLiteral("type"), type == QLatin1String("punch") ? QStringLiteral("punch_ack")
                                                                            : QStringLiteral("pong"));
        reply.insert(QStringLiteral("side"), side_);
        sendTo(reply, source);
    } else if (type == QLatin1String("punch_ack") && phase_ == Phase::Punching) {
        working_ = source;
        for (const auto& c : candidates_)
            if (!tunneled && c.addr == from && c.port == fromPort) working_.via = c.via;
        phase_ = Phase::Pinging;
        pingsSent_ = 0;
        lastPingAt_ = clock_.elapsed();
    } else if (type == QLatin1String("pong") && phase_ == Phase::Pinging) {
        const int seq = o.value(QStringLiteral("seq")).toInt(-1);
        if (pingSentAt_.contains(seq)) {
            rtts_.append(clock_.elapsed() - pingSentAt_.take(seq));
            if (rtts_.size() >= kPings) finishPunch(true);
        }
    }
}

TunnelBridge::TunnelBridge(quint16 gamePort, QObject* parent)
    : QObject(parent), sock_(new QUdpSocket(this)), gamePort_(gamePort) {}

bool TunnelBridge::open(QString* error) {
    // on loopback: only this PC's game talks to it (no firewall prompt), on a port Windows picks
    if (!sock_->bind(QHostAddress(QHostAddress::LocalHost), 0)) {
        if (error) *error = tr("Cannot open the match's tunnel on this PC: %1").arg(sock_->errorString());
        return false;
    }
    connect(sock_, &QUdpSocket::readyRead, this, [this] {
        QList<QByteArray> batch;
        while (sock_->hasPendingDatagrams()) {
            const QNetworkDatagram d = sock_->receiveDatagram(2048);
            if (d.senderPort() != gamePort_) continue;  // the game's packets only
            batch << d.data();
        }
        sent_ += batch.size();
        if (!batch.isEmpty()) emit outgoing(batch);
    });
    applog::write(QStringLiteral("tunnel: the game's peer is %1").arg(endpoint()));
    return true;
}

QString TunnelBridge::endpoint() const { return QStringLiteral("127.0.0.1:%1").arg(sock_->localPort()); }

void TunnelBridge::toGame(const QByteArray& datagram) {
    received_++;
    sock_->writeDatagram(datagram, QHostAddress(QHostAddress::LocalHost), gamePort_);
}
