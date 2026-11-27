/*
 * Copyright (C) 2026 - Ed Beroset <beroset@ieee.org>
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

#include <MDConfItem>
#include <QDateTime>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDebug>
#include <QDBusVariant>
#include <QRegularExpression>
#include <QTimeZone>
#include "settime.h"

static const char TIMEDATE_SERVICE[] = "org.freedesktop.timedate1";
static const char TIMEDATE_PATH[] = "/org/freedesktop/timedate1";

bool setSystemTime(const QDateTime& dateTime) {
    QDBusInterface interface("org.freedesktop.timedate1",
                             "/org/freedesktop/timedate1",
                             "org.freedesktop.timedate1",
                             QDBusConnection::systemBus());

    const QDateTime utcDateTime = dateTime.toUTC();

    const qint64 usec_utc{utcDateTime.toSecsSinceEpoch() * 1'000'000};

    constexpr bool isAdmin{false};
    constexpr bool adjustNTP{false};

    QDBusMessage reply = interface.call("SetTime", usec_utc, isAdmin, adjustNTP);

    if (reply.type() == QDBusMessage::ErrorMessage) {
        qWarning("Failed to set time: %s", reply.errorMessage().toLocal8Bit().constData());
        return false;
    }

    return true;
}


bool autoTimeEnabled()
{
    return MDConfItem(QString::fromLatin1(AutoTimeKey)).value(true).toBool();
}

bool autoTimeZoneEnabled()
{
    return MDConfItem(QString::fromLatin1(AutoTimeZoneKey)).value(true).toBool();
}

bool setSystemTimeFromPhone(const QDateTime& dateTime)
{
    if (!autoTimeEnabled()) {
        qInfo("Time from the phone not applied: Automatic date & time is off");
        return false;
    }
    if (!setSystemTime(dateTime))
        return false;
    // once: the key is written only when it is not true yet
    MDConfItem phoneSet(QString::fromLatin1(PhoneSetKey));
    if (!phoneSet.value(false).toBool())
        phoneSet.set(true);
    return true;
}

QString systemTimeZoneName()
{
    QDBusMessage get = QDBusMessage::createMethodCall(QString::fromLatin1(TIMEDATE_SERVICE), QString::fromLatin1(TIMEDATE_PATH),
                                                      QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("Get"));
    get << QString::fromLatin1(TIMEDATE_SERVICE) << QStringLiteral("Timezone");
    const QDBusMessage reply = QDBusConnection::systemBus().call(get, QDBus::Block, 5000);
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty())
        return QString();
    return reply.arguments().first().value<QDBusVariant>().variant().toString();
}

bool setSystemTimeZone(const QString &zone)
{
    QDBusMessage set = QDBusMessage::createMethodCall(QString::fromLatin1(TIMEDATE_SERVICE), QString::fromLatin1(TIMEDATE_PATH),
                                                      QString::fromLatin1(TIMEDATE_SERVICE), QStringLiteral("SetTimezone"));
    set << zone << false;   // not interactive
    const QDBusMessage reply = QDBusConnection::systemBus().call(set, QDBus::Block, 5000);
    if (reply.type() == QDBusMessage::ErrorMessage) {
        qWarning("Failed to set the time zone: %s", reply.errorMessage().toLocal8Bit().constData());
        return false;
    }
    return reply.type() == QDBusMessage::ReplyMessage;
}

QString zoneForPhone(const QString &zoneId, bool hasOffset, int offsetSecs)
{
    // an IANA name: letters, digits and _+-/ in parts, no "..", at most 64 characters
    static const QRegularExpression name(QStringLiteral("^[A-Za-z0-9_+-]+(/[A-Za-z0-9_+-]+)*$"));
    if (!zoneId.isEmpty() && zoneId.size() <= 64 && name.match(zoneId).hasMatch()
            && QTimeZone::isTimeZoneIdAvailable(zoneId.toLatin1()))
        return zoneId;
    // the Etc zones are whole hours, UTC-12 to UTC+14, their sign the other way round
    if (hasOffset && offsetSecs % 3600 == 0 && offsetSecs >= -12 * 3600 && offsetSecs <= 14 * 3600) {
        const int hours = offsetSecs / 3600;
        const QString etc = hours == 0 ? QStringLiteral("Etc/GMT")
                : QStringLiteral("Etc/GMT%1%2").arg(hours > 0 ? QLatin1Char('-') : QLatin1Char('+')).arg(qAbs(hours));
        if (QTimeZone::isTimeZoneIdAvailable(etc.toLatin1()))
            return etc;
    }
    return QString();
}

bool setSystemTimeZoneFromPhone(const QString &zone)
{
    if (zone.isEmpty())
        return false;
    if (!autoTimeZoneEnabled()) {
        qInfo("Time zone from the phone not applied: Automatic time zone is off");
        return false;
    }
    if (systemTimeZoneName() == zone)
        return true;
    if (!setSystemTimeZone(zone))
        return false;
    // no names in the log: the zone is where the phone is
    qInfo("Time zone set from the phone");
    return true;
}
