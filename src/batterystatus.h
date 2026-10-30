/*
 * Copyright (C) 2026 - Florent Revest <revestflo@gmail.com>
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

#ifndef BATTERYSTATUS_H
#define BATTERYSTATUS_H

#include <QObject>
#include <QString>

class BatteryStatus : public QObject
{
    Q_OBJECT
public:
    explicit BatteryStatus(QObject *parent = nullptr);

    int chargePercentage() const { return m_chargePercentage; }

    // mce's words (as its battery-udev module derives them from the kernel's
    // power supplies, the launcher's battery notifier uses the same), empty
    // until mce said: charger "on", "off" or "unknown"; battery state
    // "charging", "discharging", "not_charging", "full" or "unknown"; battery
    // status "ok", "low", "empty", "full" or "unknown"
    QString chargerState() const { return m_chargerState; }
    QString batteryState() const { return m_batteryState; }
    QString batteryStatus() const { return m_batteryStatus; }

signals:
    void chargePercentageChanged(int percentage);
    // the charger, the battery state or the battery status changed
    void stateChanged();

private slots:
    void onBatteryLevelInd(int percentage);
    void onChargerStateInd(const QString &state);
    void onBatteryStateInd(const QString &state);
    void onBatteryStatusInd(const QString &status);

private:
    void query(const char *method, const char *slot);
    void setState(QString &member, const QString &value);

    int m_chargePercentage = -1;
    QString m_chargerState, m_batteryState, m_batteryStatus;
};

#endif // BATTERYSTATUS_H
