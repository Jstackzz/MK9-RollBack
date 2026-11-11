#include "CrashReports.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSettings>
#include <QSysInfo>
#include <QThread>

#include <windows.h>

#include "AppPaths.h"
#include "ServerClient.h"

namespace {
constexpr int kKeep = 20;              // reports kept, the newest
constexpr int kGameLogLines = 200;     // the end of the game's log in report.txt
constexpr int kLauncherLogLines = 60;  // and of the launcher's
constexpr int kActivity = 8;           // what the launcher was doing: its last texts
constexpr int kPart = 9000;            // base64 characters per crash_report message (the server reads lines of 16 KB)
constexpr int kNoteLines = 16;         // a crash_note's lines, at most (the server's limit)

QString native(const QString& p) { return QDir::toNativeSeparators(p); }

// the last `n` lines of a text file, read from its end (a game's log runs to megabytes)
QStringList tail(const QString& path, int n, qint64 maxBytes = 512 * 1024) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    const bool cut = f.size() > maxBytes;
    if (cut) f.seek(f.size() - maxBytes);
    QStringList lines = QString::fromUtf8(f.readAll()).split(QLatin1Char('\n'));
    if (cut && !lines.isEmpty()) lines.removeFirst();  // the line the cut went through
    for (QString& l : lines) l = l.trimmed();
    while (!lines.isEmpty() && lines.last().isEmpty()) lines.removeLast();
    return lines.mid(qMax(0, int(lines.size()) - n));
}

// the first lines of a file's start that match (the graphics card the game's log names)
QStringList grepHead(const QString& path, const QRegularExpression& re, int n, qint64 maxBytes = 256 * 1024) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    QStringList out;
    for (const QString& line : QString::fromUtf8(f.read(maxBytes)).split(QLatin1Char('\n')))
        if (line.contains(re) && out.size() < n) out << line.trimmed();
    return out;
}

QString thisPc() {
    QStringList out;
    out << QStringLiteral("Windows: %1 (%2)").arg(QSysInfo::prettyProductName(), QSysInfo::kernelVersion());
    const QSettings cpu(QStringLiteral("HKEY_LOCAL_MACHINE\\HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0"),
                        QSettings::NativeFormat);
    out << QStringLiteral("processor: %1, %2 threads")
               .arg(cpu.value(QStringLiteral("ProcessorNameString")).toString().trimmed())
               .arg(QThread::idealThreadCount());
    MEMORYSTATUSEX mem{};
    mem.dwLength = sizeof mem;
    if (GlobalMemoryStatusEx(&mem))
        out << QStringLiteral("memory: %1 GB, %2 GB free")
                   .arg(double(mem.ullTotalPhys) / (1 << 30), 0, 'f', 1)
                   .arg(double(mem.ullAvailPhys) / (1 << 30), 0, 'f', 1);
    return out.join(QLatin1Char('\n'));
}

QString hex(int code) { return QStringLiteral("%1").arg(quint32(code), 8, 16, QLatin1Char('0')).toUpper(); }
}  // namespace

CrashReports& CrashReports::instance() {
    static CrashReports reports;
    return reports;
}

QString CrashReports::folder() { return QDir(paths::dataDir()).filePath(QStringLiteral("crashes")); }

void CrashReports::setActivity(const QString& text) {
    if (text.isEmpty() || (!activity_.isEmpty() && activity_.last().endsWith(text))) return;
    activity_ << QStringLiteral("%1 %2").arg(QDateTime::currentDateTime().toString(QStringLiteral("hh:mm:ss")), text);
    while (activity_.size() > kActivity) activity_.removeFirst();
}

QString CrashReports::exitText(int exitCode) {
    switch (quint32(exitCode)) {
    case 0xC0000005: return tr("an access violation");
    case 0xC00000FD: return tr("a stack overflow");
    case 0xC0000409: return tr("a fail fast (stack buffer overrun)");
    case 0xC000001D: return tr("an illegal instruction");
    case 0xC0000094: return tr("a division by zero");
    case 0xC0000374: return tr("heap corruption");
    case 0xC0000017: return tr("running out of memory");
    case 0xC0000135: return tr("a missing DLL");
    case 0xC0000142: return tr("a DLL that failed to start");
    case 0xE06D7363: return tr("a C++ exception");
    case 3: return tr("abort()");
    }
    return {};
}

void CrashReports::gameEnded(const QString& gameId, const QString& sessionDir, int exitCode, qint64 ranMs) {
    const QDateTime now = QDateTime::currentDateTime();
    QDir root(folder());
    root.mkpath(QStringLiteral("."));
    const QFileInfoList old = root.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name | QDir::Reversed);
    for (int i = kKeep - 1; i < old.size(); i++) QDir(old[i].absoluteFilePath()).removeRecursively();
    QString name = QStringLiteral("%1-%2").arg(now.toString(QStringLiteral("yyyyMMdd-hhmmss")), gameId);
    for (int n = 2; root.exists(name); n++)
        name = QStringLiteral("%1-%2-%3").arg(now.toString(QStringLiteral("yyyyMMdd-hhmmss")), gameId).arg(n);
    root.mkpath(name);
    const QDir dir(root.filePath(name));
    const QDir session(sessionDir);

    // what the game wrote about its crash (nothing when it went without running any code of its own), or about a match
    // where it stood still until it was closed (hang.txt); its dump moves here, its log is copied (the session folder
    // goes once 30 newer ones are there)
    QString gameReport;
    QFile crashTxt(session.filePath(QStringLiteral("crash.txt")));
    QFile hangTxt(session.filePath(QStringLiteral("hang.txt")));
    // (a game that stood still may have been killed: closed for a match that ended, it did not answer)
    const bool stoodStill = !crashTxt.exists() && hangTxt.exists();
    if (crashTxt.open(QIODevice::ReadOnly)) gameReport = QString::fromUtf8(crashTxt.readAll()).trimmed();
    else if (stoodStill && hangTxt.open(QIODevice::ReadOnly)) gameReport = QString::fromUtf8(hangTxt.readAll()).trimmed();
    const QString dump = session.filePath(QStringLiteral("crash.dmp"));
    if (QFileInfo::exists(dump) && !QFile::rename(dump, dir.filePath(QStringLiteral("crash.dmp"))))
        QFile::copy(dump, dir.filePath(QStringLiteral("crash.dmp")));
    const QString gameLog = session.filePath(QStringLiteral("game.log"));
    QFile::copy(gameLog, dir.filePath(QStringLiteral("game.log")));

    QString what;
    for (const QString& line : gameReport.split(QLatin1Char('\n')))
        if (line.startsWith(QLatin1String("exception ")) || line.startsWith(QLatin1String("what: ")) ||
            (stoodStill && line.startsWith(QLatin1String("main thread "))))
            what = line.trimmed();  // the exception line comes after the "what" line, and says more
    const QString codeWords = exitText(exitCode);
    const QString summary = stoodStill ? tr("The game stood still in a match (no new frame for 10 s or more) and was "
                                            "closed.")
                            : codeWords.isEmpty() ? tr("The game ended with the error code %1.").arg(hex(exitCode))
                                                  : tr("The game crashed with %1 (%2).").arg(codeWords, hex(exitCode));
    const QStringList graphics =
        grepHead(gameLog, QRegularExpression(QStringLiteral("adapter|GPU:|Vulkan device")), 3);

    QStringList r;
    r << QStringLiteral("MK9 Online crash report");
    r << QStringLiteral("time: %1").arg(now.toString(QStringLiteral("yyyy-MM-dd hh:mm:ss t")));
    r << QStringLiteral("launcher: %1").arg(QStringLiteral(MK9_LAUNCHER_VERSION));
    QFile built(QDir(cfg::gameFolder(gameId)).filePath(QStringLiteral("built.txt")));
    if (built.open(QIODevice::ReadOnly)) r << QStringLiteral("game %1: built from %2").arg(gameId, QString::fromUtf8(built.readAll()).trimmed());
    r << QStringLiteral("exit code: %1%2, after %3 s").arg(hex(exitCode), codeWords.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(codeWords))
             .arg(ranMs / 1000);
    if (!what.isEmpty()) r << QStringLiteral("crash: %1").arg(what);
    r << thisPc();
    for (const QString& line : graphics) r << QStringLiteral("graphics: %1").arg(line.section(QLatin1Char(']'), -1).trimmed());
    r << QString() << QStringLiteral("=== what the launcher was doing (its last messages)");
    r << (activity_.isEmpty() ? QStringLiteral("(nothing yet)") : activity_.join(QLatin1Char('\n')));
    r << QString() << (stoodStill ? QStringLiteral("=== the game's own report (hang.txt)")
                                  : QStringLiteral("=== the game's own report (crash.txt)"));
    r << (gameReport.isEmpty() ? QStringLiteral("(none: the game ended without writing one)") : gameReport);
    r << QString() << QStringLiteral("=== the end of the game's log (game.log)");
    r << tail(gameLog, kGameLogLines);
    r << QString() << QStringLiteral("=== the end of the launcher's log");
    r << tail(paths::logFile(), kLauncherLogLines);
    QFile out(dir.filePath(QStringLiteral("report.txt")));
    if (out.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) out.write(r.join(QLatin1Char('\n')).toUtf8() + '\n');
    out.close();

    // the note the server gets by itself (sendNote): how the game ended, then the lines of its own report that say what
    // and where, with the stacks' lines joined, its graphics card and the last errors of its log
    QStringList note{stoodStill ? QStringLiteral("closed while it stood still in a match, after %1 s").arg(ranMs / 1000)
                                : QStringLiteral("exit code %1%2, after %3 s")
                                      .arg(hex(exitCode), codeWords.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(codeWords))
                                      .arg(ranMs / 1000)};
    static const QRegularExpression keep(QStringLiteral(
        "^(game \\d|what: |exception |  (reading|writing|executing) address|thread |main thread |rendering thread|"
        "guest registers: |frames run: |launcher link status: |match: |netplay: |game: )"));
    static const QRegularExpression frameLine(QStringLiteral("^\\s*\\d+ (\\S.*)$"));
    const QStringList own = gameReport.split(QLatin1Char('\n'));
    for (int i = 0; i < own.size(); i++) {
        if (own[i].contains(keep)) {
            note << own[i].trimmed();
        } else if (own[i].startsWith(QLatin1String("host call stack:"))) {  // "  3 mk9.exe+0x1a2b sub_82..+0x24" each
            QStringList frames;
            for (int j = i + 1; j < own.size() && frames.size() < 8; j++) {
                const QRegularExpressionMatch m = frameLine.match(own[j]);
                if (!m.hasMatch()) break;
                frames << m.captured(1);
            }
            if (!frames.isEmpty()) note << QStringLiteral("host: ") + frames.mid(0, 4).join(QStringLiteral(" < "));
            if (frames.size() > 4) note << QStringLiteral("host: ... < ") + frames.mid(4).join(QStringLiteral(" < "));
        } else if (own[i].startsWith(QLatin1String("guest call stack"))) {  // return addresses, 8 a line
            QStringList returns;
            for (int j = i + 1; j < own.size() && own[j].startsWith(QLatin1Char(' ')); j++) returns << own[j].trimmed();
            if (!returns.isEmpty()) note << QStringLiteral("guest: ") + returns.join(QLatin1Char(' '));
        }
    }
    for (const QString& line : graphics) note << QStringLiteral("graphics: %1").arg(line.section(QLatin1Char(']'), -1).trimmed());
    QStringList errors;
    for (const QString& line : tail(gameLog, kGameLogLines))
        if (line.contains(QLatin1String("] [error]")) || line.contains(QLatin1String("] [critical]")))
            errors << line.section(QLatin1Char(']'), 2).trimmed();  // without its time
    note << errors.mid(qMax(0, int(errors.size()) - 3));
    static const QRegularExpression matchId(QStringLiteral("match[= ]([0-9a-f]{6,16})\\b"));
    const QRegularExpressionMatch inMatch = matchId.match(gameReport);
    QFile noteFile(dir.filePath(QStringLiteral("note.json")));
    if (noteFile.open(QIODevice::WriteOnly | QIODevice::Truncate))
        noteFile.write(QJsonDocument(QJsonObject{{QStringLiteral("game"), gameId},
                                                 {QStringLiteral("match"), inMatch.hasMatch() ? inMatch.captured(1) : QString()},
                                                 {QStringLiteral("lines"), QJsonArray::fromStringList(note)}})
                           .toJson());
    noteFile.close();
    applog::write(QStringLiteral("crash report: %1 (%2)").arg(native(dir.absolutePath()), what.isEmpty() ? summary : what));
    emit written(dir.absolutePath(), summary);
}

bool CrashReports::sendNote(ServerClient* server, const QString& reportDir) {
    if (!server || !server->isLoggedIn()) return false;
    const QDir dir(reportDir);
    if (dir.exists(QStringLiteral("noted.txt"))) return false;
    QFile f(dir.filePath(QStringLiteral("note.json")));
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QJsonObject note = QJsonDocument::fromJson(f.readAll()).object();
    // as a desync report's lines: printable, no address of anyone, no user folder, 200 characters at most
    static const QRegularExpression ip(QStringLiteral("\\b\\d{1,3}(\\.\\d{1,3}){3}\\b"));
    static const QRegularExpression home(QStringLiteral("([A-Za-z]:[\\\\/]Users[\\\\/])[^\\\\/]+"),
                                         QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression unprintable(QStringLiteral("[^\\x20-\\x7E]"));
    QJsonArray lines;
    for (const QJsonValue& v : note.value(QStringLiteral("lines")).toArray()) {
        if (lines.size() >= kNoteLines) break;
        QString l = v.toString().trimmed();
        l.replace(unprintable, QStringLiteral("?"));
        l.replace(ip, QStringLiteral("x.x.x.x"));
        l.replace(home, QStringLiteral("\\1*"));
        lines << l.left(200);
    }
    const QString game = note.value(QStringLiteral("game")).toString();
    if (game.isEmpty() || lines.isEmpty()) return false;
    server->send({{QStringLiteral("type"), QStringLiteral("crash_note")},
                  {QStringLiteral("game"), game},
                  {QStringLiteral("match"), note.value(QStringLiteral("match")).toString().left(16)},
                  {QStringLiteral("lines"), lines}});
    QFile mark(dir.filePath(QStringLiteral("noted.txt")));
    if (mark.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
        mark.write(QStringLiteral("noted to %1 on %2\n").arg(server->address(), QDateTime::currentDateTime().toString(Qt::ISODate)).toUtf8());
    applog::write(QStringLiteral("crash report: where it happened went to the server (%1 lines)").arg(lines.size()));
    return true;
}

QByteArray CrashReports::forSending(const QString& reportDir) {
    QFile f(QDir(reportDir).filePath(QStringLiteral("report.txt")));
    if (!f.open(QIODevice::ReadOnly)) return {};
    QString text = QString::fromUtf8(f.readAll());
    // C:\Users\<name> (and C:/Users/<name>) say who this is: they go, the rest of each path stays
    const QString home = QDir::homePath();
    for (const QString& form : {native(home), QDir::fromNativeSeparators(home)})
        text.replace(form, QStringLiteral("%USERPROFILE%"), Qt::CaseInsensitive);
    return text.toUtf8();
}

bool CrashReports::send(ServerClient* server, const QString& reportDir) {
    if (!server || !server->isOnline()) return false;
    const QByteArray text = forSending(reportDir);
    if (text.isEmpty()) return false;
    const QByteArray packed = qCompress(text, 9).toBase64();
    const QString id = QString::number(QRandomGenerator::global()->generate64(), 16).rightJustified(16, QLatin1Char('0'));
    const int parts = int((packed.size() + kPart - 1) / kPart);
    for (int i = 0; i < parts; i++)
        server->send({{QStringLiteral("type"), QStringLiteral("crash_report")},
                      {QStringLiteral("report"), id},
                      {QStringLiteral("part"), i},
                      {QStringLiteral("parts"), parts},
                      {QStringLiteral("data"), QString::fromLatin1(packed.mid(qsizetype(i) * kPart, kPart))}});
    QFile mark(QDir(reportDir).filePath(QStringLiteral("sent.txt")));
    if (mark.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
        mark.write(QStringLiteral("sent to %1 on %2 as %3\n")
                       .arg(server->address(), QDateTime::currentDateTime().toString(Qt::ISODate), id)
                       .toUtf8());
    applog::write(QStringLiteral("crash report: sent %1 (%2 bytes in %3 parts) as %4")
                      .arg(native(reportDir)).arg(text.size()).arg(parts).arg(id));
    return true;
}
