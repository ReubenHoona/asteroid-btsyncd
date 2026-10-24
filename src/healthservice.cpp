/*
 * Copyright (C) 2026 - Reuben Horner <reubarb.scons@gmail.com>
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
#include "healthrecords.h"
#include "characteristic.h"
#include "common.h"

#include <MDConfItem>
#include <QDBusMessage>
#include <QDebug>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>
#include <QXmlStreamReader>

// What this watch can send, for the capabilities characteristic
static const char HEALTH_CAPABILITIES[] =
    "<health><version>1</version><types>hr,steps</types><mx>1000</mx></health>";

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

/*
 * Phone to watch: <req><ty>TYPE</ty><si>SINCE</si><mx>MAX</mx></req>, in as
 * many writes as needed. A new request replaces an unanswered one.
 */
class HealthRequestChrc : public Characteristic
{
public:
    HealthRequestChrc(QDBusConnection bus, unsigned int index, HealthService *service)
      : Characteristic(bus, index, HEALTH_REQ_UUID, {"encrypt-authenticated-write"}, service, service), mService(service) {}

public slots:
    void WriteValue(QByteArray value, QVariantMap options)
    {
        const int mtu = options.contains("mtu") ? options.value("mtu").toInt() : 0;
        mBuffer += value;
        const int end = mBuffer.indexOf("</req>");
        if (end < 0) {
            if (mBuffer.size() > MaxRequestSize) {
                qWarning() << "Health request without an end, dropped";
                mBuffer.clear();
                mService->request(QString(), 0, 0, mtu);
            }
            return;
        }
        const QByteArray xml = mBuffer.left(end + 6);
        mBuffer.clear();

        QXmlStreamReader reader(xml);
        QString type;
        qint64 since = 0;
        int max = 0;
        bool ok = reader.readNextStartElement() && reader.name() == QLatin1String("req");
        while (ok && reader.readNextStartElement()) {
            if (reader.name() == QLatin1String("ty"))
                type = reader.readElementText().trimmed();
            else if (reader.name() == QLatin1String("si"))
                since = reader.readElementText().trimmed().toLongLong(&ok);
            else if (reader.name() == QLatin1String("mx"))
                max = reader.readElementText().trimmed().toInt(&ok);
            else
                reader.skipCurrentElement();
        }
        if (!ok || reader.hasError()) {
            qWarning() << "Bad health request" << xml;
            mService->request(QString(), 0, 0, mtu);
            return;
        }
        mService->request(type.isEmpty() ? QStringLiteral("?") : type, since, max, mtu);
    }

private:
    HealthService *mService;
    QByteArray mBuffer;
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
    addCharacteristic(new HealthRequestChrc(bus, 0, this));
    addCharacteristic(mData);
    addCharacteristic(new HealthCapabilitiesChrc(bus, 2, this));
}

void HealthService::request(const QString &type, qint64 since, int max, int mtu)
{
    if (mtu > 0)
        mPendingMtu = mtu;

    if (type.isNull()) {
        mPending = HealthRecords::errorXml(QString(), QStringLiteral("request"));
    } else {
        const HealthRecords::Type t = HealthRecords::typeFromName(type);
        if (t == HealthRecords::Unknown) {
            mPending = HealthRecords::errorXml(type, QStringLiteral("type"));
        } else {
            const HealthRecords::Page page = HealthRecords::query(dataRoot(), t, since, max);
            mPending = HealthRecords::toXml(t, page);
            qDebug() << "Health request" << type << "since" << since << ":" << page.records.size()
                     << "records," << mPending.size() << "bytes" << (page.more ? "(more)" : "");
        }
    }
    QTimer::singleShot(0, this, &HealthService::sendPending);
}

void HealthService::sendPending()
{
    if (mPending.isEmpty())
        return;
    const QByteArray xml = mPending;
    mPending.clear();
    mData->send(xml, mPendingMtu);
}
