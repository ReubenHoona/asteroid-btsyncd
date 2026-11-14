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

#include "heartrateservice.h"

#include <MDConfItem>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDebug>
#include <time.h>

#ifdef BTSYNCD_HAVE_SENSORS
#include <QtSensors/QHrmSensor>
#endif

#ifdef BTSYNCD_HAVE_SENSORS
void HrmSensorSource::start()
{
    if (!mSensor) {
        mSensor = new QHrmSensor(this);
        mSensor->setAlwaysOn(true); // sensorfw stops sensors in standby (display off) otherwise
        connect(mSensor, &QSensor::readingChanged, this, &HrmSensorSource::onReading);
    }
    if (!mSensor->isActive() && !mSensor->start())
        qWarning() << "Share heart rate: the heart rate sensor did not start";
}

void HrmSensorSource::stop()
{
    if (mSensor && mSensor->isActive())
        mSensor->stop();
}

void HrmSensorSource::onReading()
{
    QHrmReading *r = mSensor->reading();
    if (!r)
        return;
    // sensorfw's time stamps are CLOCK_BOOTTIME microseconds (see sensorlogd's sampleTime.h)
    struct timespec boot;
    clock_gettime(CLOCK_BOOTTIME, &boot);
    const qint64 nowUs = (qint64)boot.tv_sec * 1000000 + boot.tv_nsec / 1000;
    const quint64 ts = r->timestamp();
    int age = 0;
    if (ts != 0 && (qint64)ts <= nowUs)
        age = int(qMin<qint64>((nowUs - (qint64)ts) / 1000, 1000000));
    emit reading(r->bpm(), mSensor->status() > QHrmSensor::NoContact, age);
}
#endif

/* ---- the characteristics ---- */

HeartRateMeasureChrc::HeartRateMeasureChrc(QDBusConnection bus, int index, HeartRateService *service)
    : Characteristic(bus, index, HRS_MEASURE_UUID, {"notify"}, service, service), mService(service)
{
}

void HeartRateMeasureChrc::send(const QByteArray &value)
{
    mValue = value;
    emit valueChanged();
    QDBusMessage message = QDBusMessage::createSignal(getPath().path(), "org.freedesktop.DBus.Properties",
                                                      "PropertiesChanged");
    QVariantMap changed;
    changed[QStringLiteral("Value")] = QVariant(mValue);
    message << QVariant(GATT_CHRC_IFACE) << QVariant(changed) << QVariant(QStringList());
    if (!QDBusConnection::systemBus().send(message))
        fprintf(stderr, "HeartRateMeasureChrc: failed to send the PropertiesChanged signal\n");
}

void HeartRateMeasureChrc::StartNotify()
{
    if (!mService->startNotify() && calledFromDBus())
        sendErrorReply(QStringLiteral("org.bluez.Error.NotPermitted"), QStringLiteral("Heart rate is not shared"));
}

void HeartRateMeasureChrc::StopNotify()
{
    mService->stopNotify();
}

class HeartRateLocationChrc : public Characteristic
{
public:
    HeartRateLocationChrc(QDBusConnection bus, int index, Service *service)
        : Characteristic(bus, index, HRS_LOCATION_UUID, {"read"}, service, service) {}

public slots:
    QByteArray ReadValue(QVariantMap options) override
    {
        const QByteArray value(1, char(2)); // wrist
        const int offset = options.value("offset", 0).toInt();
        return offset > 0 ? value.mid(offset) : value;
    }
};

/* ---- the advertisement ---- */

HeartRateAdvertisement::HeartRateAdvertisement(QDBusConnection bus, QObject *parent)
    : QObject(parent), mBus(bus)
{
    mBus.registerObject(QLatin1String(Path), this, QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllProperties);
}

HeartRateAdvertisement::~HeartRateAdvertisement()
{
    mBus.unregisterObject(QLatin1String(Path));
}

/* ---- the service ---- */

QByteArray HeartRateService::measurement(int bpm, bool contact)
{
    // bits 1-2: 3 contact detected, 2 supported but not detected
    quint8 flags = contact ? 0x06 : 0x04;
    if (!contact || bpm < 0)
        bpm = 0;
    bpm = qMin(bpm, 0xffff);
    QByteArray out;
    if (bpm > 255) {
        flags |= 0x01;
        out.append(char(flags));
        out.append(char(bpm & 0xff));
        out.append(char(bpm >> 8));
    } else {
        out.append(char(flags));
        out.append(char(bpm));
    }
    return out;
}

HeartRateService::HeartRateService(int index, QDBusConnection bus, HeartRateSource *source, QObject *parent)
    : Service(bus, index, HRS_UUID, parent), mBus(bus), mSource(source), mAdapter(QStringLiteral("/org/bluez/hci0"))
{
    mMeasure = new HeartRateMeasureChrc(bus, 0, this);
    addCharacteristic(mMeasure);
    addCharacteristic(new HeartRateLocationChrc(bus, 1, this));
#ifdef BTSYNCD_HAVE_SENSORS
    if (!mSource)
        mSource = new HrmSensorSource(this);
#endif
    if (mSource) {
        mSource->setParent(this);
        connect(mSource, &HeartRateSource::reading, this, &HeartRateService::onReading);
    }
    mShare = new MDConfItem(SHARE_HEART_RATE_KEY, this);
    connect(mShare, &MDConfItem::valueChanged, this, &HeartRateService::update);
    update();
}

HeartRateService::~HeartRateService()
{
    if (mMeasuring && mSource)
        mSource->stop();
    if (mAdvertising)
        setAdvertising(false);
}

bool HeartRateService::sharing() const
{
    return mShare->value(false).toBool();
}

void HeartRateService::setAdapterPath(const QString &path)
{
    mAdapter = path;
}

bool HeartRateService::startNotify()
{
    if (!sharing()) {
        qInfo() << "Share heart rate: a subscription while it is off, refused";
        return false;
    }
    mNotifying = true;
    update();
    return true;
}

void HeartRateService::stopNotify()
{
    mNotifying = false;
    update();
}

void HeartRateService::update()
{
    const bool share = sharing();
    // the advertisement follows the toggle alone
    if (share != mAdvertWanted)
        setAdvertising(share);
    // the sensor runs only for a subscriber of a shared service
    const bool measure = share && mNotifying;
    // (switched off while a central listens: it gets nothing more, and
    // BlueZ still counts it, so it goes on when the toggle comes back)
    if (measure != mMeasuring) {
        mMeasuring = measure;
        if (measure)
            mLastSent.invalidate();
        if (mSource) {
            if (measure)
                mSource->start();
            else
                mSource->stop();
        }
        qInfo() << "Share heart rate: the sensor is" << (measure ? "on" : "off");
    }
}

void HeartRateService::onReading(int bpm, bool contact, int ageMs)
{
    if (!mMeasuring || ageMs > MaxReadingAgeMs)
        return;
    if (mLastSent.isValid() && mLastSent.elapsed() < MinNotifyIntervalMs)
        return;
    mLastSent.start();
    mMeasure->send(measurement(bpm, contact && bpm > 0));
}

void HeartRateService::setAdvertising(bool on)
{
    mAdvertWanted = on;
    if (on && mAdvertising)
        return;
    if (on) {
        if (!mAdvert)
            mAdvert = new HeartRateAdvertisement(mBus, this);
        mAdvertising = true;
        QDBusMessage message = QDBusMessage::createMethodCall(BLUEZ_SERVICE_NAME, mAdapter, LE_ADVERTISING_MANAGER_IFACE,
                                                              "RegisterAdvertisement");
        message << QVariant::fromValue(QDBusObjectPath(HeartRateAdvertisement::Path)) << QVariant::fromValue(QVariantMap());
        QDBusPendingCall call = mBus.asyncCall(message);
        QDBusPendingCallWatcher *watcher = new QDBusPendingCallWatcher(call, this);
        connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher] {
            QDBusPendingReply<> reply = *watcher;
            watcher->deleteLater();
            // switched off while BlueZ was answering: the unregistering is already on its way
            if (!mAdvertWanted || !mAdvertising)
                return;
            if (reply.isError()) {
                qWarning() << "Share heart rate: RegisterAdvertisement failed:" << reply.error().message();
                mAdvertising = false;
                emit advertisingChanged(false);
                return;
            }
            qInfo() << "Share heart rate: advertising";
            emit advertisingChanged(true);
        });
        return;
    }
    if (!mAdvertising)
        return;
    mAdvertising = false;
    QDBusMessage message = QDBusMessage::createMethodCall(BLUEZ_SERVICE_NAME, mAdapter, LE_ADVERTISING_MANAGER_IFACE,
                                                          "UnregisterAdvertisement");
    message << QVariant::fromValue(QDBusObjectPath(HeartRateAdvertisement::Path));
    mBus.asyncCall(message);
    qInfo() << "Share heart rate: not advertising";
    emit advertisingChanged(false);
}
