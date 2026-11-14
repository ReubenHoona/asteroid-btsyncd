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
    *flag24 = -1;
    if (value.size() < 6) // year, month, day, hour, minute, second
        return false;
    int year = 1900 + (unsigned char) value[0];
    int month = 1+ (unsigned char) value[1];
    int day = (unsigned char) value[2];
    int hour = (unsigned char) value[3];
    int minute = (unsigned char) value[4];
    int second = (unsigned char) value[5];

    QDateTime newTime(QDate(year, month, day), QTime(hour, minute, second));
    if (!newTime.isValid())
        return false;
    newTime.setTimeZone(QTimeZone::systemTimeZone());
    *time = newTime;
    if (value.size() >= 7) {
        const unsigned char f = (unsigned char) value[6];
        if (f <= 1)
            *flag24 = f;
    }
    return true;
}

void TimeSetChrc::WriteValue(QByteArray value, QVariantMap)
{
    if (!hasMinLength(value, 6)) // year, month, day, hour, minute, second
        return;
    QDateTime newTime;
    int flag24;
    if (!decode(value, &newTime, &flag24)) {
        qWarning("Time: not a date, ignored");
        return;
    }
    setSystemTime(newTime);
    if (flag24 >= 0)
        emit phone24hFormat(flag24 == 1);
}

TimeService::TimeService(int index, QDBusConnection bus, QObject *parent) : Service(bus, index, TIME_UUID, parent)
{
    TimeSetChrc *set = new TimeSetChrc(bus, 0, this);
    addCharacteristic(set);
    mFromPhone = new MDConfItem(FromPhoneKey, this);
    mPhone24h = new MDConfItem(Phone24hKey, this);
    mUse12h = new MDConfItem(Use12hKey, this);
    connect(set, &TimeSetChrc::phone24hFormat, this, &TimeService::onPhoneFormat);
    // the setting switched on later takes the phone's last flag at once
    connect(mFromPhone, &MDConfItem::valueChanged, this, &TimeService::applyFormat);
    applyFormat();
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
