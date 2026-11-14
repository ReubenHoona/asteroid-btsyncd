/*
 * Copyright (C) 2026 eos-mainline contributors
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

#ifndef HEARTRATESERVICE_H
#define HEARTRATESERVICE_H

#include <QByteArray>
#include <QDBusConnection>
#include <QDBusContext>
#include <QDBusObjectPath>
#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QStringList>

#include "characteristic.h"
#include "common.h"
#include "service.h"

class MDConfItem;

/*
 * "Share heart rate" (stock Wear OS: Settings > Connectivity, a Quick
 * Settings tile; no account here): the standard Bluetooth Heart Rate
 * Service (0x180D) for gym equipment and phone apps.
 *   2A37 Heart Rate Measurement   notify (no pairing needed)
 *        flags byte, then the rate: bit 0 set when the rate is 16 bits
 *        (over 255), bits 1-2 = 3 sensor contact supported and detected,
 *        2 supported and not detected (then the rate is 0); no other field
 *   2A38 Body Sensor Location     read: 2 (wrist)
 * Only while the quick panel's toggle (dconf SHARE_HEART_RATE_KEY, bool,
 * default false) is on:
 *   - btsyncd advertises the service (its own LE advertisement, slow:
 *     1-1.5 s) and unregisters it when the toggle goes off;
 *   - a central that subscribes (StartNotify) starts the heart rate sensor,
 *     the last one leaving (StopNotify) or the toggle going off stops it.
 *     A subscription while the toggle is off is refused (NotPermitted).
 * No timer anywhere: a notification goes out for each reading the sensor
 * delivers (about one a second, none faster than every 800 ms), readings
 * older than 3 s (a replay at the start) are dropped. The heart rate sensor
 * is the stock source of battery cost here: the launcher's toggle shows the
 * note "Using this feature will impact battery life." Out of scope: the
 * connection limit and saved devices of stock (no account), disconnecting
 * a central when the toggle goes off (it stops receiving).
 */

/* Where the readings come from (the QtSensors heart rate sensor on the watch) */
class HeartRateSource : public QObject
{
    Q_OBJECT
public:
    using QObject::QObject;
    virtual void start() = 0;
    virtual void stop() = 0;

signals:
    // bpm (0 when none), whether the sensor touches skin, how old it is (ms)
    void reading(int bpm, bool contact, int ageMs);
};

#ifdef BTSYNCD_HAVE_SENSORS
class QHrmSensor;
class HrmSensorSource : public HeartRateSource
{
    Q_OBJECT
public:
    explicit HrmSensorSource(QObject *parent = nullptr) : HeartRateSource(parent) {}
    void start() override;
    void stop() override;

private slots:
    void onReading();

private:
    QHrmSensor *mSensor = nullptr;
};
#endif

class HeartRateService;

/* 2A37: notifications only */
class HeartRateMeasureChrc : public Characteristic, public QDBusContext
{
    Q_OBJECT
    Q_PROPERTY(QByteArray Value READ getValue NOTIFY valueChanged)

public:
    HeartRateMeasureChrc(QDBusConnection bus, int index, HeartRateService *service);
    void send(const QByteArray &value);

public slots:
    void StartNotify() override;
    void StopNotify() override;

signals:
    void valueChanged();

private:
    QByteArray getValue() { return mValue; }
    QByteArray mValue;
    HeartRateService *mService;
};

/* The advertisement of the service while it is shared */
class HeartRateAdvertisement : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", LE_ADVERTISEMENT_IFACE)
    Q_PROPERTY(QString Type READ getType())
    Q_PROPERTY(QStringList ServiceUUIDs READ getServiceUUIDs())
    Q_PROPERTY(QStringList Includes READ getIncludes())
    Q_PROPERTY(bool Discoverable READ getDiscoverable())
    Q_PROPERTY(uint MinInterval READ getMinInterval())
    Q_PROPERTY(uint MaxInterval READ getMaxInterval())

public:
    static constexpr const char *Path = "/org/asteroidos/btsyncd/advertisement_hrs";
    explicit HeartRateAdvertisement(QDBusConnection bus, QObject *parent = nullptr);
    ~HeartRateAdvertisement();

public slots:
    void Release() {}

private:
    QString getType() { return QStringLiteral("peripheral"); }
    QStringList getServiceUUIDs() { return {QStringLiteral(HRS_UUID)}; }
    QStringList getIncludes() { return {QStringLiteral("local-name")}; }
    bool getDiscoverable() { return true; }
    uint getMinInterval() { return 1000; }
    uint getMaxInterval() { return 1500; }
    QDBusConnection mBus;
};

class HeartRateService : public Service
{
    Q_OBJECT
public:
    // source: where the readings come from (default: the watch's heart rate
    // sensor, when built with QtSensors; none otherwise)
    HeartRateService(int index, QDBusConnection bus, HeartRateSource *source = nullptr, QObject *parent = nullptr);
    ~HeartRateService();

    // The notification's value: flags, then the rate (0 with no contact)
    static QByteArray measurement(int bpm, bool contact);
    // No faster than this
    static const int MinNotifyIntervalMs = 800;
    // A reading older than this is not sent
    static const int MaxReadingAgeMs = 3000;

    bool sharing() const;         // the toggle is on
    bool advertising() const { return mAdvertising; }
    bool notifying() const { return mNotifying; }
    bool measuring() const { return mMeasuring; }
    // BlueZ's adapter for the advertisement (default /org/bluez/hci0)
    void setAdapterPath(const QString &path);

    // From the characteristic; false: refused
    bool startNotify();
    void stopNotify();

signals:
    void advertisingChanged(bool advertising);

private slots:
    void update();
    void onReading(int bpm, bool contact, int ageMs);

private:
    void setAdvertising(bool on);

    QDBusConnection mBus;
    HeartRateSource *mSource = nullptr;
    HeartRateMeasureChrc *mMeasure = nullptr;
    MDConfItem *mShare = nullptr;
    HeartRateAdvertisement *mAdvert = nullptr;
    QString mAdapter;
    bool mNotifying = false;
    bool mMeasuring = false;
    bool mAdvertising = false;     // registered with BlueZ (or being)
    bool mAdvertWanted = false;
    QElapsedTimer mLastSent;
};

#endif // HEARTRATESERVICE_H
