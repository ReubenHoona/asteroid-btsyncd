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

#ifndef BLUEZMANAGER_H
#define BLUEZMANAGER_H

#include <QObject>
#include <QDBusObjectPath>
#include <QDBusServiceWatcher>
#include <QDBusConnection>
#include <QTimer>

#include "ancs.h"
#include "cts.h"

typedef QMap<QString, QMap<QString, QVariant>> InterfaceList;

class BlueZManager : public QObject
{
    Q_OBJECT
public:
    explicit BlueZManager(QDBusObjectPath appPath, QDBusObjectPath advertPath, QObject *parent = 0);
    void updateConnected();
    // A central (the phone) is connected
    bool connected() const { return mConnected; }

private:
    bool mConnected, mServicesResolved;
    QDBusObjectPath mAppPath, mAdvertPath;
    QString mAdapter, mConnectedDevice;
    QDBusServiceWatcher *mWatcher;
    QDBusConnection mBus;
    ANCS mAncs;
    CTS mCts;

    // The adapter's Powered property: after a power cycle BlueZ still lists
    // the advertisement, but the controller no longer sends it
    bool mPowered = false;
    // A device is paired (bonded); known once BlueZ answered with an adapter
    bool mPaired = false;
    bool mPairedKnown = false;
    // An adapter object appeared (again): register everything even when its
    // path is the one we had
    bool mRegisterAll = false;
    bool mReRegistering = false;
    QTimer mAdvertCheck;

    void updateAdapter();
    void setAdapter(QString adatper);
    void setConnected(bool connected);
    void setServicesResolved(bool servicesResolved);
    void registerAll();
    void registerAdvertisement();

public:
    // Unregister the advertisement (errors ignored), then register it again
    void reRegisterAdvertisement(const QString &reason);

private slots:
    // Every minute while no central is connected: re-register when BlueZ
    // has no active advertisement although ours should be
    void checkAdvertising();

signals:
    void connectedChanged();
    // No device is paired with the watch (any more, or when btsyncd started)
    void noDevicePaired();
    void adapterChanged();
    void servicesResolvedChanged();

public slots:
    void serviceRegistered(const QString& name);
    void serviceUnregistered(const QString& name);
    void InterfacesAdded(QDBusObjectPath path, InterfaceList interfaces);
    void InterfacesRemoved(QDBusObjectPath, QStringList);
    void PropertiesChanged(QString interface, QMap<QString, QVariant> changed, QStringList invalidated);

    void onConnectedChanged();
    void onServicesResolvedChanged();
    void onAdapterChanged();
};

#endif // BLUEZMANAGER_H
