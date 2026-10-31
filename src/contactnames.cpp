/*
 * Copyright (C) 2026 Reuben Horner <reubarb.scons@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "contactnames.h"

#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QStandardPaths>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace ContactNames {

QString path()
{
    const QString env = qEnvironmentVariable("BTSYNCD_STT_CONTEXT");
    if (!env.isEmpty())
        return env;
    // as asteroid-assistant-stt: $XDG_DATA_HOME, else ~/.local/share
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
            + QStringLiteral("/asteroid-assistant/stt-context.txt");
}

QByteArray content(const QStringList &phrases)
{
    QByteArray out;
    for (const QString &p : phrases)
        out += p.toUtf8() + '\n';
    return out;
}

QByteArray hash()
{
    QFile f(path());
    if (!f.open(QIODevice::ReadOnly))
        return QByteArray();
    // the file is ours and small (at most 3000 x 97 bytes): read it whole
    const QByteArray data = f.read(MaxPhrases * (MaxPhraseBytes + 1) + 1);
    if (data.isEmpty())
        return QByteArray();
    return QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex().left(16);
}

QStringList clean(const QStringList &phrases)
{
    QStringList out;
    QSet<QString> seen;
    for (QString p : phrases) {
        for (QChar &c : p)
            if (c.category() == QChar::Other_Control || c.category() == QChar::Other_Format
                    || c == QLatin1Char('#') || c == QLatin1Char(':') || c.isSpace())
                c = QLatin1Char(' ');
        p = p.simplified();
        // at most MaxPhraseBytes of UTF-8, cut between characters
        while (p.toUtf8().size() > MaxPhraseBytes)
            p.chop(p.size() > 1 && p.at(p.size() - 1).isLowSurrogate() ? 2 : 1);
        p = p.trimmed();
        if (p.isEmpty())
            continue;
        bool letter = false;
        for (const QChar c : p)
            letter |= c.isLetter();
        if (!letter)
            continue;
        const QString key = p.toCaseFolded();
        if (seen.contains(key))
            continue;
        seen.insert(key);
        out << p;
        if (out.size() >= MaxPhrases)
            break;
    }
    return out;
}

static bool fail(QString *error, const QString &what)
{
    if (error)
        *error = what + QStringLiteral(": ") + QString::fromLocal8Bit(strerror(errno));
    return false;
}

bool write(const QStringList &phrases, QString *error)
{
    const QString file = path();
    const QFileInfo info(file);
    const QString dir = info.absolutePath();
    if (!QDir().mkpath(dir))
        return fail(error, QStringLiteral("cannot make the directory"));
    // only ceres reads it: the directory 0700, the file 0600
    ::chmod(QFile::encodeName(dir).constData(), 0700);
    const QByteArray data = content(clean(phrases));
    const QByteArray tmp = QFile::encodeName(dir + QStringLiteral("/.stt-context.txt.XXXXXX"));
    QByteArray name = tmp;
    const int fd = ::mkostemp(name.data(), O_CLOEXEC);   // created 0600
    if (fd < 0)
        return fail(error, QStringLiteral("cannot create the temporary file"));
    ::fchmod(fd, 0600);
    qint64 done = 0;
    while (done < data.size()) {
        const ssize_t n = ::write(fd, data.constData() + done, size_t(data.size() - done));
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            ::close(fd);
            ::unlink(name.constData());
            return fail(error, QStringLiteral("cannot write"));
        }
        done += n;
    }
    if (::fsync(fd) != 0 || ::close(fd) != 0) {
        ::unlink(name.constData());
        return fail(error, QStringLiteral("cannot sync"));
    }
    if (::rename(name.constData(), QFile::encodeName(file).constData()) != 0) {
        ::unlink(name.constData());
        return fail(error, QStringLiteral("cannot rename"));
    }
    // the rename itself on disk
    const int dfd = ::open(QFile::encodeName(dir).constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd >= 0) {
        ::fsync(dfd);
        ::close(dfd);
    }
    return true;
}

bool remove(const char *why)
{
    const QByteArray file = QFile::encodeName(path());
    if (::unlink(file.constData()) != 0)
        return false;
    qInfo() << "Assistant: the contact names are removed (" << why << ")";
    return true;
}

bool Assembler::add(const QString &generation, int index, int total, const QStringList &phrases)
{
    if (generation.isEmpty() || total < 1 || total > MaxParts || index < 0 || index >= total)
        return false;
    if (generation != mGeneration || total != mTotal) {
        // a new list: what came of an older one is dropped
        mGeneration = generation;
        mTotal = total;
        mParts.clear();
    }
    mParts.insert(index, phrases);
    return mParts.size() == mTotal;
}

QStringList Assembler::take()
{
    QStringList all;
    if (mParts.size() == mTotal)
        for (auto it = mParts.constBegin(); it != mParts.constEnd(); ++it)
            all += it.value();
    reset();
    return all;
}

void Assembler::reset()
{
    mGeneration.clear();
    mTotal = 0;
    mParts.clear();
}

}
