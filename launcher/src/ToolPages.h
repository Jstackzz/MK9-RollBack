#pragma once
// "Build game" (disc image -> the game data folder -> the build script) and Settings.

#include <QPointer>
#include <QTimer>

#include <atomic>
#include <functional>
#include <memory>

#include "Widgets.h"
#include "Xiso.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QProcess;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QThread;
class ServerClient;
class UpnpMapper;

class BuildPage : public Page {
    Q_OBJECT
public:
    explicit BuildPage(QWidget* parent = nullptr);
    ~BuildPage() override;
    void setGame(const QString& gameId) override;
    void extractImage(const QString& path);  // fill in the image and start, as if Extract was clicked
    bool busy() const { return step_ != Step::Idle; }  // extracting a disc or building the game
    // the game was built from another release than the package's project now holds (an update came): it must be
    // built again before it plays online (netplay refuses two different releases)
    static bool needsRebuild(const QString& gameId);
    void rebuild();  // as if Build was clicked

signals:
    void busyChanged(bool busy);

private:
    enum class Step { Idle, Reading, Extracting, Building };
    struct WorkResult {
        bool ok = false;
        QString error;
        QString sha1;  // of the disc's default.xex
    };
    void refreshInfo();
    void startExtract();
    void onDiscRead(const std::shared_ptr<xiso::Image>& disc, const WorkResult& r);
    void onExtracted(const WorkResult& r);
    void runWorker(std::function<void()> work, std::function<void()> done);
    void finishStep();
    void updateProgress();
    // the build script from Settings (a source build: mk9\build.cmd release), its output streamed into the log
    void runBuild();
    void setBusy(bool busy);

    QLabel* gameTitle_;
    QLabel* folderInfo_;
    QLabel* dataInfo_;
    QLabel* exeInfo_;
    QLabel* scriptInfo_;
    QLineEdit* image_;
    QPushButton* browse_;
    QPushButton* extract_;
    QPushButton* build_;
    QPushButton* cancel_;
    QProgressBar* progress_;
    LogView* log_;
    QThread* worker_ = nullptr;
    QPointer<QProcess> builder_;
    QByteArray buildOut_;
    Step step_ = Step::Idle;
    std::atomic<bool> stop_{false};
    std::atomic<qint64> bytesDone_{0};
    std::atomic<int> filesDone_{0};
    qint64 totalBytes_ = 0;
    int totalFiles_ = 0;
    QTimer progressTimer_;
    QString outDir_;
    QString imagePath_;
};

class SettingsPage : public Page {
    Q_OBJECT
public:
    explicit SettingsPage(ServerClient* server, QWidget* parent = nullptr);
    void setGame(const QString& gameId) override;
    void setUpnp(UpnpMapper* upnp);  // the router's state on the page; the game port's changes go to it

signals:
    void reconnectRequested();

private:
    void reloadServer();
    ServerClient* server_;
    QLineEdit* address_;
    QLineEdit* fingerprint_;
    QCheckBox* autoConnect_;
    QLabel* serverState_;
    QLabel* presented_;
    QLabel* routerState_;
    UpnpMapper* upnp_ = nullptr;
};
