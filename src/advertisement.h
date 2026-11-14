/*
 * Copyright (C) 2016 - Florent Revest <revestflo@gmail.com>
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

#ifndef ADVERTISEMENT_H
#define ADVERTISEMENT_H

#include <QObject>
#include <QDBusAbstractAdaptor>
#include <QDBusObjectPath>
#include <QDBusConnection>

#include "common.h"

class Advertisement : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", LE_ADVERTISEMENT_IFACE)
    Q_PROPERTY(QString Type READ getType())
    Q_PROPERTY(QStringList ServiceUUIDs READ getServiceUUIDs())
    Q_PROPERTY(QStringList SolicitUUIDs READ getSolicitUUIDs())
    Q_PROPERTY(QMap<unsigned int, QByteArray> ManufacturerData READ getManufacturerData())
    Q_PROPERTY(QMap<QString, QByteArray> ServiceData READ getServiceData())
    Q_PROPERTY(bool IncludeTxPower READ getIncludeTxPower())
    Q_PROPERTY(bool Discoverable READ getDiscoverable())
    // Advertising intervals in milliseconds (BlueZ 5.62+): see the schedule below
    Q_PROPERTY(uint MinInterval READ getMinInterval())
    Q_PROPERTY(uint MaxInterval READ getMaxInterval())

public:
    explicit Advertisement(QDBusConnection bus = QDBusConnection::systemBus(), QObject *parent = 0);
    QDBusObjectPath getPath();

    /*
     * The advertising schedule after the phone left (or at start): the controller advertises
     * fast for the first 10 s, then the interval steps up as the time since the phone left
     * grows (10, 20, 40, 80, 160, 320 and 540 s), to a slow cap reached after nine minutes.
     * Stock's watch retries its connection 2 s apart five times and then doubles the wait to a
     * cap of 300 s (CompanionProxyShard), the phone's retries being the other half; this is the
     * advertising half: the phone finds the watch fast while it has just left, and the radio
     * idles when it has been gone for long. The interval is controller work: BlueZ gets the new
     * interval with a new registration at each step (seven in nine minutes, from one timer
     * that is set again after each step); no polling. Each connection starts afresh.
     */
    struct Stage { int afterMs; uint minMs, maxMs; };
    static int stageCount();
    static const Stage &stage(int index);
    // The first step that is not past: the stage after this long without the phone
    static int stageAt(qint64 sinceMs);
    int stageIndex() const { return mStage; }
    // The intervals of the next registration
    void setStage(int index);
    // BlueZ (or the controller) did not take the intervals: advertise at BlueZ's own interval
    // from now on
    void useDefaultIntervals();
    bool defaultIntervals() const { return mDefaultIntervals; }

private:
    void addServiceUuid(QString uuid);
    void addSolicitUuid(QString uuid);
    void addServiceData(QString uuid, QByteArray data);
    void addManufacturerData(unsigned int manufCode, QByteArray data);

    QString getType();
    QStringList getServiceUUIDs();
    QStringList getSolicitUUIDs();
    QMap<unsigned int, QByteArray> getManufacturerData();
    QMap<QString, QByteArray> getServiceData();
    bool getIncludeTxPower();
    bool getDiscoverable();
    uint getMinInterval();
    uint getMaxInterval();

    QDBusConnection mBus;
    int mStage = 0;
    bool mDefaultIntervals = false;
    QString mPath, mAdType;
    QStringList mServiceUuids, mSolicitUuids;
    QMap<unsigned int, QByteArray> mManufacturerData;
    QMap<QString, QByteArray> mServiceData;
    bool mIncludeTxPower;
    bool mDiscoverable;

public slots:
    void Release();
};

#endif // ADVERTISEMENT_H
