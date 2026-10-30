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

#include "batterystatus.h"

#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>

static const char *MCE_SERVICE          = "com.nokia.mce";
static const char *MCE_REQUEST_PATH     = "/com/nokia/mce/request";
static const char *MCE_REQUEST_IF       = "com.nokia.mce.request";
static const char *MCE_SIGNAL_PATH      = "/com/nokia/mce/signal";
static const char *MCE_SIGNAL_IF        = "com.nokia.mce.signal";
static const char *MCE_BATTERY_LEVEL_GET = "get_battery_level";
static const char *MCE_BATTERY_LEVEL_SIG = "battery_level_ind";
static const char *MCE_CHARGER_STATE_GET = "get_charger_state";
static const char *MCE_CHARGER_STATE_SIG = "charger_state_ind";
static const char *MCE_BATTERY_STATE_GET = "get_battery_state";
static const char *MCE_BATTERY_STATE_SIG = "battery_state_ind";
static const char *MCE_BATTERY_STATUS_GET = "get_battery_status";
static const char *MCE_BATTERY_STATUS_SIG = "battery_status_ind";

BatteryStatus::BatteryStatus(QObject *parent) : QObject(parent)
{
    QDBusConnection::systemBus().connect(
        MCE_SERVICE, MCE_SIGNAL_PATH, MCE_SIGNAL_IF, MCE_BATTERY_LEVEL_SIG,
        this, SLOT(onBatteryLevelInd(int)));
    // Whether it charges: mce signals these only when they change (plugged
    // in, unplugged, full, low), so nothing runs here while nothing happens
    QDBusConnection::systemBus().connect(
        MCE_SERVICE, MCE_SIGNAL_PATH, MCE_SIGNAL_IF, MCE_CHARGER_STATE_SIG,
        this, SLOT(onChargerStateInd(QString)));
    QDBusConnection::systemBus().connect(
        MCE_SERVICE, MCE_SIGNAL_PATH, MCE_SIGNAL_IF, MCE_BATTERY_STATE_SIG,
        this, SLOT(onBatteryStateInd(QString)));
    QDBusConnection::systemBus().connect(
        MCE_SERVICE, MCE_SIGNAL_PATH, MCE_SIGNAL_IF, MCE_BATTERY_STATUS_SIG,
        this, SLOT(onBatteryStatusInd(QString)));

    QDBusInterface mce(MCE_SERVICE, MCE_REQUEST_PATH, MCE_REQUEST_IF,
                       QDBusConnection::systemBus());
    QDBusPendingCall call = mce.asyncCall(MCE_BATTERY_LEVEL_GET);
    auto *watcher = new QDBusPendingCallWatcher(call, this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this](QDBusPendingCallWatcher *w) {
        QDBusPendingReply<int> reply = *w;
        w->deleteLater();
        if (!reply.isError())
            onBatteryLevelInd(reply.value());
    });
    query(MCE_CHARGER_STATE_GET, "onChargerStateInd");
    query(MCE_BATTERY_STATE_GET, "onBatteryStateInd");
    query(MCE_BATTERY_STATUS_GET, "onBatteryStatusInd");
}

// mce's current value of one of the string states, into that slot
void BatteryStatus::query(const char *method, const char *slot)
{
    QDBusMessage message = QDBusMessage::createMethodCall(MCE_SERVICE, MCE_REQUEST_PATH,
                                                          MCE_REQUEST_IF, method);
    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(message), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, slot](QDBusPendingCallWatcher *w) {
        QDBusPendingReply<QString> reply = *w;
        w->deleteLater();
        if (!reply.isError())
            QMetaObject::invokeMethod(this, slot, Q_ARG(QString, reply.value()));
    });
}

void BatteryStatus::onBatteryLevelInd(int percentage)
{
    if (percentage != m_chargePercentage) {
        m_chargePercentage = percentage;
        emit chargePercentageChanged(percentage);
    }
}

void BatteryStatus::onChargerStateInd(const QString &state)
{
    setState(m_chargerState, state);
}

void BatteryStatus::onBatteryStateInd(const QString &state)
{
    setState(m_batteryState, state);
}

void BatteryStatus::onBatteryStatusInd(const QString &status)
{
    setState(m_batteryStatus, status);
}

void BatteryStatus::setState(QString &member, const QString &value)
{
    if (value != member) {
        member = value;
        emit stateChanged();
    }
}
