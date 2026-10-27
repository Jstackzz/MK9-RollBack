#pragma once
// Updates from the project's GitHub releases (Settings: update/github_repo, default Jstackzz/MK9-RollBack).
// The latest release names the package zip (MK9Online-<version>.zip) and its SHA-256 (GitHub's asset digest, or a
// <zip>.sha256 asset beside it); found() hands it to the same offer, download, check and install as a server's
// release (Updater). Asked at start and every few hours; nothing is sent but the request itself.

#include <QJsonObject>
#include <QObject>
#include <QTimer>

class QNetworkAccessManager;
class QNetworkReply;

class GitHubUpdates : public QObject {
    Q_OBJECT
public:
    explicit GitHubUpdates(QObject* parent = nullptr);
    void check();
    static QString repo();

signals:
    // {version, url, size, sha256, notes}: what Updater::usable checks
    void found(const QJsonObject& release);

private:
    void onRelease(QNetworkReply* reply);
    void onChecksum(QNetworkReply* reply, QJsonObject release);

    QNetworkAccessManager* net_ = nullptr;
    QTimer every_;
    bool busy_ = false;
};
