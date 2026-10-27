#pragma once
// The controls file the game reads (mk9/src/mk9/controls.cpp): which keys press each button of the Xbox 360 controller
// (the keyboard plays as player 1 while the game's window is in front), and what each button of a PC controller
// presses. The Controls page edits it; the launcher hands its path to every game it starts (MK9_CONTROLS), and a game
// that is running reads it again within a second of a change.
//
//   [keyboard]
//   enabled = true
//   [keys]                       # Windows virtual key names, at most two per button
//   up = ["Up", "W"]
//   [buttons]                    # a PC controller's button (where it sits) -> the button it presses
//   south = "a"                  # "" presses nothing

#include <QList>
#include <QString>

#include <array>

class QKeyEvent;

namespace controls {

// the Xbox 360 controller's buttons, in the order the page lists them (the file's names: up down left right a b x y
// lb rb lt rt start back ls rs)
enum Control { Up, Down, Left, Right, A, B, X, Y, LB, RB, LT, RT, Start, Back, LS, RS, Count };
QString controlId(int control);     // the file's name: "up", "a", "lt"
QString controlLabel(int control);  // "D-pad up", "A", "LT (left trigger)"
QString controlUse(int control);    // what it does in the game by default: "3: front kick", "block"
int controlFromId(const QString& id);

// a PC controller's button, named by where it sits (the bottom face button is Xbox A, PlayStation cross, Switch B)
struct Source {
    QString id;     // the file's name: "south", "left_shoulder", "dpad_up"
    QString label;  // "Bottom face button (A / Cross)"
    QString hint;   // what it is called on each kind of controller
    int control;    // what it presses unless changed, -1 nothing
};
const QList<Source>& sources();

constexpr int kKeysPerButton = 2;

struct Map {
    bool keyboard = true;                      // the keyboard plays as player 1
    std::array<QList<int>, Count> keys;        // Windows virtual keys, at most kKeysPerButton each
    QList<int> presses;                        // per sources() entry: the control it presses, -1 nothing
};
Map defaults();  // as the game has them without the file (mk9/src/mk9/controls.cpp Defaults)
QString file();  // controls.toml beside the launcher's settings file
Map load();      // the file's controls over the defaults
bool save(const Map& map, QString* error);

QString keyName(int vk);  // as the file writes it, in the game's names: "K", "Space", "LShift", "0xE5"
int keyFromName(const QString& name);
QString keyLabel(int vk);  // for people: "K", "Left Shift", the sign this keyboard's layout puts on the key
// the virtual key of a press, left and right Shift and Ctrl apart; 0 for a key the game cannot use (Alt and F10 open
// the window menu, the Windows keys and Esc belong to Windows and to leaving)
int keyFromEvent(const QKeyEvent* event);

}  // namespace controls
