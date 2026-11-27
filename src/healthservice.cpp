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
#include "phonerelay.h"

#include <MDConfItem>
#include <QDBusMessage>
#include <QDateTime>
#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>
#include <QXmlStreamReader>

// What this watch can send, for the capabilities characteristic (version 2:
// hrr, every heart rate reading; hr one median per minute of them. Version 3:
// wo workouts, wot their tracks, sl sleep sessions with their stages, rhr the
// days' resting heart rates, from health.db by its change sequence; mb: the
// most bytes of one answer of those; takes: what the phone may write besides
// requests, prof the user's profile and goals, healthprofile.h, synced the end
// of a sync, healthservice.h)
static const char HEALTH_CAPABILITIES[] =
    "<health><version>3</version><types>hr,steps,hrr,wo,wot,sl,rhr</types><mx>1000</mx><mb>8192</mb>"
    "<takes>prof,synced</takes></health>";

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
 * and goals, and <synced>...</synced>, the end of a sync, are taken in
 * without an answer (an unanswered request stays so).
 */
class HealthRequestChrc : public Characteristic
{
public:
    HealthRequestChrc(QDBusConnection bus, unsigned int index, HealthService *service)
      : Characteristic(bus, index, HEALTH_REQ_UUID, {"encrypt-authenticated-write"}, service, service), mService(service),
        mJoiner("Health request", {"req", "prof", "synced"}, MaxRequestSize) {}

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
            else if (message.first == "synced")
                mService->synced(message.second);
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

    mSyncRequest = new MDConfItem(QString::fromLatin1(SyncRequestKey), this);
    mSyncState = new MDConfItem(QString::fromLatin1(SyncStateKey), this);
    mSyncLast = new MDConfItem(QString::fromLatin1(SyncLastKey), this);
    mSyncSupported = new MDConfItem(QString::fromLatin1(SyncSupportedKey), this);
    // the Health app's Today list and its Sync button: a change of the key, no polling
    connect(mSyncRequest, &MDConfItem::valueChanged, this, &HealthService::onSyncRequest);
    mSyncTimeout.setSingleShot(true);
    mSyncTimeout.setInterval(SyncTimeoutMs);
    connect(&mSyncTimeout, &QTimer::timeout, this, &HealthService::onSyncTimeout);
    // a sync that waited when btsyncd last stopped does not end any more
    if (mSyncState->value().toString() == QLatin1String("syncing"))
        setSyncState("idle");
    // what health.db has now; the workouts and nights after it start a sync
    mMarks = HealthChanges::syncMarks(dataRoot());
    QDBusConnection::sessionBus().connect(QStringLiteral("org.asteroid.sensorlogd.logger"),
                                          QStringLiteral("/org/asteroid/sensorlogd/logger"),
                                          QStringLiteral("org.asteroid.sensorlogd.logger"), QStringLiteral("HealthChanged"),
                                          this, SLOT(healthChanged(qlonglong)));
}

void HealthService::phoneConnectedChanged(bool connected)
{
    // a request cut by the disconnection never ends: the phone asks again when it is back
    if (!connected) {
        mRequest->drop("the phone is gone");
        if (mSyncing) {
            mSyncing = false;
            mSyncTimeout.stop();
            setSyncState("failed");
        }
    }
}

void HealthService::healthChanged(qlonglong)
{
    const HealthChanges::SyncMarks now = HealthChanges::syncMarks(dataRoot());
    if (!now.valid)
        return;
    const HealthChanges::SyncMarks before = mMarks;
    mMarks = now;
    // health.db made after btsyncd started: what it has first is no event
    if (!before.valid)
        return;
    if (now.workoutId > before.workoutId)
        requestSync(QStringLiteral("wo"));
    else if (now.wakeDate > before.wakeDate)
        requestSync(QStringLiteral("sl"));
}

void HealthService::onSyncRequest()
{
    // "today@MS" or "button@MS": the time makes every press a change of the key
    const QString value = mSyncRequest->value().toString();
    const QString event = value.section(QLatin1Char('@'), 0, 0);
    if (event == QLatin1String("today") || event == QLatin1String("button"))
        requestSync(event);
}

bool HealthService::requestSync(const QString &event)
{
    const bool button = event == QLatin1String("button");
    if (mSyncing) {
        qInfo() << "Health sync: the one the watch asked for still runs;" << event << "not asked for";
        return false;
    }
    if (!mRelay || !mRelay->relayHas(QStringLiteral("hsync"))) {
        // no phone that does it: the button says so (stock "Can't Sync Data", Retry)
        if (button)
            setSyncState("failed");
        return false;
    }
    if (!button) {
        const qint64 last = mSyncLast->value(0).toLongLong();
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (last > 0 && now >= last && now - last < SyncQuietMs)
            return false;
    }
    QJsonObject act;
    act.insert(QStringLiteral("a"), QStringLiteral("hsync"));
    act.insert(QStringLiteral("ev"), event);
    const bool sent = mRelay->relayAct(QJsonDocument(act).toJson(QJsonDocument::Compact), [this](const QString &json) {
        const QJsonObject r = QJsonDocument::fromJson(json.toUtf8()).object();
        if (r.value(QStringLiteral("ok")).toBool())
            return;     // it runs: <synced> ends it
        qInfo() << "Health sync: the phone did not start one:" << r.value(QStringLiteral("er")).toString();
        if (mSyncing) {
            mSyncing = false;
            mSyncTimeout.stop();
            setSyncState("failed");
        }
    });
    if (!sent) {
        if (button)
            setSyncState("failed");
        return false;
    }
    mSyncing = true;
    mSyncTimeout.start();
    setSyncState("syncing");
    qInfo() << "Health sync asked for:" << event;
    return true;
}

bool HealthService::synced(const QByteArray &xml)
{
    QXmlStreamReader reader(xml);
    if (!reader.readNextStartElement() || reader.name() != QLatin1String("synced"))
        return false;
    bool ok = false, okSeen = false;
    qint64 n = -1;
    while (reader.readNextStartElement()) {
        if (reader.name() == QLatin1String("ok")) {
            ok = reader.readElementText().trimmed() == QLatin1String("1");
            okSeen = true;
        } else if (reader.name() == QLatin1String("n")) {
            n = reader.readElementText().trimmed().toLongLong();
        } else {
            reader.skipCurrentElement();
        }
    }
    if (reader.hasError() || !okSeen) {
        qWarning() << "Bad end of a health sync from the phone:" << xml.size() << "bytes";
        return false;
    }
    if (!mSyncSupported->value(false).toBool())
        mSyncSupported->set(true);
    const bool asked = mSyncing;
    mSyncing = false;
    mSyncTimeout.stop();
    if (ok) {
        mSyncLast->set(QDateTime::currentMSecsSinceEpoch());
        setSyncState("idle");
    } else {
        setSyncState("failed");
    }
    qInfo().noquote() << QStringLiteral("Health sync ended %1, %2 records%3").arg(ok ? "in full" : "early")
                         .arg(n).arg(asked ? " (the watch asked for it)" : "");
    return true;
}

void HealthService::onSyncTimeout()
{
    if (!mSyncing)
        return;
    qInfo() << "Health sync: the phone did not end the one the watch asked for";
    mSyncing = false;
    setSyncState("failed");
}

void HealthService::setSyncState(const char *state)
{
    const QString s = QString::fromLatin1(state);
    if (mSyncState->value().toString() != s)
        mSyncState->set(s);
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
    qInfo().noquote() << QStringLiteral("Health profile from the phone: profile %1, steps goal %2, sleep goal %3, units %4")
                             .arg(state(HealthProfile::Profile, values.hasProfile()),
                                  state(HealthProfile::StepsGoal, values.stepsGoal > 0),
                                  state(HealthProfile::SleepGoal, values.sleepGoalMin > 0),
                                  state(HealthProfile::Units, !values.units.isEmpty()));
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
