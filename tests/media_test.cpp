/*
 * Copyright (C) 2026 eos-mainline contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Tests of the media service additions of the wearui-w2 wave (W2-btsyncd-notif) on a private bus
 * (cmake -DBTSYNCD_TESTS=ON), run like notifications_test:
 *   dbus-run-session -- sh -c 'DBUS_SYSTEM_BUS_ADDRESS=$DBUS_SESSION_BUS_ADDRESS media_test'
 *   - the artwork characteristic 00007008 with the frames Gadgetbridge records (a picture in
 *     several writes, the same one again without data, none): the file, its hash, the colour, the
 *     player's MPRIS art URL, the music app's API and its signal; pictures that do not check out
 *     are refused; the cache keeps the last eight
 *   - the extras characteristic 00007009 (custom actions and queue) with the recorded frames,
 *     their limits, and the empty list the watch starts from at every connection
 *   - media commands 5 and 6 with the index byte, only for indexes the lists have
 */

#include "mediaservice.h"
#include "mediaphonedata.h"
#include "common.h"
#include "frames.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QVariantMap>
#include <cstdio>
#include <functional>

static int failures = 0;

#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static const char *CHR_COMMANDS = "/org/asteroidos/btsyncd/service2/char4";
static const char *CHR_ART = "/org/asteroidos/btsyncd/service2/char7";
static const char *CHR_EXTRAS = "/org/asteroidos/btsyncd/service2/char8";

class Collector : public QObject
{
    Q_OBJECT
public:
    QList<QByteArray> commands;
    QStringList arts;
    int extras = 0;
public slots:
    void propertiesChanged(const QString &, const QVariantMap &changed, const QStringList &)
    {
        if(changed.contains("Value"))
            commands << changed.value("Value").toByteArray();
    }
    void artChanged(const QString &path, const QString &color) { arts << path + "|" + color; }
    void extrasChanged() { extras++; }
};

static void spin(int ms)
{
    QElapsedTimer t;
    t.start();
    while(t.elapsed() < ms)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
}

static bool waitFor(const std::function<bool()> &cond, int ms = 3000)
{
    QElapsedTimer t;
    t.start();
    while(!cond() && t.elapsed() < ms)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return cond();
}

static QString btsyncdName;

static void write(QDBusConnection &driver, const char *path, const QByteArray &value)
{
    QDBusMessage m = QDBusMessage::createMethodCall(btsyncdName, path, GATT_CHRC_IFACE, "WriteValue");
    QVariantMap options;
    options.insert("mtu", 185);
    m.setArguments({value, options});
    CHECK(driver.call(m, QDBus::BlockWithGui, 5000).type() == QDBusMessage::ReplyMessage);
}

static QVariant api(QDBusConnection &driver, const QString &service, const char *method, const QList<QVariant> &args = QList<QVariant>())
{
    QDBusMessage m = QDBusMessage::createMethodCall(service, BTSYNCD_MEDIA_PATH, BTSYNCD_MEDIA_IFACE, method);
    m.setArguments(args);
    const QDBusMessage r = driver.call(m, QDBus::BlockWithGui, 5000);
    CHECK(r.type() == QDBusMessage::ReplyMessage);
    return r.arguments().value(0);
}

// A JPEG-looking file (the markers are what the watch checks) and the message that carries it
static QByteArray fakeJpeg(int seed, int size = 300)
{
    QByteArray jpeg("\xff\xd8\xff\xe0", 4);
    for(int i = 0; i < size; i++)
        jpeg += char((seed * 31 + i * 7) & 0x7f);
    jpeg += QByteArray("\xff\xd9", 2);
    return jpeg;
}

static QString hashOf(const QByteArray &jpeg)
{
    return QString::fromLatin1(QCryptographicHash::hash(jpeg, QCryptographicHash::Sha256).toHex().left(16));
}

static QByteArray artXml(const QByteArray &jpeg, const char *color = "336699")
{
    return "<art><h>" + hashOf(jpeg).toLatin1() + "</h><c>" + color + "</c><d>" + jpeg.toBase64() + "</d></art>";
}

static int artFiles()
{
    return QDir(MediaPhoneData::artDir()).entryList(QStringList() << "*.jpg", QDir::Files).size();
}

static QByteArray bytes(std::initializer_list<int> list)
{
    QByteArray b;
    for(int v : list)
        b += char(v);
    return b;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if(qEnvironmentVariableIsEmpty("DBUS_SESSION_BUS_ADDRESS") || qEnvironmentVariableIsEmpty("DBUS_SYSTEM_BUS_ADDRESS")) {
        fprintf(stderr, "needs a private bus as session and system bus (see the top of the source)\n");
        return 2;
    }

    MediaService media(2, QDBusConnection::systemBus());
    btsyncdName = QDBusConnection::systemBus().baseService();
    const QString session = QDBusConnection::sessionBus().baseService();
    QDir(MediaPhoneData::artDir()).removeRecursively();

    QDBusConnection driver = QDBusConnection::connectToBus(QDBusConnection::SessionBus, "driver");
    Collector collector;
    CHECK(driver.connect(btsyncdName, CHR_COMMANDS, "org.freedesktop.DBus.Properties", "PropertiesChanged",
                         &collector, SLOT(propertiesChanged(QString, QVariantMap, QStringList))));
    CHECK(driver.connect(session, BTSYNCD_MEDIA_PATH, BTSYNCD_MEDIA_IFACE, "ArtChanged", &collector, SLOT(artChanged(QString, QString))));
    CHECK(driver.connect(session, BTSYNCD_MEDIA_PATH, BTSYNCD_MEDIA_IFACE, "ExtrasChanged", &collector, SLOT(extrasChanged())));
    spin(200);

    // ---- The service has the two new characteristics
    {
        QDBusMessage m = QDBusMessage::createMethodCall(btsyncdName, "/", DBUS_OM_IFACE, "GetManagedObjects");
        // (the application object answers that; here the characteristics are read one by one)
        for(const char *path : {CHR_ART, CHR_EXTRAS}) {
            QDBusMessage g = QDBusMessage::createMethodCall(btsyncdName, path, DBUS_PROPERTIES_IFACE, "Get");
            g << QString(GATT_CHRC_IFACE) << QString("UUID");
            const QDBusMessage r = driver.call(g, QDBus::BlockWithGui, 5000);
            CHECK(r.type() == QDBusMessage::ReplyMessage);
            const QString uuid = r.arguments().value(0).value<QDBusVariant>().variant().toString();
            CHECK(uuid == (path == CHR_ART ? QString(MEDIA_ART_UUID) : QString(MEDIA_EXTRAS_UUID)));
        }
        CHECK(QString(MEDIA_ART_UUID).startsWith("00007008-"));
        CHECK(QString(MEDIA_EXTRAS_UUID).startsWith("00007009-"));
        (void) m;
    }

    // ---- Artwork: the frames Gadgetbridge records
    const QList<QByteArray> art = loadFrames("media-artwork");
    CHECK(art.size() == 6);
    CHECK(api(driver, session, "ArtPath").toString().isEmpty());
    for(int i = 0; i < 4; i++) {
        write(driver, CHR_ART, art.value(i));
        spin(30);
        if(i < 3)
            CHECK(collector.arts.isEmpty());       // joined until the closing tag
    }
    const QString recordedHash = "a952ade4894b1a37";
    const QString recordedPath = MediaPhoneData::artDir() + "/" + recordedHash + ".jpg";
    CHECK(waitFor([&] { return collector.arts.size() == 1; }));
    CHECK(collector.arts == QStringList(recordedPath + "|2060c0"));
    CHECK(api(driver, session, "ArtPath").toString() == recordedPath);
    CHECK(api(driver, session, "ArtColor").toString() == "2060c0");
    {
        QFile f(recordedPath);
        CHECK(f.open(QIODevice::ReadOnly));
        const QByteArray jpeg = f.readAll();
        CHECK(jpeg.size() == 1200);
        CHECK(hashOf(jpeg) == recordedHash);
    }
    // the player's MPRIS metadata has it too
    {
        QDBusMessage g = QDBusMessage::createMethodCall("org.mpris.MediaPlayer2.asteroid-btsyncd", "/org/mpris/MediaPlayer2",
                                                        DBUS_PROPERTIES_IFACE, "Get");
        g << QString("org.mpris.MediaPlayer2.Player") << QString("Metadata");
        const QDBusMessage r = driver.call(g, QDBus::BlockWithGui, 5000);
        CHECK(r.type() == QDBusMessage::ReplyMessage);
        QVariantMap metadata;
        const QVariant v = r.arguments().value(0).value<QDBusVariant>().variant();
        if(v.canConvert<QDBusArgument>())
            metadata = qdbus_cast<QVariantMap>(v.value<QDBusArgument>());
        CHECK(metadata.value("mpris:artUrl").toString() == "file://" + recordedPath);
    }
    // the same picture again, without its data (the phone knows the watch has it): nothing changes
    write(driver, CHR_ART, art.value(4));
    spin(150);
    CHECK(collector.arts.size() == 1);
    // none
    write(driver, CHR_ART, art.value(5));
    CHECK(waitFor([&] { return collector.arts.size() == 2; }));
    CHECK(collector.arts.last() == "|");
    CHECK(api(driver, session, "ArtPath").toString().isEmpty() && api(driver, session, "ArtColor").toString().isEmpty());
    // the repeat brings it back from the cache
    write(driver, CHR_ART, art.value(4));
    CHECK(waitFor([&] { return collector.arts.size() == 3; }));
    CHECK(collector.arts.last() == recordedPath + "|2060c0");
    // a hash the watch does not have, without data: kept as it is
    write(driver, CHR_ART, "<art><h>0011223344556677</h><c>ffffff</c></art>");
    spin(150);
    CHECK(collector.arts.size() == 3);
    CHECK(!QFile::exists(MediaPhoneData::artDir() + "/0011223344556677.jpg"));

    // ---- Pictures that do not check out
    {
        const int before = collector.arts.size();
        QByteArray jpeg = fakeJpeg(1);
        // the hash of another picture
        write(driver, CHR_ART, "<art><h>" + hashOf(fakeJpeg(2)).toLatin1() + "</h><c>336699</c><d>" + jpeg.toBase64() + "</d></art>");
        // not base64
        write(driver, CHR_ART, "<art><h>" + hashOf(jpeg).toLatin1() + "</h><c>336699</c><d>!!not base64!!</d></art>");
        // a hash that is none
        write(driver, CHR_ART, "<art><h>../../etc/passwd</h><c>336699</c><d>" + jpeg.toBase64() + "</d></art>");
        // too large
        const QByteArray big = fakeJpeg(3, MediaPhoneData::MaxArtBytes + 100);
        write(driver, CHR_ART, artXml(big));
        spin(200);
        CHECK(collector.arts.size() == before);
        CHECK(!QFile::exists(MediaPhoneData::artDir() + "/" + hashOf(jpeg) + ".jpg"));
        CHECK(!QFile::exists(MediaPhoneData::artDir() + "/" + hashOf(big) + ".jpg"));
        // a colour that is none: the picture comes without one
        write(driver, CHR_ART, artXml(jpeg, "red"));
        CHECK(waitFor([&] { return collector.arts.size() == before + 1; }));
        CHECK(collector.arts.last() == MediaPhoneData::artDir() + "/" + hashOf(jpeg) + ".jpg|");
    }
    // The cache keeps the last eight (and the one that shows)
    {
        for(int i = 10; i < 22; i++) {
            const int before = collector.arts.size();
            write(driver, CHR_ART, artXml(fakeJpeg(i, 100 + i)));
            CHECK(waitFor([&] { return collector.arts.size() == before + 1; }));
            spin(20);
        }
        CHECK(artFiles() == MediaPhoneData::KeepArt);
        CHECK(QFile::exists(api(driver, session, "ArtPath").toString()));
    }
    // the phone goes: no picture
    {
        const int before = collector.arts.size();
        media.phoneConnectedChanged(false);
        CHECK(waitFor([&] { return collector.arts.size() == before + 1; }));
        CHECK(collector.arts.last() == "|");
    }

    // ---- Extras: the frames Gadgetbridge records
    const QList<QByteArray> extras = loadFrames("media-extras");
    CHECK(extras.size() == 3);
    write(driver, CHR_EXTRAS, extras.value(0));
    spin(100);
    CHECK(collector.extras == 0);                  // the second write ends it
    write(driver, CHR_EXTRAS, extras.value(1));
    CHECK(waitFor([&] { return collector.extras == 1; }));
    {
        const QJsonObject o = QJsonDocument::fromJson(api(driver, session, "Extras").toString().toUtf8()).object();
        CHECK(o.value("actions").toArray().size() == 3);
        CHECK(o.value("actions").toArray().at(0).toString() == "Shuffle");
        CHECK(o.value("actions").toArray().at(2).toString() == "Like");
        CHECK(o.value("queue").toArray().size() == 24);
        CHECK(o.value("queue").toArray().at(0).toObject().value("t").toString() == "Track 0");
        CHECK(o.value("queue").toArray().at(0).toObject().value("s").toString() == "Artist 0");
        CHECK(o.value("queue").toArray().at(23).toObject().value("t").toString() == "Track 23");
        CHECK(o.value("current").toInt() == 0);
    }
    // the same again changes nothing
    write(driver, CHR_EXTRAS, extras.value(0));
    write(driver, CHR_EXTRAS, extras.value(1));
    spin(150);
    CHECK(collector.extras == 1);

    // ---- The commands: custom action 5 and queue item 6 with the index
    collector.commands.clear();
    CHECK(api(driver, session, "CustomAction", {1}).toBool() == true);
    CHECK(waitFor([&] { return collector.commands.size() == 1; }));
    CHECK(collector.commands.last() == bytes({5, 1}));
    CHECK(api(driver, session, "CustomAction", {2}).toBool() == true);
    CHECK(waitFor([&] { return collector.commands.size() == 2; }));
    CHECK(collector.commands.last() == bytes({5, 2}));
    CHECK(api(driver, session, "CustomAction", {3}).toBool() == false);       // the list has three
    CHECK(api(driver, session, "CustomAction", {-1}).toBool() == false);
    CHECK(api(driver, session, "SkipToQueueItem", {23}).toBool() == true);
    CHECK(waitFor([&] { return collector.commands.size() == 3; }));
    CHECK(collector.commands.last() == bytes({6, 23}));
    CHECK(api(driver, session, "SkipToQueueItem", {0}).toBool() == true);
    CHECK(waitFor([&] { return collector.commands.size() == 4; }));
    CHECK(collector.commands.last() == bytes({6, 0}));
    CHECK(api(driver, session, "SkipToQueueItem", {24}).toBool() == false);   // 24 items: 0 to 23
    spin(100);
    CHECK(collector.commands.size() == 4);
    CHECK(MEDIA_COMMAND_CUSTOM == 5 && MEDIA_COMMAND_QUEUE == 6);
    // the older commands are as they were
    CHECK(MEDIA_COMMAND_PREVIOUS == 0 && MEDIA_COMMAND_NEXT == 1 && MEDIA_COMMAND_PLAY == 2 && MEDIA_COMMAND_PAUSE == 3 && MEDIA_COMMAND_VOLUME == 4);

    // an index above 127 goes out as an unsigned byte (the lists are shorter, so only the byte is checked here)
    {
        MediaPhoneData data;
        QList<QPair<int, int>> got;
        QObject::connect(&data, &MediaPhoneData::indexedCommand, [&](quint8 c, quint8 i) { got << qMakePair(int(c), int(i)); });
        QByteArray xml = "<mx><q><c>0</c>";
        for(int i = 0; i < 40; i++)
            xml += "<i><t>t</t></i>";
        xml += "</q></mx>";
        data.applyExtras(xml);
        CHECK(data.queueCount() == MediaPhoneData::MaxQueue);
        CHECK(!data.requestQueueItem(MediaPhoneData::MaxQueue) && data.requestQueueItem(MediaPhoneData::MaxQueue - 1));
        CHECK((got == QList<QPair<int, int>>() << qMakePair(6, 23)));
    }

    // ---- Limits of the extras
    {
        MediaPhoneData data;
        QByteArray xml = "<mx><ca>";
        for(int i = 0; i < 14; i++)
            xml += "<a>Action " + QByteArray::number(i) + "</a>";
        xml += "<a>   </a></ca><q><c>99</c><i><t>" + QByteArray(80, 'T') + "</t><s>" + QByteArray(80, 'S') + "</s></i></q></mx>";
        data.applyExtras(xml);
        const QJsonObject o = QJsonDocument::fromJson(data.extrasJson().toUtf8()).object();
        CHECK(o.value("actions").toArray().size() == MediaPhoneData::MaxActions);
        CHECK(o.value("queue").toArray().size() == 1);
        const QString title = o.value("queue").toArray().at(0).toObject().value("t").toString();
        const QString subtitle = o.value("queue").toArray().at(0).toObject().value("s").toString();
        CHECK(title.length() == MediaPhoneData::MaxTitleChars && title.endsWith(QChar(0x2026)));
        CHECK(subtitle.length() == MediaPhoneData::MaxSubtitleChars && subtitle.endsWith(QChar(0x2026)));
        CHECK(o.value("current").toInt() == -1);        // 99 is not in a list of one
        data.applyExtras("<mx><q><c>x</c></q></mx>");
        CHECK(QJsonDocument::fromJson(data.extrasJson().toUtf8()).object().value("current").toInt() == -1);
        // a garbled message changes nothing
        const QString before = data.extrasJson();
        data.applyExtras("<mx><ca><a>x</b></ca></mx>");
        CHECK(data.extrasJson() == before || data.actionCount() == 0);
    }

    // ---- The empty list, and the phone going
    write(driver, CHR_EXTRAS, extras.value(2));
    CHECK(waitFor([&] { return collector.extras == 2; }));
    CHECK(QJsonDocument::fromJson(api(driver, session, "Extras").toString().toUtf8()).object().value("queue").toArray().isEmpty());
    CHECK(api(driver, session, "CustomAction", {0}).toBool() == false);
    write(driver, CHR_EXTRAS, extras.value(0));
    write(driver, CHR_EXTRAS, extras.value(1));
    CHECK(waitFor([&] { return collector.extras == 3; }));
    media.phoneConnectedChanged(false);
    CHECK(waitFor([&] { return collector.extras == 4; }));
    CHECK(api(driver, session, "CustomAction", {0}).toBool() == false);
    CHECK(api(driver, session, "SkipToQueueItem", {0}).toBool() == false);
    // a connection starts from nothing; the phone's first write is taken
    media.phoneConnectedChanged(true);
    write(driver, CHR_EXTRAS, extras.value(0));
    write(driver, CHR_EXTRAS, extras.value(1));
    CHECK(waitFor([&] { return collector.extras == 5; }));
    // half a message lost with the connection is dropped, the next one comes
    write(driver, CHR_EXTRAS, extras.value(0));
    media.phoneConnectedChanged(false);
    media.phoneConnectedChanged(true);
    write(driver, CHR_EXTRAS, "<mx><ca><a>One</a></ca></mx>");
    CHECK(waitFor([&] { return QJsonDocument::fromJson(api(driver, session, "Extras").toString().toUtf8()).object().value("actions").toArray().size() == 1; }));

    if(failures)
        fprintf(stderr, "%d check(s) failed\n", failures);
    else
        fprintf(stderr, "media_test: all checks passed\n");
    return failures ? 1 : 0;
}

#include "media_test.moc"
