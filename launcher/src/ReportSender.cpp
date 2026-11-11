#include "ReportSender.h"

#include <QBuffer>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSysInfo>

#include <algorithm>

#include "AppPaths.h"
#include "GameProcess.h"
#include "ServerClient.h"

namespace {

constexpr int kNextFrames = 20;     // the game's kNextFrames: next_00 .. next_19 follow the frame F3 caught
constexpr qint64 kWaitMs = 15000;   // a report whose last pictures never came goes as it is
constexpr int kPartBytes = 11400;   // a report_file part's bytes (base64 under the server's 16000, a line under 16 KB)
constexpr int kPartsPerTick = 2;    // every 100 ms: about 230 KB a second
constexpr qint64 kLogHead = 16 << 10;   // the start of the game's log: the graphics card, what was set
constexpr qint64 kLogTail = 256 << 10;  // the end of it

QByteArray jpeg(QImage img, int maxWidth, int quality) {
    if (img.width() > maxWidth) img = img.scaledToWidth(maxWidth, Qt::SmoothTransformation);
    QByteArray out;
    QBuffer buf(&out);
    buf.open(QIODevice::WriteOnly);
    img.convertToFormat(QImage::Format_RGB888).save(&buf, "JPG", quality);
    return out;
}

// what may leave this PC: no IP address, no user folder (as the desync reports)
QByteArray clean(const QByteArray& raw) {
    static const QRegularExpression ip(QStringLiteral("\\b\\d{1,3}(\\.\\d{1,3}){3}\\b"));
    static const QRegularExpression home(QStringLiteral("([A-Za-z]:\\\\Users\\\\)[^\\\\\\r\\n]+"));
    QString text = QString::fromUtf8(raw);
    text.replace(ip, QStringLiteral("x.x.x.x"));
    text.replace(home, QStringLiteral("\\1*"));
    return text.toUtf8();
}

}  // namespace

ReportSender::ReportSender(ServerClient* server, std::function<QList<GameProcess*>()> games, QObject* parent)
    : QObject(parent), server_(server), games_(std::move(games)) {
    pollTimer_.setInterval(2000);
    connect(&pollTimer_, &QTimer::timeout, this, &ReportSender::poll);
    pollTimer_.start();
    sendTimer_.setInterval(100);
    connect(&sendTimer_, &QTimer::timeout, this, &ReportSender::sendSome);
    connect(server_, &ServerClient::message, this, &ReportSender::onMessage);
}

void ReportSender::poll() {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (GameProcess* g : games_()) {
        if (!g || g->sessionDir().isEmpty()) continue;
        const QDir caps(QDir(g->sessionDir()).filePath(QStringLiteral("captures")));
        for (const QString& d : caps.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            const QString path = caps.filePath(d);
            if (seen_.contains(path)) continue;
            seen_.insert(path);
            waiting_.append({g->gameId(), g->sessionDir(), path, now});
        }
    }
    if (!sending_.isEmpty() || !server_->isLoggedIn()) return;  // one at a time, signed in
    for (qsizetype i = 0; i < waiting_.size(); ++i) {
        if (complete(waiting_[i]) || now - waiting_[i].firstSeen > kWaitMs) {
            build(waiting_.takeAt(i));
            return;
        }
    }
}

bool ReportSender::complete(const Capture& c) const {
    return QFile::exists(QDir(c.dir).filePath(QStringLiteral("state.txt")));  // the game writes it last
}

void ReportSender::build(const Capture& c) {
    struct File {
        QString name;
        QByteArray data;
        bool zlib;
    };
    QList<File> files;
    const QDir dir(c.dir);
    if (const QImage img(dir.filePath(QStringLiteral("final.png"))); !img.isNull())
        files.append({QStringLiteral("final.jpg"), jpeg(img, 1920, 90), false});
    for (int i = 0; i < kNextFrames; i++) {
        const QString n = QStringLiteral("next_%1").arg(i, 2, 10, QLatin1Char('0'));
        if (const QImage img(dir.filePath(n + QStringLiteral(".png"))); !img.isNull())
            files.append({n + QStringLiteral(".jpg"), jpeg(img, 1280, 85), false});
    }
    if (QFile f(dir.filePath(QStringLiteral("state.txt"))); f.open(QIODevice::ReadOnly))
        files.append({QStringLiteral("state.txt"), qCompress(f.readAll(), 9), true});
    if (QFile f(QDir(c.sessionDir).filePath(QStringLiteral("game.log"))); f.open(QIODevice::ReadOnly)) {
        QByteArray log;
        if (f.size() > kLogHead + kLogTail) {
            log = f.read(kLogHead) + "\n[...]\n";
            f.seek(f.size() - kLogTail);
        }
        log += f.readAll();
        files.append({QStringLiteral("game.log"), qCompress(clean(log), 9), true});
    }
    const QSize window = cfg::windowSize();
    const QString info = QStringLiteral("launcher %1\ngame %2\nsession %3\ncapture %4\nsent %5 UTC\nwindows %6\n"
                                        "display: %7, window %8x%9, drawn at %10x, vsync %11\n")
                             .arg(QStringLiteral(MK9_LAUNCHER_VERSION), c.gameId, QFileInfo(c.sessionDir).fileName(),
                                  QFileInfo(c.dir).fileName(),
                                  QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")),
                                  QSysInfo::prettyProductName(),
                                  cfg::fullscreen() ? QStringLiteral("fullscreen") : QStringLiteral("window"))
                             .arg(window.width())
                             .arg(window.height())
                             .arg(cfg::renderScale())
                             .arg(cfg::vsync() ? QStringLiteral("on") : QStringLiteral("off"));
    files.append({QStringLiteral("info.txt"), qCompress(info.toUtf8(), 9), true});

    sending_ = QStringLiteral("%1").arg(QRandomGenerator::global()->generate64(), 16, 16, QLatin1Char('0')).left(16);
    sendingDir_ = c.dir;
    files_ = int(files.size());
    bytes_ = 0;
    for (const File& f : files) {
        bytes_ += f.data.size();
        const int parts = std::max(1, int((f.data.size() + kPartBytes - 1) / kPartBytes));
        for (int i = 0; i < parts; i++) {
            QJsonObject m{{QStringLiteral("type"), QStringLiteral("report_file")},
                          {QStringLiteral("report"), sending_},
                          {QStringLiteral("game"), c.gameId},
                          {QStringLiteral("name"), f.name},
                          {QStringLiteral("part"), i},
                          {QStringLiteral("parts"), parts},
                          {QStringLiteral("data"), QString::fromLatin1(f.data.mid(qsizetype(i) * kPartBytes, kPartBytes).toBase64())}};
            if (f.zlib) m.insert(QStringLiteral("zlib"), true);
            outbox_.append(m);
        }
    }
    outbox_.append({{QStringLiteral("type"), QStringLiteral("report_done")}, {QStringLiteral("report"), sending_}});
    emit logLine(tr("F3 report: sending %1 files (%2 KB) to the developers").arg(files_).arg(bytes_ / 1024));
    sendTimer_.start();
}

void ReportSender::sendSome() {
    if (outbox_.isEmpty()) {
        sendTimer_.stop();
        return;
    }
    if (!server_->isLoggedIn()) {  // the server drops a report with its connection
        finish(tr("F3 report not sent: the connection to the server dropped"));
        return;
    }
    for (int i = 0; i < kPartsPerTick && !outbox_.isEmpty(); i++) server_->send(outbox_.takeFirst());
}

void ReportSender::onMessage(const QJsonObject& msg) {
    if (sending_.isEmpty()) return;
    const QString type = msg.value(QStringLiteral("type")).toString();
    if (type == QLatin1String("report_saved") && msg.value(QStringLiteral("report")).toString() == sending_) {
        QDir(sendingDir_).removeRecursively();  // the server has it: the pictures take tens of MB here
        finish(tr("F3 report sent, thank you"));
    } else if (type == QLatin1String("error")) {
        const QString re = msg.value(QStringLiteral("re")).toString();
        if (re == QLatin1String("report_file") || re == QLatin1String("report_done"))
            finish(tr("F3 report not sent: %1").arg(msg.value(QStringLiteral("message")).toString()));
    }
}

void ReportSender::finish(const QString& text) {
    outbox_.clear();
    sendTimer_.stop();
    sending_.clear();
    sendingDir_.clear();
    emit logLine(text);
}
