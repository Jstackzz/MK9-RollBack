#include "LobbyPage.h"

#include <QComboBox>
#include <QDateTime>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QStackedWidget>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

#include "AppPaths.h"
#include "Dialogs.h"
#include "GameData.h"
#include "MatchController.h"
#include "ServerClient.h"
#include "Style.h"

namespace {
QString s(const QJsonObject& o, const char* k) { return o.value(QLatin1String(k)).toString(); }

QTableWidget* makeTable(const QStringList& headers) {
    auto* t = new QTableWidget(0, static_cast<int>(headers.size()));
    t->setHorizontalHeaderLabels(headers);
    t->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    t->verticalHeader()->hide();
    t->setEditTriggers(QAbstractItemView::NoEditTriggers);
    t->setSelectionBehavior(QAbstractItemView::SelectRows);
    t->setSelectionMode(QAbstractItemView::SingleSelection);
    t->setAlternatingRowColors(true);
    return t;
}
}  // namespace

LobbyPage::LobbyPage(ServerClient* server, MatchController* controller, QWidget* parent)
    : Page(parent), server_(server), controller_(controller) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(28, 20, 28, 16);
    root->setSpacing(14);
    root->addWidget(ui::pageHeader(tr("Lobbies"), tr("Private or public rooms for two players and their spectators, or "
                                                     "King of the Hill for up to eight: chat, ready up, and the host "
                                                     "starts; fighters are picked on the game's select screen, and "
                                                     "spectators watch each match in their own game. Lobby matches "
                                                     "count as Kasual.")));
    auto* columns = new QHBoxLayout;
    columns->setSpacing(16);
    root->addLayout(columns, 1);

    QVBoxLayout* left = nullptr;
    columns->addWidget(ui::card(tr("Open lobbies"), &left), 1);
    auto* bar = new QHBoxLayout;
    auto* refreshBtn = new QPushButton(tr("Refresh"));
    auto* create = new QPushButton(tr("Create lobby"));
    code_ = new QLineEdit;
    code_->setPlaceholderText(tr("code"));
    code_->setMaxLength(5);
    code_->setMaximumWidth(90);
    auto* joinCode = new QPushButton(tr("Join code"));
    bar->addWidget(refreshBtn);
    bar->addWidget(create);
    bar->addStretch();
    bar->addWidget(code_);
    bar->addWidget(joinCode);
    left->addLayout(bar);
    list_ = makeTable({tr("Code"), tr("Name"), tr("Host"), tr("Mode"), tr("Region"), tr("Players"), tr("Lock")});
    list_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    list_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    left->addWidget(list_, 1);
    auto* joinRow = new QHBoxLayout;
    listInfo_ = ui::label(QString(), "Muted");
    listInfo_->setWordWrap(true);
    auto* joinSel = new QPushButton(tr("Join selected"));
    auto* watchSel = new QPushButton(tr("Watch selected"));
    watchSel->setToolTip(tr("Join as a spectator: each of the lobby's matches plays in your game, as its players play "
                            "it. A match under way is caught up from its start."));
    joinRow->addWidget(listInfo_, 1);
    joinRow->addWidget(watchSel);
    joinRow->addWidget(joinSel);
    left->addLayout(joinRow);

    QVBoxLayout* rightBody = nullptr;
    columns->addWidget(ui::card(tr("Your lobby"), &rightBody), 1);
    right_ = new QStackedWidget;
    rightBody->addWidget(right_, 1);
    auto* none = ui::label(tr("You are not in a lobby.\nCreate one, or join one from the list or by its code."), "Muted");
    none->setAlignment(Qt::AlignCenter);
    right_->addWidget(none);

    auto* panel = new QWidget;
    auto* pv = new QVBoxLayout(panel);
    pv->setContentsMargins(0, 0, 0, 0);
    lobbyTitle_ = ui::label(QString(), "Big");
    lobbyTitle_->setTextFormat(Qt::PlainText);  // the lobby's name is whatever its host typed: never markup
    lobbyInfo_ = ui::label(QString(), "Muted");
    lobbyInfo_->setWordWrap(true);
    pv->addWidget(lobbyTitle_);
    pv->addWidget(lobbyInfo_);
    members_ = makeTable({tr("Player"), tr("Place"), tr("Ready"), tr("Connection")});
    members_->setMaximumHeight(190);
    pv->addWidget(members_);
    auto* controls = new QHBoxLayout;
    character_ = new QComboBox;
    character_->setMinimumWidth(150);
    ready_ = new QPushButton(tr("Ready"));
    ready_->setCheckable(true);
    start_ = new QPushButton(tr("START"));
    start_->setObjectName(QStringLiteral("Primary"));
    // the server's countdown once the two to play are ready, shown here (no START then)
    countdown_ = ui::label(QString(), "Big");
    countdown_->setAlignment(Qt::AlignCenter);
    countdown_->hide();
    nextInfo_ = ui::label(QString(), "Muted");
    nextInfo_->setAlignment(Qt::AlignCenter);
    nextInfo_->setWordWrap(true);
    nextInfo_->hide();
    tick_ = new QTimer(this);
    tick_->setInterval(100);
    leave_ = new QPushButton(tr("Leave"));
    role_ = new QPushButton(tr("Watch"));
    leaveMatch_ = new QPushButton(tr("LEAVE MATCH"));
    leaveMatch_->setToolTip(tr("End your match in this lobby (it does not count) and go back to the menu"));
    leaveMatch_->hide();
    controls->addWidget(character_, 1);
    controls->addWidget(ready_);
    controls->addWidget(role_);
    controls->addWidget(leave_);
    pv->addLayout(controls);
    pv->addWidget(start_);
    pv->addWidget(countdown_);
    pv->addWidget(nextInfo_);
    pv->addWidget(leaveMatch_);
    chat_ = new LogView;
    pv->addWidget(chat_, 1);
    auto* chatRow = new QHBoxLayout;
    chatInput_ = new QLineEdit;
    chatInput_->setMaxLength(200);
    chatInput_->setPlaceholderText(tr("Say something..."));
    auto* send = new QPushButton(tr("Send"));
    chatRow->addWidget(chatInput_, 1);
    chatRow->addWidget(send);
    pv->addLayout(chatRow);
    right_->addWidget(panel);

    connect(refreshBtn, &QPushButton::clicked, this, &LobbyPage::refresh);
    connect(create, &QPushButton::clicked, this, &LobbyPage::createLobby);
    connect(joinCode, &QPushButton::clicked, this, [this] { join(code_->text().trimmed().toUpper(), false); });
    connect(joinSel, &QPushButton::clicked, this, [this] {
        const int row = list_->currentRow();
        if (row < 0) return;
        join(list_->item(row, 0)->text(), !list_->item(row, 6)->text().isEmpty());
    });
    connect(watchSel, &QPushButton::clicked, this, [this] {
        const int row = list_->currentRow();
        if (row < 0) return;
        join(list_->item(row, 0)->text(), !list_->item(row, 6)->text().isEmpty(), true);
    });
    connect(list_, &QTableWidget::cellDoubleClicked, this, [this](int row, int) {
        // a lobby whose two places are taken: watch it
        const bool full = list_->item(row, 5)->data(Qt::UserRole).toBool();
        join(list_->item(row, 0)->text(), !list_->item(row, 6)->text().isEmpty(), full);
    });
    connect(leave_, &QPushButton::clicked, this, [this] {
        controller_->stopWatching();
        server_->send({{QStringLiteral("type"), QStringLiteral("lobby_leave")}});
    });
    connect(role_, &QPushButton::clicked, this, [this] {
        const bool watchNow = !spectating();
        if (!watchNow) controller_->stopWatching();  // to play: the game is needed for it
        server_->send({{QStringLiteral("type"), QStringLiteral("lobby_role")},
                       {QStringLiteral("role"), watchNow ? QStringLiteral("spectator") : QStringLiteral("player")}});
    });
    connect(start_, &QPushButton::clicked, this, [this] { server_->send({{QStringLiteral("type"), QStringLiteral("lobby_start")}}); });
    connect(tick_, &QTimer::timeout, this, &LobbyPage::updateStatus);
    connect(leaveMatch_, &QPushButton::clicked, controller_, &MatchController::leaveMatch);
    connect(controller_, &MatchController::phaseChanged, this, &LobbyPage::updateLeaveMatch);
    connect(ready_, &QPushButton::clicked, this, [this](bool on) {
        if (on) {
            QString err;
            if (!controller_->prepareGame(s(lobby_, "game"), &err)) {
                ready_->setChecked(false);
                QMessageBox::warning(this, tr("Ready"), err);
                return;
            }
        }
        // with this PC's connection, for the members' table (and the address hidden, when the player asked)
        QJsonObject ready{{QStringLiteral("type"), QStringLiteral("lobby_ready")}, {QStringLiteral("ready"), on},
                          {QStringLiteral("delay"), cfg::inputDelayValue(cfg::inputDelay())},
                          {QStringLiteral("netcode"), cfg::netcode()}, {QStringLiteral("relay"), cfg::hideIp()},
                          {QStringLiteral("link"), server_->linkType()}};
        if (controller_->serverRttMs() >= 0) ready.insert(QStringLiteral("server_rtt"), controller_->serverRttMs());
        server_->send(ready);
    });
    connect(character_, &QComboBox::activated, this, [this] {
        const QString id = character_->currentData().toString();  // empty: picked on the game's select screen
        if (id.isEmpty()) cfg::set(QStringLiteral("online/pick_in_game"), true);
        else {
            cfg::set(QStringLiteral("online/pick_in_game"), false);
            cfg::set(QStringLiteral("ui/%1/%2/character").arg(s(lobby_, "game"), s(lobby_, "mode")), id);
        }
        server_->send({{QStringLiteral("type"), QStringLiteral("lobby_character")}, {QStringLiteral("character"), id}});
    });
    connect(send, &QPushButton::clicked, this, &LobbyPage::sendChat);
    connect(chatInput_, &QLineEdit::returnPressed, this, &LobbyPage::sendChat);
    connect(server_, &ServerClient::message, this, &LobbyPage::onMessage);
    // signed out or offline: the server took this player out of the lobby (a lobby is the session's), so the page
    // goes back to the list rather than showing a room it is no longer in. A dropped connection gets the place back
    // at the next login within the server's 60 s, so a spectator's game keeps watching meanwhile: from what it has,
    // then from the feed sent again
    rejoinGrace_ = new QTimer(this);
    rejoinGrace_->setSingleShot(true);
    rejoinGrace_->setInterval(70000);
    connect(rejoinGrace_, &QTimer::timeout, this, [this] {
        if (!inLobby()) controller_->stopWatching();
    });
    connect(server_, &ServerClient::stateChanged, this, [this](ServerClient::State st) {
        if (st == ServerClient::State::LoggedIn) {
            refresh();
        } else if (inLobby()) {
            const bool dropped = st == ServerClient::State::Offline && watchingHere_;
            leftLobby(dropped);
            if (dropped) rejoinGrace_->start();
        }
    });
}

void LobbyPage::pressReady() {
    if (!inLobby()) return;
    if (start_->isVisible() && start_->isEnabled()) start_->click();  // a server without the countdown: the host's
    else if (ready_->isVisible() && ready_->isEnabled()) ready_->click();
}

void LobbyPage::focusReady() {
    if (inLobby() && ready_->isVisible() && ready_->isEnabled()) ready_->setFocus(Qt::OtherFocusReason);
}

void LobbyPage::setGame(const QString& gameId) {
    Page::setGame(gameId);
    refresh();
}

void LobbyPage::showEvent(QShowEvent* event) {
    Page::showEvent(event);
    refresh();
}

void LobbyPage::refresh() {
    if (!server_->isLoggedIn()) {
        listInfo_->setText(tr("Sign in to see lobbies."));
        list_->setRowCount(0);
        return;
    }
    server_->send({{QStringLiteral("type"), QStringLiteral("lobby_list")}, {QStringLiteral("game"), gameId_}});
}

QString LobbyPage::savedCharacter(const QString& game, const QString& mode) const {
    const Game* g = GameData::instance().game(game);
    const GameMode* m = g ? g->mode(mode) : nullptr;
    // the game's own select screen: the mode's way (all of MK9's), or the player's choice
    if ((m && m->picksInGame()) || cfg::pickInGame()) return QString();
    QString id = cfg::get(QStringLiteral("ui/%1/%2/character").arg(game, mode)).toString();
    if (id.isEmpty() && g) {
        const auto roster = g->roster(mode);
        if (!roster.isEmpty()) id = roster.first().id;
    }
    return id;
}

void LobbyPage::createLobby() {
    const Game* g = GameData::instance().game(gameId_);
    if (!g || !server_->isLoggedIn()) {
        QMessageBox::information(this, tr("Lobby"), tr("Sign in first."));
        return;
    }
    CreateLobbyDialog dlg(*g, tr("%1's lobby").arg(server_->accountName()), this);
    if (dlg.exec() != QDialog::Accepted) return;
    QJsonObject msg{{QStringLiteral("type"), QStringLiteral("lobby_create")},
                    {QStringLiteral("name"), dlg.name()},
                    {QStringLiteral("game"), gameId_},
                    {QStringLiteral("mode"), dlg.mode()},
                    {QStringLiteral("password"), dlg.password()},
                    {QStringLiteral("rounds"), dlg.rounds()},
                    {QStringLiteral("region"), cfg::region()},
                    {QStringLiteral("koth"), dlg.kingOfTheHill()},
                    {QStringLiteral("character"), savedCharacter(gameId_, dlg.mode())},
                    {QStringLiteral("link"), server_->linkType()}};
    if (controller_->serverRttMs() >= 0) msg.insert(QStringLiteral("server_rtt"), controller_->serverRttMs());
    server_->send(msg);
}

bool LobbyPage::spectating() const {
    for (const auto& mv : lobby_.value(QStringLiteral("members")).toArray()) {
        const QJsonObject mem = mv.toObject();
        if (s(mem, "name") == server_->accountName()) return mem.value(QStringLiteral("spectator")).toBool();
    }
    return false;
}

void LobbyPage::join(const QString& code, bool locked, bool spectate) {
    if (code.size() != 5) {
        listInfo_->setText(tr("Lobby codes have 5 characters."));
        return;
    }
    QString password;
    if (locked) {
        bool ok = false;
        password = QInputDialog::getText(this, tr("Lobby %1").arg(code), tr("Password"), QLineEdit::Password, QString(), &ok);
        if (!ok) return;
    }
    QString game = gameId_, mode = QStringLiteral("versus");
    for (int r = 0; r < list_->rowCount(); ++r)
        if (list_->item(r, 0)->text() == code) mode = list_->item(r, 3)->data(Qt::UserRole).toString();
    QJsonObject msg{{QStringLiteral("type"), QStringLiteral("lobby_join")}, {QStringLiteral("lobby"), code},
                    {QStringLiteral("password"), password}, {QStringLiteral("character"), savedCharacter(game, mode)},
                    {QStringLiteral("spectate"), spectate}, {QStringLiteral("link"), server_->linkType()}};
    if (controller_->serverRttMs() >= 0) msg.insert(QStringLiteral("server_rtt"), controller_->serverRttMs());
    server_->send(msg);
}

void LobbyPage::sendChat() {
    const QString text = chatInput_->text().trimmed();
    if (text.isEmpty() || lobby_.isEmpty()) return;
    server_->send({{QStringLiteral("type"), QStringLiteral("lobby_chat")}, {QStringLiteral("text"), text}});
    chatInput_->clear();
}

void LobbyPage::appendChat(const QString& from, const QString& text, double ts) {
    const QString when = QDateTime::fromMSecsSinceEpoch(static_cast<qint64>(ts * 1000)).toString(QStringLiteral("hh:mm"));
    chat_->appendPlainText(QStringLiteral("%1  %2: %3").arg(when, from, text));
}

void LobbyPage::onMessage(const QJsonObject& msg) {
    const QString type = s(msg, "type");
    if (type == QLatin1String("lobbies")) {
        const QJsonArray items = msg.value(QStringLiteral("lobbies")).toArray();
        list_->setRowCount(static_cast<int>(items.size()));
        const Game* g = GameData::instance().game(gameId_);
        for (int i = 0; i < items.size(); ++i) {
            const QJsonObject lb = items.at(i).toObject();
            const GameMode* m = g ? g->mode(s(lb, "mode")) : nullptr;
            const int players = lb.value(QStringLiteral("players")).toInt();
            const int most = lb.value(QStringLiteral("max_players")).toInt(2);
            const int watching = lb.value(QStringLiteral("spectators")).toInt();
            const bool koth = lb.value(QStringLiteral("koth")).toBool();
            const QString who = watching ? tr("%1/%2, %3 watching").arg(players).arg(most).arg(watching)
                                         : QStringLiteral("%1/%2").arg(players).arg(most);
            const QString modeName = m ? m->name : s(lb, "mode");
            const QStringList cells = {s(lb, "id"), s(lb, "name"), s(lb, "host"),
                                       koth ? tr("%1, King of the Hill").arg(modeName) : modeName,
                                       GameData::regionName(s(lb, "region")), who,
                                       lb.value(QStringLiteral("locked")).toBool() ? tr("locked") : QString()};
            for (int c = 0; c < cells.size(); ++c) {
                auto* item = new QTableWidgetItem(cells[c]);
                if (c == 3) item->setData(Qt::UserRole, s(lb, "mode"));
                // every place to play taken (or, two-player, its match runs): watch it
                if (c == 5)
                    item->setData(Qt::UserRole, players >= most || (!koth && s(lb, "state") == QLatin1String("in_match")));
                list_->setItem(i, c, item);
            }
        }
        listInfo_->setText(items.isEmpty() ? tr("No open lobbies for this game.") : tr("%1 lobbies").arg(items.size()));
    } else if (type == QLatin1String("lobby")) {
        showLobby(msg.value(QStringLiteral("lobby")).toObject());
    } else if (type == QLatin1String("lobby_left")) {
        // the player left (the server says so only then; a dropped connection keeps the place, and the game): the
        // lobby's game is not needed any more
        leftLobby();
        controller_->closeGameWhenIdle();
        refresh();
    } else if (type == QLatin1String("lobby_chat")) {
        if (s(msg, "lobby") == s(lobby_, "id"))
            appendChat(s(msg, "from"), s(msg, "text"), msg.value(QStringLiteral("ts")).toDouble());
    } else if (type == QLatin1String("error") && s(msg, "re").startsWith(QLatin1String("lobby_"))) {
        const QString text = s(msg, "message");
        if (lobby_.isEmpty())
            listInfo_->setText(text);
        else
            chat_->appendPlainText(tr("-- %1").arg(text));
        if (s(msg, "re") == QLatin1String("lobby_ready")) ready_->setChecked(false);
    }
}

void LobbyPage::showLobby(const QJsonObject& lobby) {
    const bool newLobby = s(lobby, "id") != s(lobby_, "id");
    lobby_ = lobby;
    right_->setCurrentIndex(1);
    const Game* g = GameData::instance().game(s(lobby, "game"));
    const GameMode* m = g ? g->mode(s(lobby, "mode")) : nullptr;
    lobbyTitle_->setText(QStringLiteral("%1   [%2]").arg(s(lobby, "name"), s(lobby, "id")));
    const bool koth = lobby.value(QStringLiteral("koth")).toBool();
    const int most = lobby.value(QStringLiteral("max_players")).toInt(2);
    const QString king = s(lobby, "king");
    const int streak = lobby.value(QStringLiteral("streak")).toInt();
    // the players in the server's order (King of the Hill: the line, the first two fight), then the spectators
    QJsonArray members;
    for (const auto& mv : lobby.value(QStringLiteral("members")).toArray())
        if (!mv.toObject().value(QStringLiteral("spectator")).toBool()) members.append(mv);
    const int playersIn = int(members.size());
    for (const auto& mv : lobby.value(QStringLiteral("members")).toArray())
        if (mv.toObject().value(QStringLiteral("spectator")).toBool()) members.append(mv);
    const int watching = int(members.size()) - playersIn;
    QString extras;
    if (koth) {
        extras += tr("  |  King of the Hill, %1/%2 in line").arg(playersIn).arg(most);
        if (!king.isEmpty()) extras += tr(", king %1 (%n win(s) in a row)", nullptr, streak).arg(king);
    }
    if (watching) extras += tr("  |  %1 watching").arg(watching);
    lobbyInfo_->setText(tr("%1  |  %2  |  best of %3  |  %4%5  |  %6%7")
                            .arg(g ? g->shortName : s(lobby, "game"), m ? m->name : s(lobby, "mode"))
                            .arg(lobby.value(QStringLiteral("rounds")).toInt())
                            .arg(GameData::regionName(s(lobby, "region")),
                                 lobby.value(QStringLiteral("locked")).toBool() ? tr(", password") : QString(),
                                 s(lobby, "state").replace(QLatin1Char('_'), QLatin1Char(' ')), extras));
    members_->setRowCount(static_cast<int>(members.size()));
    // who must be ready for the host's START: both players, or the first two in a King of the Hill line
    bool allReady = koth ? playersIn >= 2 : playersIn == 2;
    bool meReady = false, meWatching = false;
    int myPlace = -1;
    QString myChar;
    for (int i = 0; i < members.size(); ++i) {
        const QJsonObject mem = members.at(i).toObject();
        const bool ready = mem.value(QStringLiteral("ready")).toBool();
        const bool spectator = mem.value(QStringLiteral("spectator")).toBool();
        if (!spectator && (!koth || i < 2)) allReady = allReady && ready;
        if (s(mem, "name") == server_->accountName()) {
            meReady = ready;
            meWatching = spectator;
            myPlace = i;
            myChar = s(mem, "character");
        }
        const QString name = s(mem, "name") + (mem.value(QStringLiteral("host")).toBool() ? tr("  (host)") : QString());
        QString place;
        if (spectator) place = tr("watching");
        else if (!koth) place = tr("plays");
        else if (i == 0) place = s(mem, "name") == king ? tr("KING (%n win(s) in a row)", nullptr, streak) : tr("first in line");
        else if (i == 1) place = tr("challenger");
        else place = tr("#%1 in line").arg(i + 1);
        // the member's connection (its own column): the link's icon, and their ping to the server
        const QString link = s(mem, "link");
        const int ping = mem.value(QStringLiteral("ping")).toInt(-1);
        const QStringList cells = {name, place, spectator ? tr("WATCHING") : ready ? tr("READY") : tr("-"),
                                   ping >= 0 ? tr("%1 ms").arg(ping) : QString()};
        for (int c = 0; c < cells.size(); ++c) members_->setItem(i, c, new QTableWidgetItem(cells[c]));
        if (const QPixmap icon = style::linkIcon(link, 16); !icon.isNull()) {
            members_->item(i, 3)->setIcon(QIcon(icon));
            members_->item(i, 3)->setToolTip(style::linkName(link));
        }
    }
    if (g && (newLobby || character_->count() == 0)) {
        const QSignalBlocker block(character_);
        character_->clear();
        // the game's own select screen, with the player's own controller (cfg::pickInGame)
        if (!(m && m->picksInGame())) character_->addItem(tr("Pick in the game"), QString());
        for (const auto& c : g->roster(s(lobby, "mode"))) character_->addItem(c.name, c.id);
    }
    if (myChar == QLatin1String("-")) myChar.clear();
    const int ci = character_->findData(myChar);
    if (ci >= 0) {
        const QSignalBlocker block(character_);
        character_->setCurrentIndex(ci);
    }
    const bool inMatch = s(lobby, "state") == QLatin1String("in_match");
    const bool host = s(lobby, "host") == server_->accountName();
    ready_->setChecked(meReady);
    ready_->setEnabled(!inMatch);
    ready_->setVisible(!meWatching);
    character_->setEnabled(!inMatch && !(m && m->picksInGame()));
    character_->setVisible(!meWatching && !(m && m->picksInGame()));  // MK9: picked on the game's select screen
    // a spectator takes a free place to play between matches; a player steps aside to watch
    role_->setText(meWatching ? tr("Play") : tr("Watch"));
    role_->setToolTip(meWatching ? tr("Take a free place to play in this lobby's next match")
                                 : tr("Step aside and watch this lobby's matches in your game"));
    role_->setEnabled(meWatching ? playersIn < most && (koth || !inMatch) : !inMatch);
    watchingHere_ = meWatching;
    if (!meWatching) controller_->stopWatching();  // a player now: nothing more to watch
    else rejoinGrace_->stop();                      // back in after a dropped connection
    // a server that counts down (its state has starts_in) starts the match by itself; one without it, the host's START
    const bool countsDown = lobby.contains(QStringLiteral("starts_in"));
    const double startsIn = lobby.value(QStringLiteral("starts_in")).toDouble(-1);
    startsAtMs_ = countsDown && !inMatch && startsIn >= 0
                      ? QDateTime::currentMSecsSinceEpoch() + qint64(startsIn * 1000.0) : 0;
    // Ready starts no game: this player's boots once the countdown for their match begins, on both PCs at once (at its
    // hold point by the time the match is set up); a countdown called off closes it again, nothing waits in a lobby
    const bool myMatch = !meWatching && meReady && myPlace >= 0 && (!koth || myPlace < 2);
    if (startsAtMs_ && myMatch) controller_->lobbyMatchComing(s(lobby, "game"));
    else if (!inMatch) controller_->lobbyMatchCalledOff();
    start_->setVisible(host && !countsDown);
    start_->setEnabled(host && allReady && !inMatch);
    start_->setText(inMatch ? tr("MATCH IN PROGRESS") : koth && playersIn >= 2 ? tr("START: %1 VS %2")
                                                                                       .arg(s(members.at(0).toObject(), "name"),
                                                                                            s(members.at(1).toObject(), "name"))
                                                                                 : tr("START"));
    updateLeaveMatch();
    updateStatus();
    if (newLobby) {
        chat_->clear();
        for (const auto& cv : lobby.value(QStringLiteral("chat")).toArray()) {
            const QJsonObject c = cv.toObject();
            appendChat(s(c, "from"), s(c, "text"), c.value(QStringLiteral("ts")).toDouble());
        }
        chat_->appendPlainText(meWatching
                                   ? tr("-- Watching lobby %1: each of its matches plays in your game.")
                                         .arg(s(lobby, "id"))
                                   : tr("-- Joined lobby %1. Share the code with your opponent.").arg(s(lobby, "id")));
    }
}

void LobbyPage::updateLeaveMatch() {
    // in a match of this lobby that the game reports no result for: the players end it here when they are done
    const bool mine = !lobby_.isEmpty() && controller_->canLeaveMatch() &&
                      controller_->currentFound().value(QStringLiteral("lobby")).toString() == s(lobby_, "id");
    leaveMatch_->setVisible(mine);
}

void LobbyPage::updateStatus() {
    if (lobby_.isEmpty() || !lobby_.contains(QStringLiteral("starts_in"))) {  // no countdown: START says it all
        tick_->stop();
        countdown_->hide();
        nextInfo_->hide();
        return;
    }
    if (s(lobby_, "state") == QLatin1String("in_match")) {
        tick_->stop();
        countdown_->setText(tr("MATCH IN PROGRESS"));
        countdown_->show();
        nextInfo_->hide();
        return;
    }
    if (startsAtMs_) {
        if (!tick_->isActive()) tick_->start();
        const qint64 left = startsAtMs_ - QDateTime::currentMSecsSinceEpoch();
        countdown_->setText(left > 0 ? tr("STARTING IN %1").arg((left + 999) / 1000) : tr("STARTING..."));
        countdown_->show();
        nextInfo_->hide();
        return;
    }
    tick_->stop();
    countdown_->hide();
    const bool koth = lobby_.value(QStringLiteral("koth")).toBool();
    nextInfo_->setText(spectating() ? tr("The next match plays in your game once its players are ready.")
                       : koth       ? tr("The match starts by itself once the first two in line are ready.")
                                    : tr("The match starts by itself once both players are ready."));
    nextInfo_->show();
}

void LobbyPage::leftLobby(bool keepWatching) {
    if (!keepWatching) controller_->stopWatching();
    watchingHere_ = false;
    tick_->stop();
    startsAtMs_ = 0;
    countdown_->hide();
    nextInfo_->hide();
    lobby_ = {};
    right_->setCurrentIndex(0);
    character_->clear();
    chat_->clear();
}
