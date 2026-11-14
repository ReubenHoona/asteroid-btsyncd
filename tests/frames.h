/*
 * Copyright (C) 2026 eos-mainline contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef FRAMES_H
#define FRAMES_H

#include <QByteArray>
#include <QFile>
#include <QList>
#include <QString>

#ifndef FRAMES_DIR
#define FRAMES_DIR "frames"
#endif

/*
 * Frames recorded from Gadgetbridge's tests (its AsteroidOSFixtures): one write per line of
 * frames/NAME.frames.txt, the characters \ newline and carriage return escaped as \\ \n \r.
 * Copies of app/src/test/resources/asteroidos/frames of the gb repo; when Gadgetbridge's
 * frames change, copy them again and these tests say what btsyncd must follow.
 */
static inline QList<QByteArray> loadFrames(const char *name)
{
    QList<QByteArray> frames;
    QFile f(QString::fromLatin1(FRAMES_DIR) + "/" + QString::fromLatin1(name) + ".frames.txt");
    if(!f.open(QIODevice::ReadOnly))
        return frames;
    const QList<QByteArray> lines = f.readAll().split('\n');
    for(const QByteArray &line : lines) {
        if(line.isEmpty())
            continue;
        QByteArray out;
        for(int i = 0; i < line.size(); i++) {
            if(line.at(i) == '\\' && i + 1 < line.size()) {
                const char n = line.at(++i);
                out += n == 'n' ? '\n' : n == 'r' ? '\r' : n;
            } else {
                out += line.at(i);
            }
        }
        frames << out;
    }
    return frames;
}

#endif // FRAMES_H
