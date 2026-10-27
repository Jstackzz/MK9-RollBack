#pragma once
// Xbox disc images, read straight from the image file: the file table, one file, or every file into a folder. The
// Xbox 360's game discs keep their files in the Xbox Game Disc file system (GDFX), which is the original Xbox's
// XDVDFS: the volume descriptor "MICROSOFT*XBOX*MEDIA" at sector 32 of the game partition, each folder a binary tree
// of entries. Where the game partition starts depends on the disc: 0 (an XISO, the partition alone), 0xFD90000 (a
// whole XGD2 dump, Mortal Kombat Komplete Edition's), 0x2080000 (XGD3), 0x18300000 (an original Xbox XGD1 dump).
// The launcher's disc step, without Python (tools/extract_iso.py reads the same).

#include <QFile>
#include <QList>
#include <QString>

#include <atomic>

namespace xiso {

struct Entry {
    QString path;  // relative, '/' separated
    quint32 sector = 0;
    quint32 size = 0;
    bool dir = false;
};

class Image {
public:
    bool open(const QString& path, QString* error);
    const QList<Entry>& entries() const { return entries_; }
    qint64 base() const { return base_; }  // where the game partition starts in the image
    QString layout() const;                // "XGD2 disc", "XISO", ...: what that offset says the image is
    qint64 totalBytes() const;  // of the files
    int fileCount() const;
    const Entry* find(const QString& path) const;  // case-insensitive
    bool read(const Entry& e, QByteArray* out, QString* error);
    // every file under outDir (each written as <name>.part, then renamed; a file already there with the right size is
    // kept, so a cancelled extraction goes on where it stopped); bytesDone and filesDone count up as it goes; false on
    // an error or when cancel is set
    bool extractAll(const QString& outDir, const std::atomic<bool>& cancel, std::atomic<qint64>& bytesDone,
                    std::atomic<int>& filesDone, QString* error);

private:
    bool findBase(QString* error);
    bool walk(quint32 sector, quint32 size, const QString& prefix, int depth, QString* error);

    QFile f_;
    qint64 base_ = 0;
    QList<Entry> entries_;
};

}  // namespace xiso
