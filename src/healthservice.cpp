/*
 * Copyright (C) 2026 - eos
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

#include "healthservice.h"
#include "healthchanges.h"
#include "healthprofile.h"
#include "healthrecords.h"
#include "characteristic.h"
#include "common.h"
#include "messagejoiner.h"

#include <MDConfItem>
#include <QDBusMessage>
#include <QDebug>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>
#include <QXmlStreamReader>

// What this watch can send, for the capabilities characteristic (version 2:
// hrr, every heart rate reading; hr one median per minute of them. Version 3:
// wo workouts, wot their tracks, sl sleep sessions with their stages, rhr the
// days' resting heart rates, from health.db by its change sequence; mb: the
// most bytes of one answer of those; takes: what the phone may write besides
// requests, prof the user's profile and goals, healthprofile.h)
static const char HEALTH_CAPABILITIES[] =
    "<health><version>3</version><types>hr,steps,hrr,wo,wot,sl,rhr</types><mx>1000</mx><mb>8192</mb>"
    "<takes>prof</takes></health>";

// Requests are a few dozen bytes; anything longer without </req> is junk
static const int MaxRequestSize = 1024;

/*
 * Where sensorlogd keeps its logs, with a trailing slash: the dconf key
 * /org/asteroidos/btsyncd/health-data-dir if set (for tests), else
 * sensorlogd's loggerRootPath setting, else ~/.asteroid-sensorlogd/ (as
 * sensorlogd's common.cpp has it)
 */
static QString dataRoot()
{
    QString root = MDConfItem("/org/asteroidos/btsyncd/health-data-dir").value().toString();
    if (root.isEmpty()) {
        QSettings settings("asteroid", "sensorlogd");
        root = settings.value("loggerRootPath",
                              QStandardPaths::writableLocation(QStandardPaths::HomeLocation)
                                  + "/.asteroid-sensorlogd/").toString();
    }
    if (!root.endsWith('/'))
        root += '/';
    return root;
}

// The heart rate readings sent: those sensorlogd's own records use
// (heartrateSensor/minStatus, QHrmSensor::Status; default medium and up)
static int minAccuracy()
{
    QSettings settings("asteroid", "sensorlogd");
    return qBound(1, settings.value("heartrateSensor/minStatus", HealthRecords::DefaultMinAccuracy).toInt(), 4);
}

/*
 * Phone to watch: <req><ty>TYPE</ty><si>SINCE</si><mx>MAX</mx></req> (version
 * 3 also <sq>SEQ</sq> and <id>WORKOUT</id>), in as many writes as needed. A new request replaces an unanswered one; a request
 * that begins (at offset 0) while one is incomplete means the rest of that one
 * was lost, as does the phone going (drop()). <prof>...</prof>, the profile
 * and goals, is taken in without an answer (an unanswered request stays so).
 */
class HealthRequestChrc : public Characteristic
{
public:
    HealthRequestChrc(QDBusConnection bus, unsigned int index, HealthService *service)
      : Characteristic(bus, index, HEALTH_REQ_UUID, {"encrypt-authenticated-write"}, service, service), mService(service),
        mJoiner("Health request", {"req", "prof"}, MaxRequestSize) {}

    void drop(const char *why) { mJoiner.drop(why); }

public slots:
    void WriteValue(QByteArray value, QVariantMap options)
    {
        const int mtu = options.contains("mtu") ? options.value("mtu").toInt() : 0;
        const auto messages = mJoiner.add(value, MessageJoiner::offsetOf(options));
        if (mJoiner.overflowed())
            mService->request(QString(), 0, 0, mtu);
        for (const auto &message : messages) {
            if (message.first == "prof")
                mService->profile(message.second);
            else
                handle(message.second, mtu);
        }
    }

private:
    void handle(const QByteArray &xml, int mtu)
    {
        QXmlStreamReader reader(xml);
        QString type;
        qint64 since = 0, seq = 0, id = -1;
        int max = 0;
        bool ok = reader.readNextStartElement() && reader.name() == QLatin1String("req");
        while (ok && reader.readNextStartElement()) {
            if (reader.name() == QLatin1String("ty"))
                type = reader.readElementText().trimmed();
            else if (reader.name() == QLatin1String("si"))
                since = reader.readElementText().trimmed().toLongLong(&ok);
            else if (reader.name() == QLatin1String("mx"))
                max = reader.readElementText().trimmed().toInt(&ok);
            else if (reader.name() == QLatin1String("sq"))
                seq = reader.readElementText().trimmed().toLongLong(&ok);
            else if (reader.name() == QLatin1String("id"))
                id = reader.readElementText().trimmed().toLongLong(&ok);
            else
                reader.skipCurrentElement();
        }
        if (!ok || reader.hasError()) {
            qWarning() << "Bad health request" << xml;
            mService->request(QString(), 0, 0, mtu);
            return;
        }
        mService->request(type.isEmpty() ? QStringLiteral("?") : type, since, max, mtu, seq, id);
    }

    HealthService *mService;
    MessageJoiner mJoiner;
};

/*
 * Phone reads what the watch can send
 */
class HealthCapabilitiesChrc : public Characteristic
{
public:
    HealthCapabilitiesChrc(QDBusConnection bus, unsigned int index, Service *service)
      : Characteristic(bus, index, HEALTH_CAP_UUID, {"encrypt-authenticated-read"}, service, service) {}

public slots:
    QByteArray ReadValue(QVariantMap options)
    {
        QByteArray value(HEALTH_CAPABILITIES);
        int offset = options.value("offset", 0).toInt();
        return offset > 0 ? value.mid(offset) : value;
    }
};

HealthDataChrc::HealthDataChrc(QDBusConnection bus, int index, Service *service)
    : Characteristic(bus, index, HEALTH_DATA_UUID, {"encrypt-authenticated-notify"}, service, service)
{
    connect(this, SIGNAL(valueChanged()), this, SLOT(emitPropertiesChanged()));
}

void HealthDataChrc::send(const QByteArray &xml, int mtu)
{
    // One notification carries at most MTU - 3 bytes; the phone joins the
    // pieces until </data>
    int chunk = qBound(20, mtu - 3, 509);
    for (int pos = 0; pos < xml.size(); pos += chunk) {
        mValue = xml.mid(pos, chunk);
        emit valueChanged();
    }
}

void HealthDataChrc::emitPropertiesChanged()
{
    QDBusConnection connection = QDBusConnection::systemBus();
    QDBusMessage message = QDBusMessage::createSignal(getPath().path(),
                                                      "org.freedesktop.DBus.Properties",
                                                      "PropertiesChanged");

    QVariantMap changedProperties;
    changedProperties[QStringLiteral("Value")] = QVariant(mValue);

    QList<QVariant> arguments;
    arguments << QVariant(GATT_CHRC_IFACE) << QVariant(changedProperties) << QVariant(QStringList());
    message.setArguments(arguments);

    if (!connection.send(message))
        fprintf(stderr, "HealthDataChrc: failed to send the PropertiesChanged signal\n");
}

HealthService::HealthService(int index, QDBusConnection bus, QObject *parent) : Service(bus, index, HEALTH_UUID, parent)
{
    mData = new HealthDataChrc(bus, 1, this);
    mRequest = new HealthRequestChrc(bus, 0, this);
    addCharacteristic(mRequest);
    addCharacteristic(mData);
    addCharacteristic(new HealthCapabilitiesChrc(bus, 2, this));
}

void HealthService::phoneConnectedChanged(bool connected)
{
    // a request cut by the disconnection never ends: the phone asks again when it is back
    if (!connected)
        mRequest->drop("the phone is gone");
}

void HealthService::request(const QString &type, qint64 since, int max, int mtu, qint64 seq, qint64 id)
{
    if (mtu > 0)
        mPendingMtu = mtu;

    const HealthChanges::Kind kind = HealthChanges::kindFromName(type);
    if (type.isNull()) {
        mPending = HealthRecords::errorXml(QString(), QStringLiteral("request"));
    } else if (kind == HealthChanges::WorkoutTrack) {
        if (id < 0) {
            mPending = HealthRecords::errorXml(type, QStringLiteral("request"));
        } else {
            mPending = HealthChanges::track(dataRoot(), id, since, max);
            qDebug() << "Health request" << type << id << "since" << since << ":" << mPending.size() << "bytes";
        }
    } else if (kind != HealthChanges::None) {
        mPending = HealthChanges::changes(dataRoot(), kind, seq, max);
        qDebug() << "Health request" << type << "since seq" << seq << ":" << mPending.size() << "bytes";
    } else {
        const HealthRecords::Type t = HealthRecords::typeFromName(type);
        if (t == HealthRecords::Unknown) {
            mPending = HealthRecords::errorXml(type, QStringLiteral("type"));
        } else {
            const HealthRecords::Page page = HealthRecords::query(dataRoot(), t, since, max, minAccuracy());
            mPending = HealthRecords::toXml(t, page);
            qDebug() << "Health request" << type << "since" << since << ":" << page.records.size()
                     << "records," << mPending.size() << "bytes" << (page.more ? "(more)" : "");
        }
    }
    QTimer::singleShot(0, this, &HealthService::sendPending);
}

int HealthService::profile(const QByteArray &xml)
{
    HealthProfile::Values values;
    if (!HealthProfile::parse(xml, &values)) {
        qWarning() << "Bad health profile from the phone:" << xml.size() << "bytes";
        return -1;
    }
    const int written = HealthProfile::apply(values);
    // which groups changed, never their values
    const auto state = [written](int group, bool sent) {
        return QLatin1String((written & group) ? "written" : sent ? "unchanged" : "not sent");
    };
    qInfo().noquote() << QStringLiteral("Health profile from the phone: profile %1, steps goal %2, sleep goal %3")
                             .arg(state(HealthProfile::Profile, values.hasProfile()),
                                  state(HealthProfile::StepsGoal, values.stepsGoal > 0),
                                  state(HealthProfile::SleepGoal, values.sleepGoalMin > 0));
    return written;
}

void HealthService::sendPending()
{
    if (mPending.isEmpty())
        return;
    const QByteArray xml = mPending;
    mPending.clear();
    mData->send(xml, mPendingMtu);
}
