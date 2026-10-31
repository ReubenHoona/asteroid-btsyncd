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

#ifndef CONTACTNAMES_H
#define CONTACTNAMES_H

#include <QByteArray>
#include <QMap>
#include <QString>
#include <QStringList>

/*
 * The phone's contact names for the watch's speech recogniser (the user agreed,
 * 2026-10-31): names only, never numbers. Gadgetbridge sends them over the
 * assistant relay (assistantprotocol.h, <cn>); they are written to
 *   $BTSYNCD_STT_CONTEXT (tests), else ~/.local/share/asteroid-assistant/stt-context.txt
 *   ($XDG_DATA_HOME if set)
 * the file asteroid-assistant-stt reads its user context from (one phrase per
 * line) and asteroid-assistant matches contact names against. btsyncd runs as
 * ceres: the file is ceres', mode 0600 in a 0700 directory, written atomically
 * (a temporary file in the same directory, fsync, rename). It is removed when
 * the phone says the setting is off and when no phone is paired any more.
 * Only counts go to the log, never a name.
 */
namespace ContactNames {

// asteroid-assistant-stt's limits: at most 3000 phrases of 96 bytes
const int MaxPhrases = 3000;
const int MaxPhraseBytes = 96;
// a list comes in parts of at most a few kB (the reply characteristic joins a
// part until </cn>, at most 8 kB): 1000 names are about 5 parts
const int MaxParts = 128;

QString path();
// What the watch has: the first 16 hex digits of the SHA-256 of the file, "" if none
QByteArray hash();
// The phrases as the file has them: one per line, cleaned (see clean())
QByteArray content(const QStringList &phrases);
// Write the list (atomically, 0600); false on an error (why in *error)
bool write(const QStringList &phrases, QString *error = nullptr);
// Remove the list; true if there was one
bool remove(const char *why);
// No control characters, no "#" (a comment) or ":" (a bonus), spaces folded,
// at most MaxPhraseBytes of UTF-8, no repeats (ignoring case), at most MaxPhrases
QStringList clean(const QStringList &phrases);

// The parts of one list as they come (a new generation starts over)
class Assembler
{
public:
    // A part: true when it completed its list (then take() gives it)
    bool add(const QString &generation, int index, int total, const QStringList &phrases);
    QStringList take();
    void reset();
    int partsHeld() const { return mParts.size(); }

private:
    QString mGeneration;
    int mTotal = 0;
    QMap<int, QStringList> mParts;
};

}

#endif // CONTACTNAMES_H
