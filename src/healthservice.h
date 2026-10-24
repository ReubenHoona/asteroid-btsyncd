/*
 * Copyright (C) 2026 - Reuben Horner <reubarb.scons@gmail.com>
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

#ifndef HEALTHSERVICE_H
#define HEALTHSERVICE_H

#include <QByteArray>
#include <QObject>
#include <QString>

#include "service.h"

/*
 * Watch to phone: the answer to a request, as XML in notifications of at
 * most MTU - 3 bytes; the phone joins them until </data>.
 */
class HealthDataChrc : public Characteristic
{
    Q_OBJECT
    Q_PROPERTY(QByteArray Value READ getValue NOTIFY valueChanged)

public:
    HealthDataChrc(QDBusConnection bus, int index, Service *service);
    void send(const QByteArray &xml, int mtu);

public slots:
    void StartNotify() override {}
    void StopNotify() override {}

signals:
    void valueChanged();

private slots:
    void emitPropertiesChanged();

private:
    QByteArray mValue;
    QByteArray getValue() { return mValue; }
};

/*
 * Heart rate and step history from asteroid-sensorlogd. The phone writes
 *   <req><ty>hr|steps</ty><si>SINCE</si><mx>MAX</mx></req>
 * and gets the records after SINCE (epoch seconds) on the data
 * characteristic, oldest first:
 *   <data><ty>TYPE</ty><t0>T0</t0><n>N</n><mo>MORE</mo><r>dt,value;...</r></data>
 * dt is the time since the record before (the first: since T0); MORE is 1
 * when the phone should ask again from the last record. Values are bpm, or
 * the steps taken since the record before. Errors:
 *   <data><ty>TYPE</ty><er>type|request</er></data>
 */
class HealthService : public Service
{
    Q_OBJECT
public:
    explicit HealthService(int index, QDBusConnection bus, QObject *parent = 0);

    // Answers after the write returns, so the phone sees its write
    // acknowledged before the data
    void request(const QString &type, qint64 since, int max, int mtu);

private slots:
    void sendPending();

private:
    HealthDataChrc *mData;
    QByteArray mPending;
    int mPendingMtu = 23;
};

#endif // HEALTHSERVICE_H
