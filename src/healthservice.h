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
 * Heart rate and step history from asteroid-sensorlogd (see
 * PROTOCOL-health-sync.md). The phone writes
 *   <req><ty>hr|steps|hrr</ty><si>SINCE</si><mx>MAX</mx></req>
 * and gets the records after SINCE on the data characteristic.
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
