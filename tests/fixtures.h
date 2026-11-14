/*
 * Copyright (C) 2026 eos-mainline contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The frames Gadgetbridge writes (the gb repo's app/src/test/resources/asteroidos/frames/*.txt,
 * kept in tests/fixtures/ with a copy of the JSON): one write per line, with
 * backslash, line feed and carriage return escaped as \\, \n and \r. The
 * btsyncd tests feed these lines to the characteristics, so a change in what
 * the phone writes shows here as well as in the phone's own tests.
 */

#ifndef BTSYNCD_TEST_FIXTURES_H
#define BTSYNCD_TEST_FIXTURES_H

#include <QByteArray>
#include <QFile>
#include <QList>
#include <cstdio>
#include <cstdlib>

#ifndef FIXTURE_DIR
#error "FIXTURE_DIR is set by tests/CMakeLists.txt"
#endif

static inline QByteArray fixtureUnescape(const QByteArray &line)
{
    QByteArray out;
    for (int i = 0; i < line.size(); i++) {
        if (line.at(i) == '\\' && i + 1 < line.size()) {
            const char c = line.at(++i);
            out += c == 'n' ? '\n' : c == 'r' ? '\r' : c;
        } else {
            out += line.at(i);
        }
    }
    return out;
}

static inline QList<QByteArray> fixtureFrames(const char *name)
{
    QFile f(QStringLiteral(FIXTURE_DIR "/") + QLatin1String(name));
    if (!f.open(QIODevice::ReadOnly)) {
        fprintf(stderr, "FAIL cannot read the fixture %s\n", name);
        exit(1);
    }
    QList<QByteArray> frames;
    for (const QByteArray &line : f.readAll().split('\n')) {
        if (!line.isEmpty())
            frames << fixtureUnescape(line);
    }
    return frames;
}

static inline QByteArray fixtureFile(const char *name)
{
    QFile f(QStringLiteral(FIXTURE_DIR "/") + QLatin1String(name));
    if (!f.open(QIODevice::ReadOnly)) {
        fprintf(stderr, "FAIL cannot read the fixture %s\n", name);
        exit(1);
    }
    return f.readAll();
}

#endif
