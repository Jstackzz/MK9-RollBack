#include "GitHubUpdates.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QUrl>

#include "AppPaths.h"

namespace {

QNetworkRequest apiRequest(const QUrl& url) {
    QNetworkRequest req(url);
    req.setRawHeader("Accept", "application/vnd.github+json");
    req.setRawHeader("X-GitHub-Api-Version", "2022-11-28");
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("MK9Launcher/%1").arg(QStringLiteral(MK9_LAUNCHER_VERSION)));
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setTransferTimeout(20000);
    return req;
}

const QRegularExpression kZip(QStringLiteral("^MK9Online-[0-9.]+\\.zip$"));
const QRegularExpression kHex(QStringLiteral("[0-9a-fA-F]{64}"));

}  // namespace

GitHubUpdates::GitHubUpdates(QObject* parent) : QObject(parent), net_(new QNetworkAccessManager(this)) {
    every_.setInterval(6 * 60 * 60 * 1000);
    connect(&every_, &QTimer::timeout, this, &GitHubUpdates::check);
    every_.start();
}

QString GitHubUpdates::repo() {
    const QString r = cfg::get(QStringLiteral("update/github_repo"), QStringLiteral("Jstackzz/MK9-RollBack")).toString().trimmed();
    static const QRegularExpression ok(QStringLiteral("^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$"));
    return ok.match(r).hasMatch() ? r : QString();
}

void GitHubUpdates::check() {
    const QString r = repo();
    if (busy_ || r.isEmpty() || !cfg::get(QStringLiteral("update/github"), true).toBool()) return;
    busy_ = true;
    QNetworkReply* reply = net_->get(apiRequest(QUrl(QStringLiteral("https://api.github.com/repos/%1/releases/latest").arg(r))));
    connect(reply, &QNetworkReply::finished, this, [this, reply] { onRelease(reply); });
}

void GitHubUpdates::onRelease(QNetworkReply* reply) {
    reply->deleteLater();
    busy_ = false;
    if (reply->error() != QNetworkReply::NoError) {
        // 404: no release yet; anything else: offline or rate limited. Asked again later either way
        applog::write(QStringLiteral("update: GitHub (%1) not asked: %2").arg(repo(), reply->errorString()));
        return;
    }
    const QJsonObject rel = QJsonDocument::fromJson(reply->readAll()).object();
    if (rel.value(QStringLiteral("draft")).toBool() || rel.value(QStringLiteral("prerelease")).toBool()) return;
    QString version = rel.value(QStringLiteral("tag_name")).toString();
    if (version.startsWith(QLatin1Char('v')) || version.startsWith(QLatin1Char('V'))) version.remove(0, 1);
    QJsonObject zip, sum;
    const QJsonArray assets = rel.value(QStringLiteral("assets")).toArray();
    for (const auto& a : assets) {
        const QJsonObject o = a.toObject();
        if (kZip.match(o.value(QStringLiteral("name")).toString()).hasMatch()) zip = o;
    }
    if (zip.isEmpty()) {
        applog::write(QStringLiteral("update: GitHub release %1 has no MK9Online zip").arg(version));
        return;
    }
    for (const auto& a : assets) {
        const QJsonObject o = a.toObject();
        if (o.value(QStringLiteral("name")).toString() == zip.value(QStringLiteral("name")).toString() + QStringLiteral(".sha256"))
            sum = o;
    }
    QJsonObject release{{QStringLiteral("version"), version},
                        {QStringLiteral("url"), zip.value(QStringLiteral("browser_download_url")).toString()},
                        {QStringLiteral("size"), zip.value(QStringLiteral("size")).toDouble()},
                        {QStringLiteral("notes"), rel.value(QStringLiteral("body")).toString().left(1500)},
                        {QStringLiteral("source"), QStringLiteral("github")}};
    // GitHub's own digest of the asset ("sha256:..."), else the .sha256 file published beside it
    const QString digest = zip.value(QStringLiteral("digest")).toString();
    if (digest.startsWith(QLatin1String("sha256:")) && kHex.match(digest).hasMatch()) {
        release.insert(QStringLiteral("sha256"), kHex.match(digest).captured(0).toLower());
        emit found(release);
        return;
    }
    if (sum.isEmpty()) {
        applog::write(QStringLiteral("update: GitHub release %1 has no checksum; not offered").arg(version));
        return;
    }
    QNetworkReply* r2 = net_->get(apiRequest(QUrl(sum.value(QStringLiteral("browser_download_url")).toString())));
    connect(r2, &QNetworkReply::finished, this, [this, r2, release] { onChecksum(r2, release); });
}

void GitHubUpdates::onChecksum(QNetworkReply* reply, QJsonObject release) {
    reply->deleteLater();
    const auto m = kHex.match(QString::fromLatin1(reply->read(4096)));
    if (reply->error() != QNetworkReply::NoError || !m.hasMatch()) {
        applog::write(QStringLiteral("update: the checksum of GitHub release %1 could not be read")
                          .arg(release.value(QStringLiteral("version")).toString()));
        return;
    }
    release.insert(QStringLiteral("sha256"), m.captured(0).toLower());
    emit found(release);
}
