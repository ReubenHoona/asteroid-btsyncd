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

#include <MDConfItem>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QXmlStreamReader>

#include "weatherservice.h"
#include "characteristic.h"
#include "common.h"
#include "messagejoiner.h"
#include "wakehold.h"
#include <time.h>
#include <math.h>
#include <cmath>

int getQByteArrayInt(QByteArray arr, int index) {
    return (((unsigned char) arr[index * 2]) << 8) | ((unsigned char) arr[index * 2 + 1]);
}

class WeatherCityChrc : public Characteristic
{
public:
    WeatherCityChrc(QDBusConnection bus, int index, Service *service) : Characteristic(bus, index, WEAT_CITY_UUID, {"encrypt-authenticated-write","encrypt-authenticated-notify"}, service) {}

public slots:
    void WriteValue(QByteArray value, QVariantMap)
    {
        MDConfItem("/org/asteroidos/weather/city-name").set(QString::fromUtf8(value));
        // A city without a location after it (an older phone): the old
        // location is not this city's. The phone sends the location next.
        MDConfItem("/org/asteroidos/weather/phone-location/latitude").unset();
        MDConfItem("/org/asteroidos/weather/phone-location/longitude").unset();
    }
};

// "LAT,LON" in degrees (UTF-8, '.' decimals, rounded by the phone to 0.01):
// where the forecast is for. eos-weather uses it when the phone is away.
class WeatherLocationChrc : public Characteristic
{
public:
    WeatherLocationChrc(QDBusConnection bus, int index, Service *service) : Characteristic(bus, index, WEAT_LOC_UUID, {"encrypt-authenticated-write"}, service) {}

public slots:
    void WriteValue(QByteArray value, QVariantMap)
    {
        const QList<QByteArray> parts = value.trimmed().split(',');
        bool okLat = false, okLon = false;
        const double lat = parts.size() == 2 ? parts[0].trimmed().toDouble(&okLat) : 0;
        const double lon = parts.size() == 2 ? parts[1].trimmed().toDouble(&okLon) : 0;
        if (!okLat || !okLon || !std::isfinite(lat) || !std::isfinite(lon) ||
            fabs(lat) > 90 || fabs(lon) > 180 || (lat == 0 && lon == 0))
            return;
        MDConfItem("/org/asteroidos/weather/phone-location/latitude").set(round(lat * 100) / 100);
        MDConfItem("/org/asteroidos/weather/phone-location/longitude").set(round(lon * 100) / 100);
    }
};

class WeatherIdsChrc : public Characteristic
{
public:
    WeatherIdsChrc(QDBusConnection bus, int index, Service *service) : Characteristic(bus, index, WEAT_IDS_UUID, {"encrypt-authenticated-write"}, service) {}

public slots:
    void WriteValue(QByteArray value, QVariantMap)
    {
        if (!hasMinLength(value, 10)) // 5 days * 2 bytes
            return;
        for(int i = 0; i < 5; i++)
            MDConfItem(QString("/org/asteroidos/weather/day%1/id").arg(i)).set(getQByteArrayInt(value, i));

        MDConfItem("/org/asteroidos/weather/timestamp-day0").set((int)time(NULL));
    }
};

class WeatherMinTempsChrc : public Characteristic
{
public:
    WeatherMinTempsChrc(QDBusConnection bus, int index, Service *service) : Characteristic(bus, index, WEAT_MINT_UUID, {"encrypt-authenticated-write"}, service) {}

public slots:
    void WriteValue(QByteArray value, QVariantMap)
    {
        if (!hasMinLength(value, 10)) // 5 days * 2 bytes
            return;
        for(int i = 0; i < 5; i++)
            MDConfItem(QString("/org/asteroidos/weather/day%1/min-temp").arg(i)).set(getQByteArrayInt(value, i));
    }
};

class WeatherMaxTempsChrc : public Characteristic
{
public:
    WeatherMaxTempsChrc(QDBusConnection bus, int index, Service *service) : Characteristic(bus, index, WEAT_MAXT_UUID, {"encrypt-authenticated-write"}, service) {}

public slots:
    void WriteValue(QByteArray value, QVariantMap)
    {
        if (!hasMinLength(value, 10)) // 5 days * 2 bytes
            return;
        for(int i = 0; i < 5; i++)
            MDConfItem(QString("/org/asteroidos/weather/day%1/max-temp").arg(i)).set(getQByteArrayInt(value, i));
    }
};

bool WeatherService::parseExtended(const QByteArray &xml, QString *json)
{
    QXmlStreamReader reader(xml);
    if(!reader.readNextStartElement() || reader.name() != QLatin1String("wx"))
        return false;
    const QString text = reader.readElementText();
    if(reader.hasError())
        return false;
    const QByteArray bytes = text.toUtf8();
    if(bytes.isEmpty() || bytes.size() > MaxJsonBytes)
        return false;
    QJsonParseError error;
    const QJsonDocument doc = QJsonDocument::fromJson(bytes, &error);
    if(error.error != QJsonParseError::NoError || !doc.isObject())
        return false;
    const QJsonObject o = doc.object();
    if(o.value(QStringLiteral("v")).toInt(-1) != 1)
        return false;
    // What the weather app indexes into must be what it expects
    if(o.contains(QStringLiteral("now")) && !o.value(QStringLiteral("now")).isObject())
        return false;
    if(o.contains(QStringLiteral("hours")) && (!o.value(QStringLiteral("hours")).isArray() || o.value(QStringLiteral("hours")).toArray().size() > 24))
        return false;
    if(o.contains(QStringLiteral("days")) && (!o.value(QStringLiteral("days")).isArray() || o.value(QStringLiteral("days")).toArray().size() > 10))
        return false;
    if(o.contains(QStringLiteral("aqi")) && !o.value(QStringLiteral("aqi")).isObject())
        return false;
    *json = text;
    return true;
}

QByteArray WeatherService::capabilities()
{
    return QByteArrayLiteral("<capabilities><version>1</version><extended>1</extended></capabilities>");
}

class WeatherExtendedChrc : public Characteristic
{
public:
    WeatherExtendedChrc(QDBusConnection bus, int index, Service *service)
        : Characteristic(bus, index, WEAT_EXT_UUID, {"encrypt-authenticated-write"}, service),
          // escaping can make the XML longer than the JSON it holds
          mJoiner("Weather", {"wx"}, 3 * WeatherService::MaxJsonBytes) {}

public slots:
    void WriteValue(QByteArray value, QVariantMap options)
    {
        // The phone may write while the watch sleeps: awake until it is stored
        WakeHoldScope wake;
        for(const auto &message : mJoiner.add(value, MessageJoiner::offsetOf(options))) {
            QString json;
            if(!WeatherService::parseExtended(message.second, &json)) {
                qWarning() << "Weather: a forecast that does not parse, ignored," << message.second.size() << "bytes";
                continue;
            }
            MDConfItem(WeatherService::ExtendedKey).set(json);
            MDConfItem("/org/asteroidos/weather/timestamp-day0").set((int)time(NULL));
        }
    }

private:
    MessageJoiner mJoiner;
};

class WeatherCapabilitiesChrc : public Characteristic
{
public:
    WeatherCapabilitiesChrc(QDBusConnection bus, int index, Service *service)
        : Characteristic(bus, index, WEAT_CAPS_UUID, {"encrypt-authenticated-read"}, service) {}

public slots:
    QByteArray ReadValue(QVariantMap options)
    {
        const QByteArray value = WeatherService::capabilities();
        const int offset = options.value("offset", 0).toInt();
        return offset > 0 ? value.mid(offset) : value;
    }
};

WeatherService::WeatherService(int index, QDBusConnection bus, QObject *parent) : Service(bus, index, WEATHER_UUID, parent)
{
    addCharacteristic(new WeatherCityChrc(bus, 0, this));
    addCharacteristic(new WeatherIdsChrc(bus, 1, this));
    addCharacteristic(new WeatherMaxTempsChrc(bus, 2, this));
    addCharacteristic(new WeatherMinTempsChrc(bus, 3, this));
    addCharacteristic(new WeatherLocationChrc(bus, 4, this));
    addCharacteristic(new WeatherExtendedChrc(bus, 5, this));
    addCharacteristic(new WeatherCapabilitiesChrc(bus, 6, this));
}
