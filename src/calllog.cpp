/*
 * Copyright (C) 2026 eos-mainline contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "calllog.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>

static const int MaxTextChars = 120;
// The longest call that is recorded (a card whose removal came a day late is not 3 days long)
static const int MaxSeconds = 24 * 3600;

static QString defaultFile()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
           + QStringLiteral("/asteroid-btsyncd/calllog.json");
}

CallLog::CallLog(const QString &file, QDBusConnection bus, QObject *parent)
    : QObject(parent), mFile(file.isEmpty() ? defaultFile() : file)
{
    load();
    if (bus.isConnected()
        && !bus.registerObject(QStringLiteral(BTSYNCD_CALLLOG_PATH), this,
                               QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllSignals))
        qWarning() << "CallLog: cannot register" << BTSYNCD_CALLLOG_PATH << "on the session bus";
}

qint64 CallLog::now() const
{
    return mClock ? mClock() : QDateTime::currentMSecsSinceEpoch();
}

// "+1 (555) 010-0100" -> "+15550100100"; "" when the text is not a number
QString CallLog::normalizedNumber(const QString &text)
{
    const QString t = text.trimmed();
    if (t.isEmpty() || t.size() > 40)
        return QString();
    QString out;
    for (const QChar c : t) {
        if (c.isDigit() || c == QLatin1Char('*') || c == QLatin1Char('#'))
            out += c;
        else if (c == QLatin1Char('+') && out.isEmpty())
            out += c;
        else if (c == QLatin1Char(' ') || c == QLatin1Char('-') || c == QLatin1Char('(') || c == QLatin1Char(')')
                 || c == QLatin1Char('.') || c.unicode() == 0xA0 || c.unicode() == 0x2011)
            continue;
        else
            return QString();
    }
    int digits = 0;
    for (const QChar c : out)
        digits += c.isDigit();
    return digits >= 3 ? out : QString();
}

void CallLog::parseCaller(const QString &summary, const QString &body, QString *name, QString *number)
{
    const QString s = summary.simplified();
    const QString sNumber = normalizedNumber(s);
    const QString bNumber = normalizedNumber(body);
    name->clear();
    number->clear();
    if (!bNumber.isEmpty()) {
        *number = bNumber;
        if (sNumber != bNumber)
            *name = s;
    } else if (!sNumber.isEmpty()) {
        *number = sNumber;
    } else {
        *name = s;
    }
    if (name->size() > MaxTextChars)
        *name = name->left(MaxTextChars);
}

bool CallLog::sameCaller(const QString &name1, const QString &number1, const QString &name2, const QString &number2)
{
    if (!number1.isEmpty() && !number2.isEmpty())
        return number1 == number2;
    if (!name1.isEmpty() && !name2.isEmpty())
        return name1.compare(name2, Qt::CaseInsensitive) == 0;
    // one side has a number only, the other a name only: the card words are the same text
    if (!number1.isEmpty() && !name2.isEmpty())
        return normalizedNumber(name2) == number1;
    if (!number2.isEmpty() && !name1.isEmpty())
        return normalizedNumber(name1) == number2;
    return false;
}

QByteArray CallLog::toJson(const QList<Entry> &entries)
{
    QJsonArray array;
    for (const Entry &e : entries) {
        QJsonObject o;
        o.insert(QStringLiteral("t"), double(e.t));
        o.insert(QStringLiteral("ty"), e.type);
        o.insert(QStringLiteral("n"), e.name);
        o.insert(QStringLiteral("nm"), e.number);
        o.insert(QStringLiteral("lb"), e.label);
        o.insert(QStringLiteral("d"), e.seconds);
        array.append(o);
    }
    return QJsonDocument(array).toJson(QJsonDocument::Compact);
}

QList<CallLog::Entry> CallLog::fromJson(const QByteArray &json)
{
    QList<Entry> list;
    const QJsonDocument doc = QJsonDocument::fromJson(json);
    if (!doc.isArray())
        return list;
    for (const QJsonValue &v : doc.array()) {
        const QJsonObject o = v.toObject();
        Entry e;
        e.t = qint64(o.value(QStringLiteral("t")).toDouble());
        e.type = o.value(QStringLiteral("ty")).toString();
        e.name = o.value(QStringLiteral("n")).toString().left(MaxTextChars);
        e.number = o.value(QStringLiteral("nm")).toString().left(40);
        e.label = o.value(QStringLiteral("lb")).toString().left(40);
        e.seconds = qBound(0, o.value(QStringLiteral("d")).toInt(), MaxSeconds);
        const QString &ty = e.type;
        if (e.t <= 0 || (ty != QLatin1String("in") && ty != QLatin1String("out") && ty != QLatin1String("missed")
                         && ty != QLatin1String("declined")))
            continue;
        list.append(e);
        if (list.size() == MaxEntries)
            break;
    }
    return list;
}

void CallLog::load()
{
    QFile f(mFile);
    if (!f.open(QIODevice::ReadOnly))
        return;
    mEntries = fromJson(f.read(1024 * 1024));
    qInfo() << "CallLog:" << mEntries.size() << "entries";
}

void CallLog::save()
{
    QDir().mkpath(QFileInfo(mFile).absolutePath());
    QSaveFile f(mFile);
    if (!f.open(QIODevice::WriteOnly)) {
        qWarning() << "CallLog: cannot write the log";
        return;
    }
    f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    f.write(toJson(mEntries));
    if (!f.commit())
        qWarning() << "CallLog: the log was not saved";
}

QString CallLog::Entries()
{
    return QString::fromUtf8(toJson(mEntries));
}

void CallLog::add(const Entry &entry)
{
    mEntries.prepend(entry);
    while (mEntries.size() > MaxEntries)
        mEntries.removeLast();
    save();
    // the kind and the length only: never a name or a number
    qInfo() << "CallLog: a" << entry.type << "call," << entry.seconds << "s;" << mEntries.size() << "entries";
    emit Changed();
}

void CallLog::callCard(int phoneId, const QString &summary, const QString &body,
                       const QStringList &phoneActions, const QString &packageName)
{
    if (!packageName.isEmpty())
        return;     // a VoIP app's call, not a phone call
    const bool ringing = phoneActions.contains(QLatin1String("answer")) || phoneActions.contains(QLatin1String("reject"));
    const bool inProgress = !ringing && phoneActions.contains(QLatin1String("end"));
    if (!ringing && !inProgress)
        return;
    QString name, number;
    parseCaller(summary, body, &name, &number);
    const qint64 t = now();

    auto it = mActive.find(phoneId);
    if (it == mActive.end()) {
        while (mActive.size() >= MaxActive) {
            auto oldest = mActive.begin();
            for (auto i = mActive.begin(); i != mActive.end(); ++i) {
                if (i.value().cameAt < oldest.value().cameAt)
                    oldest = i;
            }
            mActive.erase(oldest);
        }
        Active a;
        a.name = name;
        a.number = number;
        a.cameAt = t;
        if (ringing) {
            a.ringing = true;
        } else {
            a.type = QStringLiteral("out");
            a.answeredAt = t;
            // the ringing card of this call went a moment ago: it was answered
            for (int i = 0; i < mRingEnds.size(); i++) {
                const RingEnd &r = mRingEnds.at(i);
                if (t - r.endedAt > JoinMs || r.entry.type != QLatin1String("missed")
                        || !sameCaller(r.entry.name, r.entry.number, name, number))
                    continue;
                for (int j = 0; j < mEntries.size() && j < 4; j++) {
                    const Entry &e = mEntries.at(j);
                    if (e.t == r.entry.t && e.type == r.entry.type) {
                        a.type = QStringLiteral("in");
                        a.cameAt = e.t;
                        if (!e.name.isEmpty())
                            a.name = e.name;
                        if (!e.number.isEmpty())
                            a.number = e.number;
                        mEntries.removeAt(j);
                        save();
                        emit Changed();
                        break;
                    }
                }
                mRingEnds.removeAt(i);
                break;
            }
        }
        mActive.insert(phoneId, a);
        return;
    }
    Active &a = it.value();
    if (a.name.isEmpty() && !name.isEmpty())
        a.name = name;
    if (a.number.isEmpty() && !number.isEmpty())
        a.number = number;
    if (a.ringing && inProgress) {
        // the same card went from ringing to in progress: answered
        a.ringing = false;
        a.type = QStringLiteral("in");
        a.answeredAt = t;
    }
}

void CallLog::callAction(int phoneId, const QString &action)
{
    auto it = mActive.find(phoneId);
    if (it != mActive.end() && it->ringing && action == QLatin1String("reject"))
        it->declined = true;
}

void CallLog::callGone(int phoneId)
{
    auto it = mActive.find(phoneId);
    if (it == mActive.end())
        return;
    const Active a = it.value();
    mActive.erase(it);
    const qint64 t = now();
    Entry e;
    e.name = a.name;
    e.number = a.number;
    if (a.ringing) {
        e.t = a.cameAt;
        e.type = a.declined ? QStringLiteral("declined") : QStringLiteral("missed");
        add(e);
        RingEnd r;
        r.entry = e;
        r.endedAt = t;
        mRingEnds.prepend(r);
        while (mRingEnds.size() > 4)
            mRingEnds.removeLast();
        return;
    }
    e.t = a.cameAt;
    e.type = a.type;
    e.seconds = int(qBound(qint64(0), (t - a.answeredAt) / 1000, qint64(MaxSeconds)));
    add(e);
}

void CallLog::missedCallCard(int phoneId, const QString &summary, const QString &body, qint64 whenSeconds)
{
    if (mMissedSeen.contains(phoneId))
        return;
    mMissedSeen.append(phoneId);
    while (mMissedSeen.size() > 32)
        mMissedSeen.removeFirst();
    QString name, number;
    parseCaller(summary, QString(), &name, &number);
    const qint64 t = now();
    const qint64 when = whenSeconds > 0 ? whenSeconds * 1000 : t;
    // an old notification the phone sends again at a reconnection is not news
    if (when < t - 7LL * 24 * 3600 * 1000)
        return;
    for (const Entry &e : mEntries) {
        if (e.type != QLatin1String("missed") && e.type != QLatin1String("declined"))
            continue;
        if (qAbs(e.t - when) <= 120 * 1000 && sameCaller(e.name, e.number, name, number))
            return;
        if (e.t < when - 120 * 1000)
            break;      // newest first: nothing older can be within two minutes
    }
    Entry e;
    e.t = when;
    e.type = QStringLiteral("missed");
    e.name = name;
    e.number = number;
    // "Missed call (dot) Mobile": the label after the dot
    const int dot = body.indexOf(QChar(0x00B7));
    if (dot >= 0)
        e.label = body.mid(dot + 1).simplified().left(40);
    // keep the list ordered by time when the call was earlier than the newest entry
    int at = 0;
    while (at < mEntries.size() && mEntries.at(at).t > e.t)
        at++;
    mEntries.insert(at, e);
    while (mEntries.size() > MaxEntries)
        mEntries.removeLast();
    save();
    qInfo() << "CallLog: a missed call from the phone's notification;" << mEntries.size() << "entries";
    emit Changed();
}
