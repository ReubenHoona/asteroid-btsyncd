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

#ifndef BATTERYSERVICE_H
#define BATTERYSERVICE_H

#include <QObject>

#include "service.h"

class BatteryStatus;

class BatteryLvlChrc : public Characteristic
{
    Q_OBJECT
    Q_PROPERTY(QByteArray Value READ getValue NOTIFY valueChanged)
public:
    BatteryLvlChrc(QDBusConnection bus, int index, Service *service, BatteryStatus *battery);

public slots:
    QByteArray ReadValue(QVariantMap)
    {
        return m_value;
    }

    void StartNotify() {}
    void StopNotify() {}

private slots:
    void emitPropertiesChanged();
    void onBatteryPercentageChanged(int percentage);

signals:
    void valueChanged();

private:
    BatteryStatus *m_battery;
    QByteArray m_value;

    QByteArray getValue()
    {
        return m_value;
    }
};

/*
 * Battery Level Status (0x2BED, Battery Service 1.1, 3.2; layout: GATT
 * Specification Supplement, org.bluetooth.characteristic.battery_level_status):
 * whether the watch charges. Flags (uint8, no optional field: the level stays
 * in Battery Level, so this changes only with the state), then Power State
 * (uint16, little endian):
 *   bit 0      Battery Present: 1
 *   bits 1-2   Wired External Power Source Connected: 0 no, 1 yes, 2 unknown
 *   bits 3-4   Wireless External Power Source Connected: 0 no (the watch has
 *              no wireless charging)
 *   bits 5-6   Battery Charge State: 0 unknown, 1 charging, 2 discharging:
 *              active, 3 discharging: inactive (also "a battery that stopped
 *              charging because it is full, but the charger remained present")
 *   bits 7-8   Battery Charge Level: 0 unknown, 1 good, 2 low, 3 critical
 *              (mce's low and empty, the levels the watch warns at)
 *   bits 9-11  Charging Type: 0 unknown or not charging (not monitored)
 *   bits 12-14 Charging Fault Reason: bit 14 "other" while the charger is there
 *              but the battery does not charge ("not charging": mce does not say
 *              why: paused, too hot or cold, the charger gave up)
 * Notified only when the value changes (BAS 1.1: when the Power State does).
 */
class BatteryLvlStatusChrc : public Characteristic
{
    Q_OBJECT
    Q_PROPERTY(QByteArray Value READ getValue NOTIFY valueChanged)
public:
    BatteryLvlStatusChrc(QDBusConnection bus, int index, Service *service, BatteryStatus *battery);

    // The value for mce's charger state, battery state and battery status
    static QByteArray encode(const QString &charger, const QString &state, const QString &status);

public slots:
    QByteArray ReadValue(QVariantMap)
    {
        return m_value;
    }

    void StartNotify() {}
    void StopNotify() {}

private slots:
    void emitPropertiesChanged();
    void onStateChanged();

signals:
    void valueChanged();

private:
    BatteryStatus *m_battery;
    QByteArray m_value;

    QByteArray getValue()
    {
        return m_value;
    }
};

class BatteryService : public Service
{
    Q_OBJECT
public:
    explicit BatteryService(int index, QDBusConnection bus, QObject *parent = 0);
};

#endif // BATTERYSERVICE_H
