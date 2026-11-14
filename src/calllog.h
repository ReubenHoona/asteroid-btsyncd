/*
 * Copyright (C) 2026 eos-mainline contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef CALLLOG_H
#define CALLLOG_H

#include <QDBusConnection>
#include <QHash>
#include <QList>
#include <QObject>
#include <QString>

#include <functional>

#include "callobserver.h"
#include "common.h"

/*
 * The watch's call log, for the Phone app's Recents (asteroid-phone, calls-26):
 * org.asteroidos.btsyncd.CallLog on /org/asteroidos/btsyncd/calllog (session bus).
 *   Entries() -> s   a JSON array, newest first, of
 *                    {"t": ms since the epoch the call came or was placed,
 *                     "ty": "in" (answered), "out", "missed" or "declined",
 *                     "n": the name or "", "nm": the number or "", "lb": a label or "",
 *                     "d": seconds of the call (0 when it was not answered)}
 *   Changed()        an entry was added (the app reads Entries() again)
 *
 * The record is btsyncd's own, made from the phone's call cards (CallObserver, called by
 * the notification service): there is no act for it on the phone. A call that rings and is
 * then answered (the card of the ringing call goes, the card of the call in progress
 * comes) is one "in" entry, written when the call ends; a ringing card that goes without
 * that is "declined" when the user pressed Decline on the watch, else "missed" (the
 * phone's own decline cannot be told from a missed call); a card of a call in progress that
 * no ringing call led to is an "out" call (its duration counts from the card, which the
 * phone posts when the call is placed). The phone's "Missed call" notification adds a
 * missed entry only when no entry for that caller is there already.
 * At most MaxEntries are kept, in one small file (0600) that is written when an entry is
 * added and never on a timer. No name or number goes to the log.
 */
class CallLog : public QObject, public CallObserver
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.asteroidos.btsyncd.CallLog")

public:
    static const int MaxEntries = 200;
    // A ringing card that goes and a call in progress that comes within this long are one call
    static const int JoinMs = 15000;
    // Calls that are tracked at once (a card whose removal never came is forgotten past this)
    static const int MaxActive = 8;

    struct Entry {
        qint64 t = 0;
        QString type, name, number, label;
        int seconds = 0;
    };

    // file: where the log is kept; empty: ~/.local/share/asteroid-btsyncd/calllog.json.
    // bus: where the object is registered (the session bus)
    explicit CallLog(const QString &file = QString(), QDBusConnection bus = QDBusConnection::sessionBus(),
                     QObject *parent = nullptr);

    // The clock (ms since the epoch), replaceable for the tests
    void setClock(const std::function<qint64()> &clock) { mClock = clock; }

    QList<Entry> entries() const { return mEntries; }

    // CallObserver
    void callCard(int phoneId, const QString &summary, const QString &body,
                  const QStringList &phoneActions, const QString &packageName) override;
    void callAction(int phoneId, const QString &action) override;
    void callGone(int phoneId) override;
    void missedCallCard(int phoneId, const QString &summary, const QString &body, qint64 whenSeconds) override;

    // The caller as the phone's call card words it: summary (the name, or the number when
    // there is none) and body (the number). A text is a number when it has only digits, + * #
    // and the usual separators.
    static void parseCaller(const QString &summary, const QString &body, QString *name, QString *number);
    static QString normalizedNumber(const QString &text);
    static QByteArray toJson(const QList<Entry> &entries);
    static QList<Entry> fromJson(const QByteArray &json);

public slots:
    QString Entries();

signals:
    void Changed();

private:
    struct Active {
        qint64 cameAt = 0, answeredAt = 0;
        bool ringing = false, declined = false;
        QString type, name, number;
    };
    struct RingEnd {
        Entry entry;
        qint64 endedAt = 0;
    };

    qint64 now() const;
    void add(const Entry &entry);
    void save();
    void load();
    static bool sameCaller(const QString &name1, const QString &number1, const QString &name2, const QString &number2);

    QString mFile;
    std::function<qint64()> mClock;
    QList<Entry> mEntries;
    QHash<int, Active> mActive;
    // The ringing calls whose card went in the last JoinMs: the one that is answered turns up
    // as a card of a call in progress right after
    QList<RingEnd> mRingEnds;
    QList<int> mMissedSeen;
};

#endif // CALLLOG_H
