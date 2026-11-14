/*
 * Tests of the weather service on a private D-Bus (cmake -DBTSYNCD_TESTS=ON),
 * run like notifications_test:
 *   dbus-run-session -- sh -c 'DBUS_SYSTEM_BUS_ADDRESS=$DBUS_SESSION_BUS_ADDRESS weather_test'
 *   - city, condition ids, min and max temperatures (5 days) -> dconf
 *   - the forecast's location (00008005, "LAT,LON") -> phone-location/{latitude,longitude},
 *     rounded to 0.01; bad values ignored; a city write unsets it (a phone that
 *     sends no location leaves no other city's coordinates behind)
 *   - the full forecast (00008006, <wx>JSON</wx> from W2-gb-data's frames) -> dconf
 *     /org/asteroidos/weather/extended, the capabilities (00008007) say extended 1
 * Greenwich only (51.48, 0.00).
 */

#include "weatherservice.h"
#include "common.h"
#include "fixtures.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QVariantMap>
#include <MDConfItem>
#include <cmath>
#include <cstdio>
#include <functional>

static int failures = 0;

#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

// WeatherService(index 0): char0 city, char1 ids, char2 max temps, char3 min temps, char4 location
static const char *CHR_CITY = "/org/asteroidos/btsyncd/service0/char0";
static const char *CHR_IDS = "/org/asteroidos/btsyncd/service0/char1";
static const char *CHR_MAX = "/org/asteroidos/btsyncd/service0/char2";
static const char *CHR_MIN = "/org/asteroidos/btsyncd/service0/char3";
static const char *CHR_LOC = "/org/asteroidos/btsyncd/service0/char4";
static const char *CHR_EXT = "/org/asteroidos/btsyncd/service0/char5";
static const char *CHR_CAPS = "/org/asteroidos/btsyncd/service0/char6";

static const char *LAT = "/org/asteroidos/weather/phone-location/latitude";
static const char *LON = "/org/asteroidos/weather/phone-location/longitude";

static QString btsyncdName;

static void spin(int ms)
{
    QElapsedTimer t;
    t.start();
    while(t.elapsed() < ms)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
}

static bool write(QDBusConnection &driver, const char *path, const QByteArray &value)
{
    QDBusMessage m = QDBusMessage::createMethodCall(btsyncdName, path, GATT_CHRC_IFACE, "WriteValue");
    QVariantMap options;
    options.insert("mtu", 23);
    m.setArguments({value, options});
    const bool ok = driver.call(m, QDBus::BlockWithGui, 5000).type() == QDBusMessage::ReplyMessage;
    spin(50);
    return ok;
}

// dconf: another client sees a write once dconf-service has stored it
static bool waitFor(const std::function<bool()> &cond, int ms = 10000)
{
    QElapsedTimer t;
    t.start();
    while(!cond() && t.elapsed() < ms)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return cond();
}

#define EVENTUALLY(cond) CHECK(waitFor([&] { return (cond); }))

static QByteArray words(std::initializer_list<int> values)
{
    QByteArray b;
    for(int v : values) {
        b.append((char)((v >> 8) & 0xff));
        b.append((char)(v & 0xff));
    }
    return b;
}

static QVariant value(const char *key)
{
    return MDConfItem(key).value();
}

static bool near(const QVariant &v, double expected)
{
    return v.isValid() && std::fabs(v.toDouble() - expected) < 1e-9;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if(qEnvironmentVariableIsEmpty("DBUS_SESSION_BUS_ADDRESS") || qEnvironmentVariableIsEmpty("DBUS_SYSTEM_BUS_ADDRESS")) {
        fprintf(stderr, "needs a private bus as session and system bus (see the top of the source)\n");
        return 2;
    }

    WeatherService weather(0, QDBusConnection::systemBus());
    btsyncdName = QDBusConnection::systemBus().baseService();
    QDBusConnection driver = QDBusConnection::connectToBus(QDBusConnection::SessionBus, "driver");
    spin(200);

    // A forecast as Gadgetbridge sends it: the city, its location, 5 days
    CHECK(write(driver, CHR_CITY, "London"));
    EVENTUALLY(value("/org/asteroidos/weather/city-name").toString() == "London");
    CHECK(write(driver, CHR_LOC, "51.48,0.00"));
    EVENTUALLY(near(value(LAT), 51.48));
    EVENTUALLY(near(value(LON), 0.0));
    CHECK(write(driver, CHR_IDS, words({801, 804, 801, 804, 804, 804, 804})));
    CHECK(write(driver, CHR_MIN, words({282, 284, 282, 283, 283, 283, 281})));
    CHECK(write(driver, CHR_MAX, words({288, 289, 288, 286, 285, 286, 286})));
    EVENTUALLY(value("/org/asteroidos/weather/day0/id").toInt() == 801);
    EVENTUALLY(value("/org/asteroidos/weather/day4/id").toInt() == 804);
    EVENTUALLY(value("/org/asteroidos/weather/day0/min-temp").toInt() == 282);
    EVENTUALLY(value("/org/asteroidos/weather/day4/min-temp").toInt() == 283);
    EVENTUALLY(value("/org/asteroidos/weather/day0/max-temp").toInt() == 288);
    EVENTUALLY(value("/org/asteroidos/weather/day4/max-temp").toInt() == 285);
    EVENTUALLY(value("/org/asteroidos/weather/timestamp-day0").toInt() > 0);
    // fewer than 5 days: ignored
    CHECK(write(driver, CHR_IDS, words({800, 800})));
    spin(500);
    CHECK(value("/org/asteroidos/weather/day0/id").toInt() == 801);

    // rounded again on the watch; spaces and '-' are fine
    CHECK(write(driver, CHR_LOC, " 51.4779 , -0.0015 "));
    EVENTUALLY(near(value(LAT), 51.48) && near(value(LON), 0.0));
    CHECK(write(driver, CHR_LOC, "-12.3456,-120.5"));
    EVENTUALLY(near(value(LAT), -12.35) && near(value(LON), -120.5));

    // bad values change nothing
    for(const char *bad : {"", "51.48", "51.48,0.00,1", "abc,def", "91,0", "0,181", "0,0", "nan,1", "inf,1", "51.48;0.00"}) {
        CHECK(write(driver, CHR_LOC, bad));
        spin(300);
        // (a loaded machine: give dconf time to settle; a wrong value never settles back)
        if(!waitFor([&] { return near(value(LAT), -12.35) && near(value(LON), -120.5); }, 3000)) {
            fprintf(stderr, "FAIL: \"%s\" changed the location\n", bad);
            failures++;
        }
    }

    // a city without a location after it (a phone or app without coordinates): unset
    CHECK(write(driver, CHR_CITY, "Greenwich"));
    EVENTUALLY(value("/org/asteroidos/weather/city-name").toString() == "Greenwich");
    EVENTUALLY(!value(LAT).isValid() && !value(LON).isValid());

    // ---- the full forecast ----
    static const char *EXT = "/org/asteroidos/weather/extended";
    static const char *DAY0 = "/org/asteroidos/weather/timestamp-day0";
    // what the watch says it takes: the phone writes 00008006 only when it reads extended 1
    {
        QDBusMessage m = QDBusMessage::createMethodCall(btsyncdName, CHR_CAPS, GATT_CHRC_IFACE, "ReadValue");
        m.setArguments({QVariantMap()});
        const QDBusMessage r = driver.call(m, QDBus::BlockWithGui, 5000);
        CHECK(r.type() == QDBusMessage::ReplyMessage);
        CHECK(r.arguments().value(0).toByteArray() == "<capabilities><version>1</version><extended>1</extended></capabilities>");
        QVariantMap off;
        off.insert("offset", 14);
        m.setArguments({off});
        CHECK(driver.call(m, QDBus::BlockWithGui, 5000).arguments().value(0).toByteArray()
              == QByteArray("<capabilities><version>1</version><extended>1</extended></capabilities>").mid(14));
    }
    const QByteArray json = fixtureFile("weather-extended-greenwich.json").trimmed();
    const QList<QByteArray> frames = fixtureFrames("weather-extended-greenwich.frames.txt");
    CHECK(frames.size() == 5 && frames.value(0).size() == 500 && json.size() < WeatherService::MaxJsonBytes);
    MDConfItem(DAY0).set(1);
    // the five 500-byte writes of Gadgetbridge join into one forecast, stored as it came
    for(const QByteArray &f : frames)
        CHECK(write(driver, CHR_EXT, f));
    EVENTUALLY(value(EXT).toString().toUtf8() == json);
    EVENTUALLY(value(DAY0).toInt() > 1);
    {
        const QJsonObject o = QJsonDocument::fromJson(value(EXT).toString().toUtf8()).object();
        CHECK(o.value("v").toInt() == 1 && o.value("hours").toArray().size() == 24 && o.value("days").toArray().size() == 8);
        CHECK(o.value("now").toObject().contains("c") && o.value("aqi").isObject() == o.contains("aqi"));
    }
    // a message cut off by a disconnection, then a new one: the new one is stored
    MDConfItem(DAY0).set(1);
    CHECK(write(driver, CHR_EXT, frames.value(0)));
    CHECK(write(driver, CHR_EXT, frames.value(1)));
    for(const QByteArray &f : frames)
        CHECK(write(driver, CHR_EXT, f));
    EVENTUALLY(value(DAY0).toInt() > 1);
    CHECK(value(EXT).toString().toUtf8() == json);
    // XML escaping: & < > only, as the phone writes it
    CHECK(write(driver, CHR_EXT, "<wx>{\"v\":1,\"t\":5,\"by\":\"A &amp; B &lt;c&gt;\"}</wx>"));
    EVENTUALLY(value(EXT).toString() == "{\"v\":1,\"t\":5,\"by\":\"A & B <c>\"}");
    // what is not a forecast changes nothing: other versions, not JSON, not an object, too many
    // hours or days, the wrong kind of "now", more than 8 KiB, an empty one
    const QString kept = value(EXT).toString();
    QByteArray hours25 = "<wx>{\"v\":1,\"hours\":[";
    for(int i = 0; i < 25; i++)
        hours25 += QByteArray(i ? "," : "") + "{\"t\":" + QByteArray::number(i) + "}";
    hours25 += "]}</wx>";
    QByteArray days11 = "<wx>{\"v\":1,\"days\":[";
    for(int i = 0; i < 11; i++)
        days11 += QByteArray(i ? "," : "") + "{\"d\":" + QByteArray::number(i) + "}";
    days11 += "]}</wx>";
    // 24 hours and 10 days are fine
    QByteArray hours24 = "<wx>{\"v\":1,\"t\":6,\"hours\":[";
    for(int i = 0; i < 24; i++)
        hours24 += QByteArray(i ? "," : "") + "{\"t\":" + QByteArray::number(i) + "}";
    hours24 += "]}</wx>";
    QByteArray big = "<wx>{\"v\":1,\"by\":\"" + QByteArray(WeatherService::MaxJsonBytes, 'x') + "\"}</wx>";
    for(const QByteArray &bad : {QByteArray("<wx>{\"v\":2,\"t\":7}</wx>"), QByteArray("<wx>{\"t\":7}</wx>"),
                                 QByteArray("<wx>not json</wx>"), QByteArray("<wx>[1,2]</wx>"), QByteArray("<wx></wx>"),
                                 QByteArray("<wx>{\"v\":1,\"now\":5}</wx>"), QByteArray("<wx>{\"v\":1,\"hours\":{}}</wx>"),
                                 QByteArray("<wx>{\"v\":1,\"aqi\":[]}</wx>"), hours25, days11, big}) {
        CHECK(write(driver, CHR_EXT, bad));
        spin(150);
        if(value(EXT).toString() != kept) {
            fprintf(stderr, "FAIL: a forecast that is none changed the stored one: %s\n", bad.left(40).constData());
            failures++;
        }
    }
    CHECK(write(driver, CHR_EXT, hours24));
    EVENTUALLY(value(EXT).toString().contains("\"t\":6"));
    // the old characteristics are untouched by all of this
    CHECK(value("/org/asteroidos/weather/day0/id").toInt() == 801);

    if(failures)
        fprintf(stderr, "%d check(s) failed\n", failures);
    else
        fprintf(stderr, "weather_test: all checks passed\n");
    return failures ? 1 : 0;
}
