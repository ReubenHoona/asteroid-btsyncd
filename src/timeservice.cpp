/*
 * Copyright (C) 2017 - Florent Revest <revestflo@gmail.com>
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
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDateTime>
#include <QTimeZone>

#include "settime.h"
#include "timeservice.h"
#include "characteristic.h"
#include "common.h"

bool TimeSetChrc::decode(const QByteArray &value, QDateTime *time, int *flag24)
{
    Frame frame;
    const bool ok = decode(value, &frame);
    *flag24 = frame.flag24;
    if (ok)
        *time = frame.time;
    return ok;
}

bool TimeSetChrc::decode(const QByteArray &value, Frame *frame)
{
    *frame = Frame();
    if (value.size() < 6) // year, month, day, hour, minute, second
        return false;
    int year = 1900 + (unsigned char) value[0];
    int month = 1+ (unsigned char) value[1];
    int day = (unsigned char) value[2];
    int hour = (unsigned char) value[3];
    int minute = (unsigned char) value[4];
    int second = (unsigned char) value[5];

    const QDate date(year, month, day);
    const QTime clock(hour, minute, second);
    if (!date.isValid() || !clock.isValid())
        return false;
    if (value.size() >= 7) {
        const unsigned char f = (unsigned char) value[6];
        if (f <= 1)
            frame->flag24 = f;
    }
    if (value.size() >= 12) {
        const qint32 offset = qint32(quint32((unsigned char) value[7]) | (quint32((unsigned char) value[8]) << 8)
                                     | (quint32((unsigned char) value[9]) << 16) | (quint32((unsigned char) value[10]) << 24));
        // real offsets are UTC-12 to UTC+14; anything else is no offset
        if (offset >= -14 * 3600 && offset <= 14 * 3600) {
            frame->hasOffset = true;
            frame->offsetSecs = offset;
        }
        const int n = (unsigned char) value[11];
        if (n > 0 && value.size() >= 12 + n) {
            const QByteArray name = value.mid(12, n);
            bool ascii = true;
            for (const char c : name)
                ascii = ascii && c > ' ' && c < 127;
            if (ascii)
                frame->zoneId = QString::fromLatin1(name);
        }
    }
    if (frame->hasOffset) {
        // the instant the phone's wall time is
        frame->time = QDateTime(date, clock, QTimeZone::utc()).addSecs(-frame->offsetSecs);
    } else {
        QDateTime local(date, clock);
        local.setTimeZone(QTimeZone::systemTimeZone());
        if (!local.isValid())
            return false;
        frame->time = local;
    }
    return true;
}

int TimeSetChrc::frameSize(const QByteArray &start)
{
    if (start.size() < 12)
        return start.size();
    return 12 + (unsigned char) start[11];
}

void TimeSetChrc::WriteValue(QByteArray value, QVariantMap options)
{
    // a long write comes in parts at their offsets: the frame is read when it is whole
    const int offset = options.value("offset", 0).toInt();
    if (offset == 0) {
        mPending = value;
    } else if (!mPending.isEmpty() && offset == mPending.size() && mPending.size() + value.size() <= MaxFrame) {
        mPending += value;
    } else {
        qWarning("Time: a part of a write that does not follow the one before, ignored");
        mPending.clear();
        return;
    }
    if (mPending.size() < frameSize(mPending))
        return;     // the rest is still coming
    const QByteArray frameBytes = mPending;
    mPending.clear();
    if (!hasMinLength(frameBytes, 6)) // year, month, day, hour, minute, second
        return;
    Frame frame;
    if (!decode(frameBytes, &frame)) {
        qWarning("Time: not a date, ignored");
        return;
    }
    // the zone first: the clock's instant does not depend on it, the time shown does
    if (frame.hasOffset || !frame.zoneId.isEmpty())
        emit phoneTimeZone(frame.zoneId, frame.hasOffset, frame.offsetSecs);
    setSystemTimeFromPhone(frame.time);
    if (frame.flag24 >= 0)
        emit phone24hFormat(frame.flag24 == 1);
}

TimeService::TimeService(int index, QDBusConnection bus, QObject *parent) : Service(bus, index, TIME_UUID, parent)
{
    TimeSetChrc *set = new TimeSetChrc(bus, 0, this);
    addCharacteristic(set);
    mFromPhone = new MDConfItem(FromPhoneKey, this);
    mPhone24h = new MDConfItem(Phone24hKey, this);
    mUse12h = new MDConfItem(Use12hKey, this);
    mAutoZone = new MDConfItem(QString::fromLatin1(AutoTimeZoneKey), this);
    mPhoneZone = new MDConfItem(PhoneZoneKey, this);
    mAutoZoneOn = mAutoZone->value(true).toBool();
    connect(set, &TimeSetChrc::phone24hFormat, this, &TimeService::onPhoneFormat);
    connect(set, &TimeSetChrc::phoneTimeZone, this, &TimeService::onPhoneZone);
    // the setting switched on later takes the phone's last flag at once
    connect(mFromPhone, &MDConfItem::valueChanged, this, &TimeService::applyFormat);
    // and Automatic time zone the phone's last zone (a change of the key, no polling)
    connect(mAutoZone, &MDConfItem::valueChanged, this, &TimeService::onAutoZoneChanged);
    applyFormat();
}

void TimeService::phoneConnectedChanged(bool connected)
{
    mPhoneConnected = connected;
}

void TimeService::onPhoneZone(const QString &zoneId, bool hasOffset, int offsetSecs)
{
    const QString zone = zoneForPhone(zoneId, hasOffset, offsetSecs);
    if (zone.isEmpty()) {
        qInfo("The phone's time zone is not in this watch's zone database: the watch's is kept");
        return;
    }
    if (mPhoneZone->value().toString() != zone)
        mPhoneZone->set(zone);
    setSystemTimeZoneFromPhone(zone);
}

void TimeService::onAutoZoneChanged()
{
    const bool on = mAutoZone->value(true).toBool();
    const bool switchedOn = on && !mAutoZoneOn;
    mAutoZoneOn = on;
    if (!switchedOn || !mPhoneConnected)
        return;
    const QString zone = mPhoneZone->value().toString();
    if (!zone.isEmpty())
        setSystemTimeZoneFromPhone(zone);
}

void TimeService::onPhoneFormat(bool is24h)
{
    if (!mPhone24h->value().isValid() || mPhone24h->value().toBool() != is24h)
        mPhone24h->set(is24h);
    applyFormat();
}

void TimeService::applyFormat()
{
    if (!mFromPhone->value(false).toBool())
        return;
    const QVariant phone = mPhone24h->value();
    if (!phone.isValid())
        return; // the phone has not said (an older Gadgetbridge)
    const bool want12 = !phone.toBool();
    if (mUse12h->value(true).toBool() != want12 || !mUse12h->value().isValid())
        mUse12h->set(want12);
}
