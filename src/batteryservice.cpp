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

#include "batterystatus.h"

#include <QTimer>
#include <QDBusMessage>
#include <QDebug>

#include "batteryservice.h"
#include "characteristic.h"
#include "common.h"

// The new value to BlueZ, which notifies the subscribed phone
static void sendValue(const QString &path, const QByteArray &value)
{
    QDBusConnection connection = QDBusConnection::systemBus();
    QDBusMessage message = QDBusMessage::createSignal(path,
                                                      "org.freedesktop.DBus.Properties",
                                                      "PropertiesChanged");

    QVariantMap changedProperties;
    changedProperties[QStringLiteral("Value")] = QVariant(value);

    QList<QVariant> arguments;
    arguments << QVariant(GATT_CHRC_IFACE) << QVariant(changedProperties) << QVariant(QStringList());
    message.setArguments(arguments);

    if (!connection.send(message))
        qDebug() << "Failed to send DBus property notification signal";
}

BatteryLvlChrc::BatteryLvlChrc(QDBusConnection bus, int index, Service *service, BatteryStatus *battery) : Characteristic(bus, index, BATTERY_LVL_UUID, {"encrypt-authenticated-read", "encrypt-authenticated-notify"}, service)
{
    m_battery = battery;
    connect(m_battery, &BatteryStatus::chargePercentageChanged,
            this, &BatteryLvlChrc::onBatteryPercentageChanged);
    connect(this, SIGNAL(valueChanged()), this, SLOT(emitPropertiesChanged()));
    m_value = QByteArray(1, 100);
}

void BatteryLvlChrc::onBatteryPercentageChanged(int percentage)
{
    if (percentage >= 0) {
        m_value = QByteArray(1, percentage);

        emit valueChanged();
    }
}

void BatteryLvlChrc::emitPropertiesChanged()
{
    sendValue(getPath().path(), m_value);
}

BatteryLvlStatusChrc::BatteryLvlStatusChrc(QDBusConnection bus, int index, Service *service, BatteryStatus *battery) : Characteristic(bus, index, BATTERY_LVL_STATUS_UUID, {"encrypt-authenticated-read", "encrypt-authenticated-notify"}, service)
{
    m_battery = battery;
    connect(m_battery, &BatteryStatus::stateChanged,
            this, &BatteryLvlStatusChrc::onStateChanged);
    connect(this, SIGNAL(valueChanged()), this, SLOT(emitPropertiesChanged()));
    m_value = encode(QString(), QString(), QString());
}

QByteArray BatteryLvlStatusChrc::encode(const QString &charger, const QString &state, const QString &status)
{
    const bool plugged = charger == QLatin1String("on");
    quint16 power = 1;                  // battery present
    if (plugged)
        power |= 1 << 1;                // wired external power connected
    else if (charger != QLatin1String("off"))
        power |= 2 << 1;                // wired: unknown
    // wireless (bits 3-4): 0, not connected
    if (state == QLatin1String("charging"))
        power |= 1 << 5;                // charging
    else if (state == QLatin1String("full"))
        power |= 3 << 5;                // discharging: inactive (full on the charger)
    else if (state == QLatin1String("not_charging") && plugged)
        power |= 3 << 5 | 1 << 14;      // discharging: inactive, fault "other"
    else if (state == QLatin1String("discharging") || state == QLatin1String("not_charging"))
        power |= 2 << 5;                // discharging: active
    if (status == QLatin1String("ok") || status == QLatin1String("full"))
        power |= 1 << 7;                // good
    else if (status == QLatin1String("low"))
        power |= 2 << 7;                // low
    else if (status == QLatin1String("empty"))
        power |= 3 << 7;                // critical

    QByteArray value(3, 0);             // flags 0: no identifier, level or additional status
    value[1] = char(power & 0xff);
    value[2] = char(power >> 8);
    return value;
}

void BatteryLvlStatusChrc::onStateChanged()
{
    const QByteArray value = encode(m_battery->chargerState(), m_battery->batteryState(),
                                    m_battery->batteryStatus());
    if (value == m_value)
        return;
    m_value = value;
    qInfo().noquote() << QStringLiteral("Battery level status %1 (charger %2, battery %3, %4)")
                         .arg(QString::fromLatin1(value.toHex(' ')), m_battery->chargerState(),
                              m_battery->batteryState(), m_battery->batteryStatus());
    emit valueChanged();
}

void BatteryLvlStatusChrc::emitPropertiesChanged()
{
    sendValue(getPath().path(), m_value);
}

BatteryService::BatteryService(int index, QDBusConnection bus, QObject *parent) : Service(bus, index, BATTERY_UUID, parent)
{
    BatteryStatus *battery = new BatteryStatus(this);
    addCharacteristic(new BatteryLvlChrc(bus, 0, this, battery));
    addCharacteristic(new BatteryLvlStatusChrc(bus, 1, this, battery));
}
