#include "ToolPages.h"

#include <QCheckBox>
#include <QCryptographicHash>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QStorageInfo>
#include <QThread>
#include <QVBoxLayout>

#include <windows.h>

#include "AppPaths.h"
#include "GameData.h"
#include "GameProcess.h"
#include "ServerClient.h"
#include "Upnp.h"

namespace {
QString native(const QString& p) { return QDir::toNativeSeparators(p); }

QString humanBytes(qint64 b) {
    if (b >= (qint64(1) << 30)) return QStringLiteral("%1 GB").arg(double(b) / double(qint64(1) << 30), 0, 'f', 2);
    if (b >= (1 << 20)) return QStringLiteral("%1 MB").arg(double(b) / double(1 << 20), 0, 'f', 1);
    return QStringLiteral("%1 KB").arg(b / 1024);
}

QWidget* pathRow(QLineEdit* edit, QPushButton* browse) {
    auto* w = new QWidget;
    auto* h = new QHBoxLayout(w);
    h->setContentsMargins(0, 0, 0, 0);
    h->addWidget(edit, 1);
    h->addWidget(browse);
    return w;
}
}  // namespace

// --- BuildPage -------------------------------------------------------------------------------------

BuildPage::BuildPage(QWidget* parent) : Page(parent) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(28, 20, 28, 16);
    root->setSpacing(14);
    root->addWidget(ui::pageHeader(tr("Build game"),
                                   tr("The game is built on your PC from your own disc: the disc image is extracted "
                                      "into the game's data folder, then the build compiles the game for this PC. "
                                      "Nothing from the game is downloaded.")));
    auto* columns = new QHBoxLayout;
    columns->setSpacing(16);
    root->addLayout(columns);

    QVBoxLayout* info = nullptr;
    columns->addWidget(ui::card(tr("Game"), &info), 1);
    gameTitle_ = ui::label(QString(), "Big");
    folderInfo_ = ui::label(QString(), "Muted");
    dataInfo_ = ui::label(QString(), "Muted");
    exeInfo_ = ui::label(QString(), "Muted");
    scriptInfo_ = ui::label(QString(), "Muted");
    for (QLabel* l : {folderInfo_, dataInfo_, exeInfo_, scriptInfo_}) {
        l->setWordWrap(true);
        l->setTextInteractionFlags(Qt::TextSelectableByMouse);
    }
    info->addWidget(gameTitle_);
    info->addWidget(folderInfo_);
    info->addWidget(dataInfo_);
    info->addWidget(exeInfo_);
    info->addWidget(scriptInfo_);
    info->addStretch();

    QVBoxLayout* steps = nullptr;
    columns->addWidget(ui::card(tr("Steps"), &steps), 1);
    steps->addWidget(ui::label(tr("1. Your Xbox 360 disc image (.iso)"), "Muted"));
    image_ = new QLineEdit;
    image_->setPlaceholderText(tr("path to your disc image"));
    browse_ = new QPushButton(tr("Browse..."));
    steps->addWidget(pathRow(image_, browse_));
    extract_ = new QPushButton(tr("EXTRACT AND BUILD"));
    extract_->setObjectName(QStringLiteral("Primary"));
    steps->addWidget(extract_);
    auto* buildNote = ui::label(tr("2. Then the build compiles the game for this PC; its output shows below. Again, "
                                   "by hand:"), "Muted");
    buildNote->setWordWrap(true);
    steps->addWidget(buildNote);
    build_ = new QPushButton(tr("Build the game"));
    steps->addWidget(build_);
    cancel_ = new QPushButton(tr("Cancel"));
    cancel_->setEnabled(false);
    steps->addWidget(cancel_, 0, Qt::AlignRight);
    steps->addStretch();

    progress_ = new QProgressBar;
    progress_->setRange(0, 1000);
    progress_->setValue(0);
    progress_->setFormat(QStringLiteral("%p%"));
    root->addWidget(progress_);
    log_ = new LogView;
    log_->setMaximumBlockCount(5000);  // a build says a lot
    root->addWidget(log_, 1);

    connect(browse_, &QPushButton::clicked, this, [this] {
        const QString f = QFileDialog::getOpenFileName(this, tr("Disc image"), image_->text(),
                                                       tr("Xbox 360 disc images (*.iso);;All files (*)"));
        if (!f.isEmpty()) {
            image_->setText(native(f));
            cfg::set(QStringLiteral("games/%1/image").arg(gameId_), f);
        }
    });
    connect(extract_, &QPushButton::clicked, this, &BuildPage::startExtract);
    connect(build_, &QPushButton::clicked, this, &BuildPage::runBuild);
    connect(cancel_, &QPushButton::clicked, this, [this] {
        stop_ = true;
        if (builder_ && builder_->state() != QProcess::NotRunning) {
            // the script's own programs too (cmd, cmake, ninja, the compiler): the whole tree
            QProcess::execute(QStringLiteral("taskkill"),
                              {QStringLiteral("/T"), QStringLiteral("/F"), QStringLiteral("/PID"),
                               QString::number(builder_->processId())});
        }
    });
    progressTimer_.setInterval(500);
    connect(&progressTimer_, &QTimer::timeout, this, &BuildPage::updateProgress);
}

BuildPage::~BuildPage() {
    stop_ = true;
    if (worker_) worker_->wait();
    if (builder_ && builder_->state() != QProcess::NotRunning) builder_->kill();
}

void BuildPage::extractImage(const QString& path) {
    image_->setText(native(path));
    startExtract();
}

void BuildPage::setGame(const QString& gameId) {
    if (step_ != Step::Idle) return;  // keep the running build's game
    Page::setGame(gameId);
    image_->setText(native(cfg::get(QStringLiteral("games/%1/image").arg(gameId)).toString()));
    refreshInfo();
}

void BuildPage::refreshInfo() {
    const Game* g = GameData::instance().game(gameId_);
    if (!g) return;
    gameTitle_->setText(g->title);
    folderInfo_->setText(tr("%1 (%2), title ID %3").arg(g->title, g->edition, g->titleId));
    const QString disc = cfg::gameDataRoot(gameId_);
    dataInfo_->setText(GameProcess::hasGameFiles(disc) ? tr("Disc files: %1 (default.xex present)").arg(native(disc))
                                                       : tr("Disc files: %1 (not extracted yet)").arg(native(disc)));
    const QString exe = cfg::gameExe(gameId_);
    exeInfo_->setText(QFileInfo::exists(exe) ? tr("Game: %1").arg(native(exe))
                                             : tr("Game: %1 (not built yet)").arg(native(exe)));
    const QString script = cfg::buildScript(gameId_);
    const QString args = cfg::buildArgs(gameId_);
    const QString line = args.isEmpty() ? native(script) : native(script) + QLatin1Char(' ') + args;
    scriptInfo_->setText(QFileInfo::exists(script) ? tr("Build: %1").arg(line)
                                                   : tr("Build: %1 (not there yet)").arg(line));
}

bool BuildPage::needsRebuild(const QString& gameId) {
    const QDir folder(cfg::gameFolder(gameId));
    QFile want(folder.filePath(QStringLiteral("project/VERSION")));
    if (!QFileInfo::exists(cfg::gameExe(gameId)) || !want.open(QIODevice::ReadOnly)) return false;
    QFile built(folder.filePath(QStringLiteral("built.txt")));
    const QByteArray w = want.readAll().trimmed();
    return !built.open(QIODevice::ReadOnly) || built.readAll().trimmed() != w;
}

void BuildPage::rebuild() {
    if (step_ == Step::Idle) runBuild();
}

void BuildPage::setBusy(bool busy) {
    extract_->setEnabled(!busy);
    build_->setEnabled(!busy);
    browse_->setEnabled(!busy);
    image_->setEnabled(!busy);
    cancel_->setEnabled(busy);
    emit busyChanged(busy);
}

void BuildPage::runWorker(std::function<void()> work, std::function<void()> done) {
    worker_ = QThread::create(std::move(work));
    worker_->setParent(this);
    connect(worker_, &QThread::finished, this, [this, done] {
        worker_->deleteLater();
        worker_ = nullptr;
        done();
    });
    worker_->start();
}

void BuildPage::finishStep() {
    step_ = Step::Idle;
    setBusy(false);
}

void BuildPage::startExtract() {
    if (step_ != Step::Idle) return;
    imagePath_ = QDir::fromNativeSeparators(image_->text().trimmed());
    if (!QFileInfo(imagePath_).isFile()) {
        log_->line(tr("Choose your disc image first."));
        return;
    }
    cfg::set(QStringLiteral("games/%1/image").arg(gameId_), imagePath_);
    outDir_ = cfg::gameDataRoot(gameId_);
    progress_->setValue(0);
    progress_->setFormat(QStringLiteral("%p%"));
    log_->line(tr("Reading the disc's file table: %1").arg(native(imagePath_)));
    applog::write(QStringLiteral("build %1: reading %2").arg(gameId_, native(imagePath_)));
    step_ = Step::Reading;
    stop_ = false;
    setBusy(true);
    auto disc = std::make_shared<xiso::Image>();
    auto result = std::make_shared<WorkResult>();
    runWorker(
        [disc, result, path = imagePath_] {
            if (!disc->open(path, &result->error)) return;
            const xiso::Entry* xex = disc->find(QStringLiteral("default.xex"));
            if (!xex) {
                result->error = disc->find(QStringLiteral("default.xbe"))
                                    ? tr("This is an original Xbox disc (default.xbe), not the Xbox 360 game.")
                                    : tr("The disc has no default.xex: this is not an Xbox 360 game disc.");
                return;
            }
            QByteArray data;
            if (!disc->read(*xex, &data, &result->error)) return;
            result->sha1 = QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha1).toHex());
            result->ok = true;
        },
        [this, disc, result] { onDiscRead(disc, *result); });
}

void BuildPage::onDiscRead(const std::shared_ptr<xiso::Image>& disc, const WorkResult& r) {
    if (!r.ok) {
        log_->line(r.error);
        applog::write(QStringLiteral("build %1: %2").arg(gameId_, r.error));
        finishStep();
        return;
    }
    totalBytes_ = disc->totalBytes();
    totalFiles_ = disc->fileCount();
    log_->line(tr("Xbox game file system at offset 0x%1 (%2); %3 files, %4; default.xex SHA-1 %5.")
                   .arg(QString::number(disc->base(), 16).toUpper(), disc->layout())
                   .arg(totalFiles_).arg(humanBytes(totalBytes_), r.sha1));
    applog::write(QStringLiteral("build %1: file system at 0x%2 (%3), %4 files, %5 bytes, default.xex SHA-1 %6")
                      .arg(gameId_, QString::number(disc->base(), 16).toUpper(), disc->layout())
                      .arg(totalFiles_).arg(totalBytes_).arg(r.sha1));
    const Game* g = GameData::instance().game(gameId_);
    if (g && !g->discSha1.isEmpty() && r.sha1.compare(g->discSha1, Qt::CaseInsensitive) != 0) {
        log_->line(tr("This is not the release this build is made for (%1, %2: default.xex SHA-1 %3).")
                       .arg(g->title, g->edition, g->discSha1));
        applog::write(QStringLiteral("build %1: another release, default.xex SHA-1 %2").arg(gameId_, r.sha1));
        const auto answer = QMessageBox::warning(
            this, tr("Different release"),
            tr("This disc is not %1 (%2), the release this build of the game is made for. The game will most "
               "likely crash with it.\n\nExtract it anyway?").arg(g->title, g->edition),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes) {
            finishStep();
            return;
        }
    }
    QDir().mkpath(outDir_);
    const qint64 freeBytes = QStorageInfo(outDir_).bytesAvailable();
    const qint64 needed = totalBytes_ + (qint64(512) << 20);  // the files, and a margin for saves and caches
    if (freeBytes >= 0 && freeBytes < needed) {
        log_->line(tr("Not enough free space on that drive: %1 needed, %2 free.").arg(humanBytes(needed), humanBytes(freeBytes)));
        finishStep();
        return;
    }
    log_->line(tr("Extracting into %1 ... (files already there in full are kept)").arg(native(outDir_)));
    step_ = Step::Extracting;
    bytesDone_ = 0;
    filesDone_ = 0;
    progressTimer_.start();
    auto result = std::make_shared<WorkResult>();
    runWorker([this, disc, result, out = outDir_] {
        result->ok = disc->extractAll(out, stop_, bytesDone_, filesDone_, &result->error);
    }, [this, result] { onExtracted(*result); });
}

void BuildPage::onExtracted(const WorkResult& r) {
    progressTimer_.stop();
    finishStep();
    if (!r.ok) {
        log_->line(tr("Extraction stopped: %1").arg(r.error));
        applog::write(QStringLiteral("build %1: extraction stopped: %2").arg(gameId_, r.error));
        refreshInfo();
        return;
    }
    progress_->setValue(1000);
    progress_->setFormat(tr("Extracted  -  %1 files, %2").arg(totalFiles_).arg(humanBytes(totalBytes_)));
    log_->line(tr("Extraction finished."));
    applog::write(QStringLiteral("build %1: extracted %2 files into %3").arg(gameId_).arg(totalFiles_).arg(native(outDir_)));
    refreshInfo();
    runBuild();
}

void BuildPage::updateProgress() {
    if (step_ != Step::Extracting || totalBytes_ <= 0) return;
    const qint64 done = bytesDone_;
    progress_->setValue(static_cast<int>(qMin<qint64>(999, done * 1000 / totalBytes_)));
    progress_->setFormat(tr("%p%  -  %1 of %2 files, %3 of %4").arg(filesDone_.load()).arg(totalFiles_)
                             .arg(humanBytes(done), humanBytes(totalBytes_)));
}

void BuildPage::runBuild() {
    if (builder_ || step_ != Step::Idle) return;
    const QString script = cfg::buildScript(gameId_);
    const QString args = cfg::buildArgs(gameId_);
    if (!QFileInfo::exists(script)) {
        const QString text =
            paths::isPackage()
                ? tr("The build script %1 is not in this package yet: a later release of the player package brings "
                     "it. The disc is extracted; the game itself comes with that release.").arg(native(script))
                : tr("The build script %1 is not there. Set it in Settings (Game, Build script).").arg(native(script));
        log_->line(text);
        applog::write(QStringLiteral("build %1: no build script %2").arg(gameId_, native(script)));
        refreshInfo();
        return;
    }
    if (!GameProcess::hasGameFiles(cfg::gameDataRoot(gameId_)))
        log_->line(tr("Warning: the disc is not extracted yet (no default.xex in %1).").arg(native(cfg::gameDataRoot(gameId_))));
    auto* p = new QProcess(this);
    builder_ = p;
    buildOut_.clear();
    p->setWorkingDirectory(QFileInfo(script).absolutePath());
    p->setProcessChannelMode(QProcess::MergedChannels);
    p->setProgram(QStringLiteral("cmd.exe"));
    // cmd /s /c ""<script>" <args>": the outer quotes go, the script's stay (a path with spaces)
    p->setNativeArguments(QStringLiteral("/d /s /c \"\"%1\" %2\"").arg(native(script), args));
    p->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* a) { a->flags |= CREATE_NO_WINDOW; });
    connect(p, &QProcess::readyRead, this, [this, p] {
        buildOut_.append(p->readAll());
        qsizetype nl;
        while ((nl = buildOut_.indexOf('\n')) >= 0) {
            const QString line = QString::fromLocal8Bit(buildOut_.left(nl)).trimmed();
            buildOut_.remove(0, nl + 1);
            if (!line.isEmpty()) log_->line(line);
        }
    });
    connect(p, &QProcess::finished, this, [this, p](int code, QProcess::ExitStatus st) {
        if (!buildOut_.trimmed().isEmpty()) log_->line(QString::fromLocal8Bit(buildOut_).trimmed());
        buildOut_.clear();
        const bool ok = st == QProcess::NormalExit && code == 0;
        log_->line(stop_ ? tr("Build cancelled.")
                         : ok ? tr("Build finished: the game is ready (Single player, or search for a match).")
                              : tr("The build failed (exit code %1): the lines above say why.").arg(code));
        applog::write(QStringLiteral("build %1: script exited with code %2").arg(gameId_).arg(code));
        progress_->setRange(0, 1000);
        progress_->setValue(ok ? 1000 : 0);
        progress_->setFormat(ok ? tr("Built") : tr("Not built"));
        p->deleteLater();
        builder_ = nullptr;
        finishStep();
        refreshInfo();
    });
    stop_ = false;
    step_ = Step::Building;
    setBusy(true);
    progress_->setRange(0, 0);  // busy, no measure
    log_->line(tr("Building: %1 %2").arg(native(script), args));
    applog::write(QStringLiteral("build %1: running %2 %3").arg(gameId_, native(script), args));
    p->start();
    if (!p->waitForStarted(10000)) {
        log_->line(tr("Could not run %1: %2").arg(native(script), p->errorString()));
        p->deleteLater();
        builder_ = nullptr;
        progress_->setRange(0, 1000);
        finishStep();
    }
}

// --- SettingsPage ----------------------------------------------------------------------------------

SettingsPage::SettingsPage(ServerClient* server, QWidget* parent) : Page(parent), server_(server) {
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    outer->addWidget(scroll);
    auto* content = new QWidget;
    scroll->setWidget(content);
    auto* root = new QVBoxLayout(content);
    root->setContentsMargins(28, 20, 28, 20);
    root->setSpacing(10);
    root->addWidget(ui::pageHeader(tr("Settings"), tr("Saved to %1").arg(native(paths::configFile()))));
    auto* grid = new QGridLayout;
    grid->setHorizontalSpacing(16);
    grid->setVerticalSpacing(4);
    root->addLayout(grid);

    // server
    auto* serverBox = new QGroupBox(tr("SERVER"));
    auto* sf = new QFormLayout(serverBox);
    address_ = new QLineEdit;
    address_->setPlaceholderText(QStringLiteral("host:7777"));
    fingerprint_ = new QLineEdit;
    fingerprint_->setPlaceholderText(tr("SHA-256 of the server certificate (the server's host gives it out)"));
    autoConnect_ = new QCheckBox(tr("Connect when the launcher starts"));
    autoConnect_->setChecked(cfg::get(QStringLiteral("server/auto_connect"), true).toBool());
    auto* connectBtn = new QPushButton(tr("Connect"));
    auto* disconnectBtn = new QPushButton(tr("Disconnect"));
    auto* pinBtn = new QPushButton(tr("Pin presented certificate"));
    auto* btnRow = new QWidget;
    auto* br = new QHBoxLayout(btnRow);
    br->setContentsMargins(0, 0, 0, 0);
    br->addWidget(connectBtn);
    br->addWidget(disconnectBtn);
    br->addWidget(pinBtn);
    br->addStretch();
    serverState_ = ui::label(QString(), "Muted");
    presented_ = ui::label(QString(), "Muted");
    presented_->setWordWrap(true);
    presented_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    sf->addRow(tr("Address"), address_);
    sf->addRow(tr("Fingerprint"), fingerprint_);
    sf->addRow(QString(), autoConnect_);
    sf->addRow(QString(), btnRow);
    sf->addRow(tr("State"), serverState_);
    sf->addRow(tr("Presented"), presented_);
    grid->addWidget(serverBox, 0, 0, 1, 2);

    // online
    auto* onlineBox = new QGroupBox(tr("ONLINE"));
    auto* of = new QFormLayout(onlineBox);
    auto* region = new QComboBox;
    ui::fillRegions(region, cfg::region());
    auto* maxPing = new QComboBox;
    ui::fillMaxPing(maxPing, cfg::maxPing());
    auto* delay = new QComboBox;
    delay->addItem(tr("Auto (from the ping, default)"), QStringLiteral("auto"));
    for (int f = 0; f <= 6; ++f) delay->addItem(tr("%n frame(s)", nullptr, f), f);
    delay->setCurrentIndex(cfg::inputDelay() < 0 ? 0 : qMax(0, delay->findData(cfg::inputDelay())));
    delay->setToolTip(tr("Frames between a press and the game using it. Lockstep waits at every frame whose input is "
                         "still on its way, so the delay has to cover the one-way latency: Auto takes it from the ping "
                         "measured when the match connects, (ping / 2 + 8 ms) / 16.7 ms, 2 to 8 frames. A match "
                         "takes the larger delay of the two players."));
    auto* netcode = new QComboBox;
    netcode->addItem(tr("Lockstep (default)"), QStringLiteral("lockstep"));
    netcode->addItem(tr("Rollback (experimental)"), QStringLiteral("rollback"));
    netcode->setCurrentIndex(qMax(0, netcode->findData(cfg::netcode())));
    netcode->setToolTip(tr("Lockstep waits for the other player's input before each frame. Rollback runs ahead on a "
                           "guess and corrects it: smoother over a long distance, still experimental. A match plays "
                           "rollback only when both players chose it."));
    auto* port = new QSpinBox;
    port->setRange(1024, 65535);
    port->setValue(cfg::gamePort());
    auto* strict = new QCheckBox(tr("Strict ranked matchmaking by default"));
    strict->setChecked(cfg::strict());
    auto* upnp = new QCheckBox(tr("Open the game port on the router (UPnP)"));
    upnp->setChecked(cfg::upnp());
    routerState_ = ui::label(tr("Router: not asked"), "Muted");
    routerState_->setWordWrap(true);
    of->addRow(tr("Region"), region);
    of->addRow(tr("Max ping"), maxPing);
    of->addRow(tr("Input delay"), delay);
    of->addRow(tr("Netcode"), netcode);
    of->addRow(tr("Game UDP port"), port);
    of->addRow(QString(), strict);
    of->addRow(QString(), upnp);
    of->addRow(tr("Router"), routerState_);
    auto* delayNote = ui::label(tr("A match takes the larger delay of the two players (Auto: from the ping, 2 to 8 "
                                   "frames; Ranked at least 1). Players who cannot reach each other directly play "
                                   "through the server, with a little more delay."), "Muted");
    delayNote->setWordWrap(true);
    of->addRow(QString(), delayNote);
    grid->addWidget(onlineBox, 1, 0);

    // display
    auto* displayBox = new QGroupBox(tr("DISPLAY"));
    auto* df = new QFormLayout(displayBox);
    auto* window = new QComboBox;
    window->addItem(tr("Fullscreen"), true);
    window->addItem(tr("Window"), false);
    window->setCurrentIndex(cfg::fullscreen() ? 0 : 1);
    df->addRow(tr("Game window"), window);
    auto* displayNote = ui::label(tr("The game takes it when it starts (its --fullscreen / --no-fullscreen). The two "
                                     "games of a local test always open in windows."), "Muted");
    displayNote->setWordWrap(true);
    df->addRow(QString(), displayNote);
    grid->addWidget(displayBox, 1, 1);

    // the game's files
    auto* gamesBox = new QGroupBox(tr("GAME"));
    auto* gf = new QFormLayout(gamesBox);
    for (const Game& g : GameData::instance().games()) {
        const QString id = g.id;
        struct PathSetting {
            QString key, label, value;
            bool folder;
        };
        const QList<PathSetting> rows = {
            {QStringLiteral("exe"), tr("Executable"), cfg::gameExe(id), false},
            {QStringLiteral("data"), tr("Disc files"), cfg::gameDataRoot(id), true},
            {QStringLiteral("save"), tr("Save folder"), cfg::saveFolder(id), true},
            {QStringLiteral("build_script"), tr("Build script"), cfg::buildScript(id), false},
        };
        gf->addRow(ui::label(QStringLiteral("%1 (%2)").arg(g.title, g.edition), "CardTitle"));
        for (const PathSetting& r : rows) {
            auto* edit = new QLineEdit(native(r.value));
            auto* browse = new QPushButton(tr("..."));
            gf->addRow(r.label, pathRow(edit, browse));
            const QString key = QStringLiteral("games/%1/%2").arg(id, r.key);
            connect(edit, &QLineEdit::editingFinished, this, [key, edit] {
                cfg::set(key, QDir::fromNativeSeparators(edit->text().trimmed()));
            });
            connect(browse, &QPushButton::clicked, this, [this, key, edit, r] {
                const QString f = r.folder ? QFileDialog::getExistingDirectory(this, r.label, edit->text())
                                           : QFileDialog::getOpenFileName(this, r.label, edit->text(),
                                                                          tr("Programs (*.exe *.cmd *.bat *.py);;All files (*)"));
                if (f.isEmpty()) return;
                edit->setText(native(f));
                cfg::set(key, f);
            });
        }
        auto* args = new QLineEdit(cfg::buildArgs(id));
        args->setPlaceholderText(tr("its arguments, for example release"));
        gf->addRow(tr("Build arguments"), args);
        connect(args, &QLineEdit::editingFinished, this,
                [id, args] { cfg::set(QStringLiteral("games/%1/build_args").arg(id), args->text().trimmed()); });
    }
    auto* gamesNote = ui::label(tr("Single player plays with the save folder; every online match gets a fresh save of "
                                   "its own in its session folder (the menus the launcher steers are a fresh save's)."),
                                "Muted");
    gamesNote->setWordWrap(true);
    gf->addRow(QString(), gamesNote);
    grid->addWidget(gamesBox, 2, 0, 1, 2);

    // tools and development
    auto* toolsBox = new QGroupBox(tr("TOOLS AND DEVELOPMENT"));
    auto* tf = new QFormLayout(toolsBox);
    auto* python = new QLineEdit(cfg::python());
    auto* repo = new QLineEdit(native(cfg::repoRoot()));
    auto* lp1 = new QSpinBox;
    lp1->setRange(1024, 65535);
    lp1->setValue(cfg::localTestPort(1));
    auto* lp2 = new QSpinBox;
    lp2->setRange(1024, 65535);
    lp2->setValue(cfg::localTestPort(2));
    tf->addRow(tr("Python"), python);
    tf->addRow(tr("Repository"), repo);
    tf->addRow(tr("Local test ports"), [&] {
        auto* w = new QWidget;
        auto* h = new QHBoxLayout(w);
        h->setContentsMargins(0, 0, 0, 0);
        h->addWidget(lp1);
        h->addWidget(lp2);
        h->addStretch();
        return w;
    }());
    grid->addWidget(toolsBox, 3, 0, 1, 2);
    toolsBox->setVisible(!paths::isPackage());  // a player package has no repository or Python to point at
    root->addStretch();

    connect(region, &QComboBox::currentIndexChanged, this, [region] { cfg::set(QStringLiteral("online/region"), region->currentData()); });
    connect(maxPing, &QComboBox::currentIndexChanged, this, [maxPing] { cfg::set(QStringLiteral("online/max_ping"), maxPing->currentData()); });
    connect(delay, &QComboBox::currentIndexChanged, this, [delay] { cfg::set(QStringLiteral("online/delay"), delay->currentData()); });
    connect(netcode, &QComboBox::currentIndexChanged, this,
            [netcode] { cfg::set(QStringLiteral("online/netcode"), netcode->currentData()); });
    connect(port, &QSpinBox::valueChanged, this, [this](int v) {
        cfg::set(QStringLiteral("online/game_port"), v);
        if (upnp_ && cfg::upnp()) upnp_->map(static_cast<quint16>(v));
    });
    connect(upnp, &QCheckBox::toggled, this, [this](bool on) {
        cfg::set(QStringLiteral("online/upnp"), on);
        if (on && upnp_) upnp_->map(static_cast<quint16>(cfg::gamePort()));
    });
    connect(strict, &QCheckBox::toggled, this, [](bool on) { cfg::set(QStringLiteral("online/strict"), on); });
    connect(window, &QComboBox::currentIndexChanged, this,
            [window] { cfg::set(QStringLiteral("display/fullscreen"), window->currentData()); });
    connect(python, &QLineEdit::editingFinished, this, [python] { cfg::set(QStringLiteral("tools/python"), python->text().trimmed()); });
    connect(repo, &QLineEdit::editingFinished, this, [repo] {
        cfg::set(QStringLiteral("tools/repo_root"), QDir::fromNativeSeparators(repo->text().trimmed()));
    });
    connect(lp1, &QSpinBox::valueChanged, this, [](int v) { cfg::set(QStringLiteral("dev/local_port1"), v); });
    connect(lp2, &QSpinBox::valueChanged, this, [](int v) { cfg::set(QStringLiteral("dev/local_port2"), v); });
    connect(autoConnect_, &QCheckBox::toggled, this, [](bool on) { cfg::set(QStringLiteral("server/auto_connect"), on); });
    connect(connectBtn, &QPushButton::clicked, this, [this] {
        cfg::set(QStringLiteral("server/address"), address_->text().trimmed());
        cfg::set(QStringLiteral("server/fingerprint"), ServerClient::normalizeFingerprint(fingerprint_->text()));
        emit reconnectRequested();
    });
    connect(disconnectBtn, &QPushButton::clicked, server_, &ServerClient::disconnectFromServer);
    connect(pinBtn, &QPushButton::clicked, this, [this] {
        if (server_->peerFingerprint().isEmpty()) return;
        fingerprint_->setText(ServerClient::prettyFingerprint(server_->peerFingerprint()));
    });
    connect(server_, &ServerClient::stateChanged, this, [this] { reloadServer(); });
    reloadServer();
}

void SettingsPage::setGame(const QString& gameId) { Page::setGame(gameId); }

void SettingsPage::setUpnp(UpnpMapper* upnp) {
    upnp_ = upnp;
    const auto show = [this](const QString& text) {
        routerState_->setText(text.isEmpty() ? tr("not asked (UPnP is off)") : text);
    };
    connect(upnp_, &UpnpMapper::statusChanged, this, show);
    show(upnp_->status());
}

void SettingsPage::reloadServer() {
    if (!address_->hasFocus()) address_->setText(cfg::serverAddress());
    if (!fingerprint_->hasFocus()) fingerprint_->setText(ServerClient::prettyFingerprint(cfg::fingerprint()));
    static const char* names[] = {"offline", "connecting", "connected", "signed in"};
    serverState_->setText(QString::fromLatin1(names[static_cast<int>(server_->state())]));
    presented_->setText(server_->peerFingerprint().isEmpty() ? tr("(none yet)")
                                                             : ServerClient::prettyFingerprint(server_->peerFingerprint()));
}
