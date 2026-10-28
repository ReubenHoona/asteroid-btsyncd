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

#include "bluezmanager.h"

#include <QDBusServiceWatcher>
#include <QDBusInterface>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusArgument>
#include <QDBusMessage>
#include <QDBusVariant>
#include <QDebug>
#include <QTimer>

#include "common.h"

#include <MDConfItem>

BlueZManager::BlueZManager(QDBusObjectPath appPath, QDBusObjectPath advertPath, QObject *parent)
    : QObject(parent), mAppPath(appPath), mAdvertPath(advertPath), mAdapter("adapter"), mBus(QDBusConnection::systemBus())
{
    mConnected = false;
    mServicesResolved = false;
    mConnectedDevice = "";

    mWatcher = new QDBusServiceWatcher(BLUEZ_SERVICE_NAME, QDBusConnection::systemBus());
    connect(mWatcher, SIGNAL(serviceRegistered(const QString&)), this, SLOT(serviceRegistered(const QString&)));
    connect(mWatcher, SIGNAL(serviceUnregistered(const QString&)), this, SLOT(serviceUnregistered(const QString&)));

    connect(this, SIGNAL(adapterChanged()), this, SLOT(onAdapterChanged()));
    connect(this, SIGNAL(connectedChanged()), this, SLOT(onConnectedChanged()));
    connect(this, SIGNAL(servicesResolvedChanged()), this, SLOT(onServicesResolvedChanged()));

    // BTSYNCD_ADVERT_CHECK_MS: a shorter period, for tests
    const int checkMs = qEnvironmentVariableIntValue("BTSYNCD_ADVERT_CHECK_MS");
    mAdvertCheck.setInterval(checkMs > 0 ? checkMs : 60000);
    connect(&mAdvertCheck, &QTimer::timeout, this, &BlueZManager::checkAdvertising);
    mAdvertCheck.start();

    QDBusInterface remoteOm(BLUEZ_SERVICE_NAME, "/", DBUS_OM_IFACE, mBus);
    if(remoteOm.isValid())
        serviceRegistered(BLUEZ_SERVICE_NAME);
    else
        serviceUnregistered(BLUEZ_SERVICE_NAME);
}

void BlueZManager::serviceRegistered(const QString& name)
{
    qDebug() << "Service" << name << "is running";

    mBus.connect(BLUEZ_SERVICE_NAME, "/", DBUS_OM_IFACE, "InterfacesAdded", this, SLOT(InterfacesAdded(QDBusObjectPath, InterfaceList)));
    mBus.connect(BLUEZ_SERVICE_NAME, "/", DBUS_OM_IFACE, "InterfacesRemoved", this, SLOT(InterfacesRemoved(QDBusObjectPath, QStringList)));
    // One match for the PropertiesChanged of every BlueZ object, also those
    // that appear later. A match per device path was added for every device
    // ever seen (one per random address while scanning) and never removed,
    // and dbus-daemon caps a connection's match rules. The slot filters for
    // the properties updateAdapter() reads.
    mBus.connect(BLUEZ_SERVICE_NAME, QString(), DBUS_PROPERTIES_IFACE, "PropertiesChanged", this, SLOT(PropertiesChanged(QString, QMap<QString, QVariant>, QStringList)));

    updateAdapter();
}

void BlueZManager::serviceUnregistered(const QString& name)
{
    qDebug() << "Service" << name << "is not running";
    // No adapter until bluetoothd is back; then it is a new adapter and
    // gets the GATT application and the advertisement registered again
    setAdapter("");
    mPowered = false;
    mConnectedDevice = "";
    setConnected(false);
}

void BlueZManager::InterfacesAdded(QDBusObjectPath path, InterfaceList interfaces)
{
    if (interfaces.contains(ADAPTER_IFACE) || interfaces.contains(GATT_MANAGER_IFACE)) {
        qInfo() << "Adapter" << path.path() << "appeared";
        mRegisterAll = true;
    }
    updateAdapter();
}

void BlueZManager::InterfacesRemoved(QDBusObjectPath, QStringList)
{
    updateAdapter();
}

void BlueZManager::updateAdapter() {
    QString adapter = "";
    bool connected = false;
    bool servicesResolved = false;
    bool powered = false;

    QDBusInterface remoteOm(BLUEZ_SERVICE_NAME, "/", DBUS_OM_IFACE, mBus);
    QDBusMessage result = remoteOm.call("GetManagedObjects");
    if (result.type() != QDBusMessage::ReplyMessage || result.arguments().isEmpty()) {
        // bluetoothd gone or not answering: keep the state we have (an error
        // made locally has no arguments, and at(0) would read past the list)
        qWarning() << "GetManagedObjects failed:" << result.errorMessage();
        return;
    }

    const QDBusArgument argument = result.arguments().at(0).value<QDBusArgument>();
    if (argument.currentType() == QDBusArgument::MapType) {
        argument.beginMap();
        while (!argument.atEnd()) {
            QString key;
            InterfaceList value;

            argument.beginMapEntry();
            argument >> key >> value;
            argument.endMapEntry();

            if (value.contains(GATT_MANAGER_IFACE) && value.contains(LE_ADVERTISING_MANAGER_IFACE)) {
                adapter = key;
                powered = value.value(ADAPTER_IFACE).value("Powered").toBool();
            }

            if (value.contains(DEVICE_MANAGER_IFACE)) {
                 QMap<QString, QVariant> properties = value.value(DEVICE_MANAGER_IFACE);
                 if(properties.contains("Connected") && properties.value("Connected").toBool()) {
                    connected = true;
                    if(properties.contains("Alias")) {
                       mConnectedDevice = properties.value("Alias").toString();
                    }
                 }

                 if(properties.contains("ServicesResolved"))
                    servicesResolved |= properties.value("ServicesResolved").toBool();

            }
        }
        argument.endMap();
    }

    // A new adapter path registers everything (onAdapterChanged)
    const bool newAdapter = adapter != mAdapter;
    setAdapter(adapter);
    if (!adapter.isEmpty() && !newAdapter) {
        if (mRegisterAll) {
            // The same path, but a new adapter object (controller reset,
            // hci0 removed and added): our registrations went with the old one
            qInfo() << "Adapter" << adapter << "is back: registering the GATT application and the advertisement again";
            registerAll();
        } else if (powered && !mPowered) {
            reRegisterAdvertisement(QStringLiteral("adapter powered on"));
        }
    }
    mRegisterAll = false;
    if (powered != mPowered)
        qInfo() << "Adapter" << adapter << (powered ? "powered on" : "powered off");
    mPowered = powered;
    setConnected(connected);
    setServicesResolved(servicesResolved);
}

void BlueZManager::setAdapter(QString adapter)
{
    if (adapter != mAdapter) {
        mAdapter = adapter;
        emit adapterChanged();
    }
}

void BlueZManager::setConnected(bool connected)
{
    if (connected != mConnected) {
        mConnected = connected;
        emit connectedChanged();
    }
}

void BlueZManager::setServicesResolved(bool servicesResolved)
{
    if (servicesResolved != mServicesResolved) {
        mServicesResolved = servicesResolved;
        emit servicesResolvedChanged();
    }
}

void BlueZManager::onAdapterChanged()
{
    if(mAdapter != "")
    {
        qDebug() << "BLE Adapter" << mAdapter << "found";
        registerAll();
    }
    else
        qDebug() << "No BLE adapter found";
}

void BlueZManager::registerAll()
{
    {
        // Don't fire-and-forget: a failed RegisterApplication/RegisterAdvertisement
        // (adapter not ready, AlreadyExists after a partial restart, ...) would
        // otherwise leave the daemon silently running with no GATT or advertising.
        // Watch each reply and log errors; a power cycle of the adapter or the
        // periodic check registers the advertisement again.
        QDBusInterface serviceManager(BLUEZ_SERVICE_NAME, mAdapter, GATT_MANAGER_IFACE, mBus);
        QDBusPendingCall appCall = serviceManager.asyncCall("RegisterApplication", QVariant::fromValue(mAppPath), QVariantMap());
        QDBusPendingCallWatcher *appWatcher = new QDBusPendingCallWatcher(appCall, this);
        connect(appWatcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *w) {
            QDBusPendingReply<> reply = *w;
            if (reply.isError())
                qWarning() << "RegisterApplication failed:" << reply.error().message();
            else
                qDebug() << "Service" << mAppPath.path() << "registered";
            w->deleteLater();
        });
    }
    registerAdvertisement();
}

void BlueZManager::registerAdvertisement()
{
    QDBusInterface adManager(BLUEZ_SERVICE_NAME, mAdapter, LE_ADVERTISING_MANAGER_IFACE, mBus);
    QDBusPendingCall advCall = adManager.asyncCall("RegisterAdvertisement", QVariant::fromValue(mAdvertPath), QVariantMap());
    QDBusPendingCallWatcher *advWatcher = new QDBusPendingCallWatcher(advCall, this);
    connect(advWatcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *w) {
        QDBusPendingReply<> reply = *w;
        if (reply.isError())
            qWarning() << "RegisterAdvertisement failed:" << reply.error().message();
        else
            qInfo() << "Advertisement" << mAdvertPath.path() << "registered";
        mReRegistering = false;
        w->deleteLater();
    });
}

void BlueZManager::reRegisterAdvertisement(const QString &reason)
{
    if (mAdapter.isEmpty() || mReRegistering)
        return;
    mReRegistering = true;
    qInfo() << "Registering the advertisement again:" << reason;
    QDBusInterface adManager(BLUEZ_SERVICE_NAME, mAdapter, LE_ADVERTISING_MANAGER_IFACE, mBus);
    QDBusPendingCall call = adManager.asyncCall("UnregisterAdvertisement", QVariant::fromValue(mAdvertPath));
    QDBusPendingCallWatcher *watcher = new QDBusPendingCallWatcher(call, this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *w) {
        QDBusPendingReply<> reply = *w;
        if (reply.isError())
            qDebug() << "UnregisterAdvertisement:" << reply.error().message() << "(ignored)";
        w->deleteLater();
        if (mAdapter.isEmpty()) {
            mReRegistering = false;
            return;
        }
        registerAdvertisement();
    });
}

void BlueZManager::checkAdvertising()
{
    if (mAdapter.isEmpty() || !mPowered || mConnected || mReRegistering)
        return;
    QDBusMessage get = QDBusMessage::createMethodCall(BLUEZ_SERVICE_NAME, mAdapter, DBUS_PROPERTIES_IFACE, "Get");
    get << QString(LE_ADVERTISING_MANAGER_IFACE) << QString("ActiveInstances");
    QDBusPendingCallWatcher *watcher = new QDBusPendingCallWatcher(mBus.asyncCall(get), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *w) {
        QDBusPendingReply<QDBusVariant> reply = *w;
        w->deleteLater();
        if (reply.isError() || mConnected)
            return;
        bool ok = false;
        const uint active = reply.value().variant().toUInt(&ok);
        if (ok && active == 0)
            reRegisterAdvertisement(QStringLiteral("BlueZ has no active advertisement"));
    });
}

void BlueZManager::PropertiesChanged(QString interface, QMap<QString, QVariant> changed, QStringList invalidated)
{
    // updateAdapter() fetches BlueZ's whole object tree with a blocking call:
    // only for what it reads. A device's RSSI, ManufacturerData or TxPower
    // change with every advertisement it sends while the adapter discovers.
    static const QStringList deviceKeys = { QStringLiteral("Connected"), QStringLiteral("ServicesResolved"),
                                            QStringLiteral("Alias") };
    static const QStringList adapterKeys = { QStringLiteral("Powered") };
    const QStringList *keys;
    if (interface == QLatin1String(DEVICE_MANAGER_IFACE))
        keys = &deviceKeys;
    else if (interface == QLatin1String(ADAPTER_IFACE))
        keys = &adapterKeys;
    else
        return;
    for (const QString &key : *keys) {
        if (changed.contains(key) || invalidated.contains(key)) {
            updateAdapter();
            return;
        }
    }
}

void BlueZManager::onConnectedChanged()
{
    QString appName, appIcon, summary, body;
    appName = "";

    if(!mConnected) {
        mAncs.disconnect();
        mCts.disconnect();
    }

    // Wear OS shows no notification when the phone connects or goes: only the
    // state (the launcher's phone toggle). The banners are opt-in.
    if(!MDConfItem(CONNECTION_NOTICES_KEY).value(false).toBool())
        return;
    if(mConnected) {
        //% "Connected"
        summary = qtTrId("id-connected");
        body = mConnectedDevice;
        appIcon = "ios-bluetooth-outline";
    } else {
        //% "Disconnected"
        summary = qtTrId("id-disconnected");
        body = "";
        appIcon = "ios-bluetooth-off-outline";
    }

    QVariantMap hints;
    hints.insert("x-nemo-preview-body", body);
    hints.insert("x-nemo-preview-summary", summary);
    hints.insert("x-nemo-feedback", "notif_silent");
    hints.insert("urgency", 3);
    hints.insert("transient", true);

    QList<QVariant> argumentList;
    argumentList << appName;
    argumentList << (uint) 0;
    argumentList << appIcon;
    argumentList << summary;
    argumentList << body;
    argumentList << QStringList();
    argumentList << hints;
    argumentList << (int) 3;

    static QDBusInterface notifyApp(NOTIFICATIONS_SERVICE_NAME, NOTIFICATIONS_PATH_BASE, NOTIFICATIONS_MAIN_IFACE);
    notifyApp.callWithArgumentList(QDBus::AutoDetect, "Notify", argumentList);
}

void BlueZManager::onServicesResolvedChanged() {
    if (mServicesResolved) {
        mAncs.searchForAncsCharacteristics();
        mCts.searchForTimeCharacteristics();
    }
}
