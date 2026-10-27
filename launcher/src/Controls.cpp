#include "Controls.h"

#include <QDir>
#include <QFile>
#include <QKeyEvent>
#include <QRegularExpression>

#include <windows.h>

#include "AppPaths.h"
#include "WinUtil.h"

namespace controls {

namespace {

struct ControlInfo {
    const char* id;
    const char* label;
    const char* use;  // in MK9 by default (the game's own controller settings may change it)
};
const ControlInfo kControls[Count] = {
    {"up", "D-pad up", "up, jump"},
    {"down", "D-pad down", "down, crouch"},
    {"left", "D-pad left", "left"},
    {"right", "D-pad right", "right"},
    {"a", "A", "3: front kick"},
    {"b", "B", "4: back kick"},
    {"x", "X", "1: front punch"},
    {"y", "Y", "2: back punch"},
    {"lb", "LB (left bumper)", "tag"},
    {"rb", "RB (right bumper)", "throw"},
    {"lt", "LT (left trigger)", "flip stance"},
    {"rt", "RT (right trigger)", "block"},
    {"start", "Start", "pause"},
    {"back", "Back", ""},
    {"ls", "Left stick click", ""},
    {"rs", "Right stick click", ""},
};

// The game's key names (mk9/src/mk9/controls.cpp KeyFromName), with what the page shows; no name: the file writes the
// key as 0x.. (the game reads that too); no label: the sign the keyboard's layout puts on that key. Letters, digits,
// F1-F24 and Num0-Num9 go by pattern. `alias`: another name taken on reading.
struct KeyInfo {
    int vk;
    const char* name;
    const char* label;
    const char* alias;
};
const KeyInfo kKeys[] = {
    {VK_UP, "Up", "Up arrow", nullptr},
    {VK_DOWN, "Down", "Down arrow", nullptr},
    {VK_LEFT, "Left", "Left arrow", nullptr},
    {VK_RIGHT, "Right", "Right arrow", nullptr},
    {VK_SPACE, "Space", "Space", nullptr},
    {VK_RETURN, "Return", "Enter", "Enter"},
    {VK_BACK, "Backspace", "Backspace", nullptr},
    {VK_TAB, "Tab", "Tab", nullptr},
    {VK_CAPITAL, nullptr, "Caps Lock", "CapsLock"},
    {VK_SHIFT, "Shift", "Shift", nullptr},
    {VK_LSHIFT, "LShift", "Left Shift", nullptr},
    {VK_RSHIFT, "RShift", "Right Shift", nullptr},
    {VK_CONTROL, "Ctrl", "Ctrl", nullptr},
    {VK_LCONTROL, "LCtrl", "Left Ctrl", nullptr},
    {VK_RCONTROL, "RCtrl", "Right Ctrl", nullptr},
    {VK_INSERT, "Insert", "Insert", nullptr},
    {VK_DELETE, "Delete", "Delete", nullptr},
    {VK_HOME, "Home", "Home", nullptr},
    {VK_END, "End", "End", nullptr},
    {VK_PRIOR, "PageUp", "Page Up", nullptr},
    {VK_NEXT, "PageDown", "Page Down", nullptr},
    {VK_MULTIPLY, "NumMul", "Num *", "NumMultiply"},
    {VK_ADD, "NumAdd", "Num +", nullptr},
    {VK_SUBTRACT, "NumSub", "Num -", "NumSubtract"},
    {VK_DECIMAL, "NumDot", "Num .", "NumDecimal"},
    {VK_DIVIDE, "NumDiv", "Num /", "NumDivide"},
    {VK_OEM_1, "Semicolon", nullptr, nullptr},
    {VK_OEM_PLUS, "Equals", nullptr, nullptr},
    {VK_OEM_COMMA, "Comma", nullptr, nullptr},
    {VK_OEM_MINUS, "Minus", nullptr, nullptr},
    {VK_OEM_PERIOD, "Period", nullptr, nullptr},
    {VK_OEM_2, "Slash", nullptr, nullptr},
    {VK_OEM_3, "Grave", nullptr, nullptr},
    {VK_OEM_4, "LBracket", nullptr, nullptr},
    {VK_OEM_5, "Backslash", nullptr, nullptr},
    {VK_OEM_6, "RBracket", nullptr, nullptr},
    {VK_OEM_7, "Quote", nullptr, "Apostrophe"},
    {VK_OEM_8, nullptr, nullptr, "Oem8"},
    {VK_OEM_102, nullptr, nullptr, "Oem102"},
};

const KeyInfo* keyInfo(int vk) {
    for (const KeyInfo& k : kKeys)
        if (k.vk == vk) return &k;
    return nullptr;
}

bool letterOrDigit(int vk) { return (vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9'); }

QString hexName(int vk) { return QStringLiteral("0x") + QStringLiteral("%1").arg(vk, 2, 16, QLatin1Char('0')).toUpper(); }

int sourceFromId(const QString& id) {
    const QList<Source>& all = sources();
    for (int i = 0; i < all.size(); ++i)
        if (all[i].id.compare(id, Qt::CaseInsensitive) == 0) return i;
    return -1;
}

// a line without its comment: from a '#' outside quotes on
QString uncommented(const QString& line) {
    bool quoted = false;
    for (int i = 0; i < line.size(); ++i) {
        if (line[i] == QLatin1Char('"')) quoted = !quoted;
        else if (line[i] == QLatin1Char('#') && !quoted) return line.left(i);
    }
    return line;
}

// the quoted strings of a value: "K" or ["W", "Up"]
QStringList strings(const QString& value) {
    static const QRegularExpression quoted(QStringLiteral("\"([^\"]*)\""));
    QStringList out;
    for (auto it = quoted.globalMatch(value); it.hasNext();) out << it.next().captured(1);
    return out;
}

}  // namespace

QString controlId(int control) {
    return control >= 0 && control < Count ? QLatin1String(kControls[control].id) : QString();
}

QString controlLabel(int control) {
    return control >= 0 && control < Count ? QLatin1String(kControls[control].label) : QString();
}

QString controlUse(int control) {
    return control >= 0 && control < Count ? QLatin1String(kControls[control].use) : QString();
}

int controlFromId(const QString& id) {
    for (int c = 0; c < Count; ++c)
        if (id.compare(QLatin1String(kControls[c].id), Qt::CaseInsensitive) == 0) return c;
    return -1;
}

// the buttons the game knows (mk9/src/mk9/controls.cpp kSources): a PC controller's, as the SDK reports them in Xbox
// 360 positions
const QList<Source>& sources() {
    static const QList<Source> all = {
        {QStringLiteral("south"), QStringLiteral("Bottom face button (A / Cross)"),
         QStringLiteral("Xbox A, PlayStation cross, Switch B"), A},
        {QStringLiteral("east"), QStringLiteral("Right face button (B / Circle)"),
         QStringLiteral("Xbox B, PlayStation circle, Switch A"), B},
        {QStringLiteral("west"), QStringLiteral("Left face button (X / Square)"),
         QStringLiteral("Xbox X, PlayStation square, Switch Y"), X},
        {QStringLiteral("north"), QStringLiteral("Top face button (Y / Triangle)"),
         QStringLiteral("Xbox Y, PlayStation triangle, Switch X"), Y},
        {QStringLiteral("left_shoulder"), QStringLiteral("Left bumper (LB / L1)"),
         QStringLiteral("Xbox LB, PlayStation L1, Switch L"), LB},
        {QStringLiteral("right_shoulder"), QStringLiteral("Right bumper (RB / R1)"),
         QStringLiteral("Xbox RB, PlayStation R1, Switch R"), RB},
        {QStringLiteral("left_trigger"), QStringLiteral("Left trigger (LT / L2)"),
         QStringLiteral("Xbox LT, PlayStation L2, Switch ZL"), LT},
        {QStringLiteral("right_trigger"), QStringLiteral("Right trigger (RT / R2)"),
         QStringLiteral("Xbox RT, PlayStation R2, Switch ZR"), RT},
        {QStringLiteral("dpad_up"), QStringLiteral("D-pad up"), QString(), Up},
        {QStringLiteral("dpad_down"), QStringLiteral("D-pad down"), QString(), Down},
        {QStringLiteral("dpad_left"), QStringLiteral("D-pad left"), QString(), Left},
        {QStringLiteral("dpad_right"), QStringLiteral("D-pad right"), QString(), Right},
        {QStringLiteral("start"), QStringLiteral("Start / Menu / Options"),
         QStringLiteral("Xbox 360 Start, Xbox Menu, PlayStation Options, Switch +"), Start},
        {QStringLiteral("back"), QStringLiteral("Back / View / Share"),
         QStringLiteral("Xbox 360 Back, Xbox View, PlayStation Share or Create, Switch -"), Back},
        {QStringLiteral("left_stick"), QStringLiteral("Left stick click (L3)"), QString(), LS},
        {QStringLiteral("right_stick"), QStringLiteral("Right stick click (R3)"), QString(), RS},
    };
    return all;
}

// as the game has them without the file: arrows or WASD move, J I K L are X Y A B (1 2 3 4), Space RT (block), O RB
// (throw), U LB (tag), P LT (flip stance), Enter Start, Backspace Back
Map defaults() {
    Map m;
    m.keys[Up] = {VK_UP, 'W'};
    m.keys[Down] = {VK_DOWN, 'S'};
    m.keys[Left] = {VK_LEFT, 'A'};
    m.keys[Right] = {VK_RIGHT, 'D'};
    m.keys[X] = {'J'};
    m.keys[Y] = {'I'};
    m.keys[A] = {'K'};
    m.keys[B] = {'L'};
    m.keys[RT] = {VK_SPACE};
    m.keys[RB] = {'O'};
    m.keys[LB] = {'U'};
    m.keys[LT] = {'P'};
    m.keys[Start] = {VK_RETURN};
    m.keys[Back] = {VK_BACK};
    for (const Source& s : sources()) m.presses << s.control;
    return m;
}

QString file() { return QDir(paths::dataDir()).filePath(QStringLiteral("controls.toml")); }

Map load() {
    Map m = defaults();
    QFile f(file());
    if (!f.open(QIODevice::ReadOnly)) return m;
    QString section;
    QString text = QString::fromUtf8(f.readAll());
    if (text.startsWith(QChar(0xFEFF))) text.remove(0, 1);  // saved by Notepad
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (const QString& raw : lines) {
        const QString line = uncommented(raw).trimmed();
        if (line.isEmpty()) continue;
        if (line.startsWith(QLatin1Char('[')) && line.endsWith(QLatin1Char(']'))) {
            section = line.mid(1, line.size() - 2).trimmed();
            continue;
        }
        const qsizetype eq = line.indexOf(QLatin1Char('='));
        if (eq <= 0) continue;
        const QString key = line.left(eq).trimmed();
        const QString value = line.mid(eq + 1).trimmed();
        if (section == QLatin1String("keyboard") && key == QLatin1String("enabled")) {
            m.keyboard = value != QLatin1String("false");
        } else if (section == QLatin1String("keys")) {
            const int c = controlFromId(key);
            if (c < 0) continue;
            m.keys[c].clear();
            for (const QString& name : strings(value)) {
                const int vk = keyFromName(name);
                if (vk && !m.keys[c].contains(vk) && m.keys[c].size() < kKeysPerButton) m.keys[c] << vk;
            }
        } else if (section == QLatin1String("buttons")) {
            const int i = sourceFromId(key);
            if (i < 0) continue;
            const QStringList target = strings(value);
            m.presses[i] = target.isEmpty() || target.first().isEmpty() ? -1 : controlFromId(target.first());
        }
    }
    return m;
}

bool save(const Map& m, QString* error) {
    QString text = QStringLiteral(
        "# MK9 Online controls. The launcher's Controls page writes this file and hands it to the game (MK9_CONTROLS);\n"
        "# a game that is running reads it again within a second of a change. The buttons are the Xbox 360\n"
        "# controller's: up down left right (the d-pad), a b x y, lb rb (the bumpers), lt rt (the triggers), start back,\n"
        "# ls rs (the stick clicks).\n"
        "\n"
        "[keyboard]\n"
        "# the keyboard plays as player 1 while the game's window is in front\n");
    text += QStringLiteral("enabled = %1\n\n").arg(m.keyboard ? QStringLiteral("true") : QStringLiteral("false"));
    text += QStringLiteral("# The keys that press each button (Windows key names, at most two each).\n[keys]\n");
    for (int c = 0; c < Count; ++c) {
        QStringList names;
        for (int vk : m.keys[c]) names << QLatin1Char('"') + keyName(vk) + QLatin1Char('"');
        text += QStringLiteral("%1 = [%2]\n").arg(controlId(c), names.join(QStringLiteral(", ")));
    }
    text += QStringLiteral("\n# What each button of a PC controller presses, by where the button sits (\"\" nothing).\n"
                           "[buttons]\n");
    const QList<Source>& all = sources();
    for (int i = 0; i < all.size(); ++i)
        text += QStringLiteral("%1 = \"%2\"\n").arg(all[i].id, controlId(i < m.presses.size() ? m.presses[i] : -1));

    // whole or not at all: a game reading it at that moment holds up the swap for an instant
    QString why;
    if (winutil::writeFileAtomic(file(), text.toUtf8(), &why)) return true;
    if (error) *error = QStringLiteral("Could not save the controls: %1").arg(why);
    return false;
}

QString keyName(int vk) {
    if (letterOrDigit(vk)) return QString(QLatin1Char(char(vk)));
    if (const KeyInfo* k = keyInfo(vk)) return k->name ? QLatin1String(k->name) : hexName(vk);
    if (vk >= VK_F1 && vk <= VK_F24) return QStringLiteral("F%1").arg(vk - VK_F1 + 1);
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return QStringLiteral("Num%1").arg(vk - VK_NUMPAD0);
    return hexName(vk);
}

int keyFromName(const QString& name) {
    const QString s = name.trimmed();
    if (s.size() == 1 && s[0].isLetterOrNumber() && s[0].unicode() < 128) return s[0].toUpper().unicode();
    for (const KeyInfo& k : kKeys) {
        if (k.name && s.compare(QLatin1String(k.name), Qt::CaseInsensitive) == 0) return k.vk;
        if (k.alias && s.compare(QLatin1String(k.alias), Qt::CaseInsensitive) == 0) return k.vk;
    }
    bool ok = false;
    if (s.size() >= 2 && s[0].toUpper() == QLatin1Char('F')) {
        const int n = s.mid(1).toInt(&ok);
        if (ok && n >= 1 && n <= 24 && s.mid(1).at(0).isDigit()) return VK_F1 + n - 1;
    }
    if (s.size() == 4 && s.left(3).compare(QLatin1String("Num"), Qt::CaseInsensitive) == 0 && s[3].isDigit())
        return VK_NUMPAD0 + s[3].digitValue();
    if (s.size() > 2 && s.startsWith(QLatin1String("0x"), Qt::CaseInsensitive)) {
        const int v = s.mid(2).toInt(&ok, 16);
        if (ok && v > 0 && v < 0xFF) return v;
    }
    return 0;
}

QString keyLabel(int vk) {
    if (letterOrDigit(vk)) return QString(QLatin1Char(char(vk)));
    if (const KeyInfo* k = keyInfo(vk); k && k->label) return QLatin1String(k->label);
    if (vk >= VK_F1 && vk <= VK_F24) return QStringLiteral("F%1").arg(vk - VK_F1 + 1);
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return QStringLiteral("Num %1").arg(vk - VK_NUMPAD0);
    // a sign key: what this keyboard's layout prints on it (the high bit marks a dead key)
    const UINT sign = MapVirtualKeyW(UINT(vk), MAPVK_VK_TO_CHAR) & 0x7FFF;
    if (sign > 0x20) return QString(QChar(char16_t(sign)));
    wchar_t text[64] = {};
    const LONG scan = LONG(MapVirtualKeyW(UINT(vk), MAPVK_VK_TO_VSC)) << 16;
    if (scan && GetKeyNameTextW(scan, text, 64) > 0) return QString::fromWCharArray(text);
    return keyName(vk);
}

int keyFromEvent(const QKeyEvent* event) {
    int vk = int(event->nativeVirtualKey());
    const quint32 scan = event->nativeScanCode();  // bit 8: an extended key (the right-hand Ctrl)
    if (vk == VK_SHIFT) vk = (scan & 0xFF) == 0x36 ? VK_RSHIFT : VK_LSHIFT;
    else if (vk == VK_CONTROL) vk = (scan & 0x100) ? VK_RCONTROL : VK_LCONTROL;
    switch (vk) {
        case VK_ESCAPE:
        case VK_MENU:
        case VK_LMENU:
        case VK_RMENU:
        case VK_F10:
        case VK_LWIN:
        case VK_RWIN:
        case VK_APPS:
        case VK_NUMLOCK:
        case VK_SCROLL:
        case VK_PAUSE:
        case VK_SNAPSHOT:
        case VK_PROCESSKEY:
        case VK_PACKET:
            return 0;
        default:
            return vk > 0 && vk < 0xFF ? vk : 0;
    }
}

}  // namespace controls
