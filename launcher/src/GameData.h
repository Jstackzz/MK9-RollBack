#pragma once
// Games, modes and rosters from the bundled launcher/data/<game>.json files (MK9: data/mk9.json).

#include <QHash>
#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>

struct Character {
    QString id;
    QString name;
    bool secret = false;  // unlocked in the game
    bool dlc = false;     // downloadable on the original release, on the Komplete Edition disc
    bool boss = false;    // a boss: on the disc, not on the regular select screen
};

struct GameMode {
    QString id;
    QString name;
    QString description;
    QString roster;
    QString characterSelect;  // "launcher" or "in_game"
    bool online = false;
    bool ranked = false;
    int fighters = 1;  // each player's fighters in a match (two in a tag team)
    QList<int> rounds;
    int defaultRounds = 3;
    bool deathTraps = false;  // its fights' arenas have death traps a match may turn off (none in MK9)
    bool picksInGame() const { return characterSelect == QLatin1String("in_game"); }
};

struct Game {
    QString id;
    QString title;
    QString shortName;
    QString edition;
    QString titleId;
    QString defaultExe;
    QString discSha1;  // SHA-1 of the disc's default.xex this build of the game is made from
    QString notes;
    QList<GameMode> modes;
    QHash<QString, QList<Character>> rosters;
    QStringList notOnXbox;

    const GameMode* mode(const QString& modeId) const;
    QList<GameMode> onlineModes() const;
    QList<Character> roster(const QString& modeId) const;
    QString characterName(const QString& characterId) const;
};

class GameData {
public:
    static GameData& instance();
    bool load(QString* error);
    const QList<Game>& games() const { return games_; }
    const Game* game(const QString& id) const;
    QString defaultGame() const { return games_.isEmpty() ? QString() : games_.first().id; }

    static const QList<QPair<QString, QString>>& regions();  // id, display name
    static QString regionName(const QString& id);

private:
    QList<Game> games_;
};
