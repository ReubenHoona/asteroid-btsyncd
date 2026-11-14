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

#include "advertisement.h"

Advertisement::Advertisement(QDBusConnection bus, QObject *parent) : QObject(parent), mBus(QDBusConnection::systemBus())
{
    mPath = ADVERTISEMENT_PATH_BASE;
    mBus = bus;
    mAdType = "peripheral";
    mIncludeTxPower = false;
    addServiceUuid("00000000-0000-0000-0000-00a57e401d05");
    mDiscoverable = true;

    bus.registerObject(mPath, this, QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllProperties);
}

QDBusObjectPath Advertisement::getPath()
{
    return QDBusObjectPath(mPath);
}

void Advertisement::addServiceUuid(QString uuid)
{
    mServiceUuids.append(uuid);
}

void Advertisement::addManufacturerData(unsigned int manufCode, QByteArray data)
{
    mManufacturerData.insert(manufCode, data);
}

void Advertisement::addServiceData(QString uuid, QByteArray data)
{
    mServiceData.insert(uuid, data);
}

QString Advertisement::getType()
{
    return mAdType;
}

QStringList Advertisement::getServiceUUIDs()
{
    return mServiceUuids;
}

QStringList Advertisement::getSolicitUUIDs()
{
    return mSolicitUuids;
}

QMap<unsigned int, QByteArray> Advertisement::getManufacturerData()
{
    return mManufacturerData;
}

QMap<QString, QByteArray> Advertisement::getServiceData()
{
    return mServiceData;
}

bool Advertisement::getIncludeTxPower()
{
    return mIncludeTxPower;
}

bool Advertisement::getDiscoverable()
{
    return mDiscoverable;
}

// ms after the phone left, minimum and maximum interval (ms). The fast stage is the 100 to 150 ms
// band Android's connect scans find quickest; the cap is 2.5 to 3 s.
static const Advertisement::Stage Stages[] = {
    {      0,  100,  150 },
    {  10000,  200,  300 },
    {  20000,  400,  600 },
    {  40000,  800, 1000 },
    {  80000, 1000, 1500 },
    { 160000, 1500, 2000 },
    { 320000, 2000, 2500 },
    { 540000, 2500, 3000 },
};

int Advertisement::stageCount()
{
    return int(sizeof(Stages) / sizeof(Stages[0]));
}

const Advertisement::Stage &Advertisement::stage(int index)
{
    return Stages[qBound(0, index, stageCount() - 1)];
}

int Advertisement::stageAt(qint64 sinceMs)
{
    int stage = 0;
    for(int i = 1; i < stageCount(); i++) {
        if(sinceMs >= Stages[i].afterMs)
            stage = i;
    }
    return stage;
}

void Advertisement::setStage(int index)
{
    mStage = qBound(0, index, stageCount() - 1);
}

void Advertisement::useDefaultIntervals()
{
    mDefaultIntervals = true;
}

// BlueZ's own interval (1.28 s) when the controller does not take ours
uint Advertisement::getMinInterval()
{
    return mDefaultIntervals ? 1280 : Stages[mStage].minMs;
}

uint Advertisement::getMaxInterval()
{
    return mDefaultIntervals ? 1280 : Stages[mStage].maxMs;
}

/* Exposed slots */
void Advertisement::Release()
{
    fprintf(stderr, "%s: Released!\n", mPath.toStdString().c_str());
}
