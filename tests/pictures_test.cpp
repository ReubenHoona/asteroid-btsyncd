/*
 * Tests of the notification pictures (capabilities <pictures>) on a private D-Bus, like
 * notifications_test (cmake -DBTSYNCD_TESTS=ON):
 *   dbus-run-session -- sh -c 'DBUS_SYSTEM_BUS_ADDRESS=$DBUS_SESSION_BUS_ADDRESS pictures_test'
 * A fake lipstick (org.freedesktop.Notifications, its own thread, with GetNotifications)
 * records what btsyncd posts; the test writes the GATT characteristics as BlueZ does.
 *   - capabilities: <pictures>384</pictures><picturebytes>24576</picturebytes>
 *   - an insert with <pc>: the card's x-asteroid-picture and -size hints, feedback
 *     ty=picture tx=ID, PictureState pending; no second request within the minute
 *   - reassembly: the picture in 500-byte writes, and as a long write with offsets;
 *     stored as ID.jpg byte for byte, PictureChanged(path, true), PictureState ready
 *   - a disconnection in the middle of a picture: the part is dropped, the card's
 *     picture is not available meanwhile; the phone's resync after it is back asks again
 *     and the whole picture then arrives
 *   - checks: a wrong hash, a wrong size, not a JPEG: asked for once more, then given up
 *     (PictureChanged false, PictureState unavailable); the phone's "cannot send it"
 *   - an update with the same picture asks for nothing, with another picture asks for
 *     it and the old file goes; re-posting a conversation keeps its picture
 *   - cleanup: the phone removes the card, the user dismisses it, lipstick expires it:
 *     the file goes; a picture for a card that went is not stored; files no card shows
 *     (also after a restart: lipstick's list) go; the cache's limits
 *   - an insert without <pc>: no picture hints, no request (older phones)
 */

#include "notificationservice.h"
#include "common.h"

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
#include <QMutex>
#include <QMutexLocker>
#include <QRandomGenerator>
#include <QStandardPaths>
#include <QThread>
#include <QVariantMap>
#include <cstdio>
#include <functional>

static int failures = 0;

#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static const char *CHR_UPDATE = "/org/asteroidos/btsyncd/service0/char0";
static const char *CHR_FEEDBACK = "/org/asteroidos/btsyncd/service0/char1";
static const char *CHR_CAPS = "/org/asteroidos/btsyncd/service0/char3";
static const char *CHR_PICTURE = "/org/asteroidos/btsyncd/service0/char5";

// One notification as lipstick lists it: a(susssasa{sv}i)
struct Listed {
    QString appName;
    uint id = 0;
    QString appIcon, summary, body;
    QStringList actions;
    QVariantMap hints;
    int timeout = 0;
};
Q_DECLARE_METATYPE(Listed)
Q_DECLARE_METATYPE(QList<Listed>)

QDBusArgument &operator<<(QDBusArgument &arg, const Listed &n)
{
    arg.beginStructure();
    arg << n.appName << n.id << n.appIcon << n.summary << n.body << n.actions << n.hints << n.timeout;
    arg.endStructure();
    return arg;
}

const QDBusArgument &operator>>(const QDBusArgument &arg, Listed &n)
{
    arg.beginStructure();
    arg >> n.appName >> n.id >> n.appIcon >> n.summary >> n.body >> n.actions >> n.hints >> n.timeout;
    arg.endStructure();
    return arg;
}

struct Posted {
    QString summary, body;
    uint replacesId = 0, id = 0;
    QVariantMap hints;
};

// lipstick's org.freedesktop.Notifications, enough for btsyncd
class FakeLipstick : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.Notifications")
public:
    QMutex lock;
    QList<Posted> posted;
    QList<uint> closed;
    // what lipstick shows now, by id
    QMap<uint, Listed> shown;
    uint nextId = 100;

public slots:
    uint Notify(const QString &appName, uint replacesId, const QString &appIcon, const QString &summary,
                const QString &body, const QStringList &actions, const QVariantMap &hints, int timeout)
    {
        QMutexLocker l(&lock);
        if(replacesId && !shown.contains(replacesId))
            return 0;
        const uint id = replacesId ? replacesId : nextId++;
        Posted p;
        p.summary = summary;
        p.body = body;
        p.replacesId = replacesId;
        p.id = id;
        p.hints = hints;
        posted << p;
        Listed n;
        n.appName = appName;
        n.id = id;
        n.appIcon = appIcon;
        n.summary = summary;
        n.body = body;
        n.actions = actions;
        n.hints = hints;
        n.timeout = timeout;
        shown.insert(id, n);
        return id;
    }
    void CloseNotification(uint id)
    {
        QMutexLocker l(&lock);
        closed << id;
        shown.remove(id);
    }
    QList<Listed> GetNotifications(const QString &)
    {
        QMutexLocker l(&lock);
        return shown.values();
    }
};

class Collector : public QObject
{
    Q_OBJECT
public:
    QByteArray feedback;
    QList<QPair<QString, bool>> pictureChanges;
public slots:
    void propertiesChanged(const QString &, const QVariantMap &changed, const QStringList &)
    {
        if(changed.contains("Value"))
            feedback += changed.value("Value").toByteArray();
    }
    void pictureChanged(const QString &path, bool available) { pictureChanges << qMakePair(path, available); }
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

static QDBusMessage gattCall(QDBusConnection &driver, const char *path, const char *method, const QList<QVariant> &args)
{
    QDBusMessage m = QDBusMessage::createMethodCall(btsyncdName, path, GATT_CHRC_IFACE, method);
    m.setArguments(args);
    return driver.call(m, QDBus::BlockWithGui, 5000);
}

static void writeTo(QDBusConnection &driver, const char *path, const QByteArray &value, int offset = 0)
{
    QVariantMap options;
    options.insert("mtu", 512);
    if(offset)
        options.insert("offset", (quint16) offset);
    const QDBusMessage r = gattCall(driver, path, "WriteValue", {value, options});
    CHECK(r.type() == QDBusMessage::ReplyMessage);
}

// What the phone writes, cut in writes of 500 bytes (Android's longest is 512)
static void writeChunks(QDBusConnection &driver, const char *path, const QByteArray &xml, int upTo = -1)
{
    const int end = upTo < 0 ? xml.size() : upTo;
    for(int pos = 0; pos < end; pos += 500)
        writeTo(driver, path, xml.mid(pos, qMin(500, end - pos)));
}

static Posted lastPosted(FakeLipstick &fake)
{
    QMutexLocker l(&fake.lock);
    return fake.posted.isEmpty() ? Posted() : fake.posted.last();
}

static int postedCount(FakeLipstick &fake)
{
    QMutexLocker l(&fake.lock);
    return fake.posted.size();
}

// A JPEG for the watch's checks (its markers), of about this many bytes
static QByteArray jpeg(int bytes, quint32 seed)
{
    QByteArray j("\xff\xd8\xff\xe0", 4);
    QRandomGenerator random(seed);
    while(j.size() < bytes - 2)
        j += char(random.bounded(256));
    j += QByteArray("\xff\xd9", 2);
    return j;
}

static QString hashOf(const QByteArray &data)
{
    return QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex().left(16));
}

static QByteArray picMessage(int phoneId, const QString &id, const QByteArray &data, const QString &hash = QString(), int size = -1)
{
    return "<pic><id>" + QByteArray::number(phoneId) + "</id><pc>" + id.toLatin1() + "</pc><h>"
            + (hash.isEmpty() ? hashOf(data) : hash).toLatin1() + "</h><sz>" + QByteArray::number(size < 0 ? data.size() : size)
            + "</sz><d>" + data.toBase64() + "</d></pic>";
}

static QByteArray insert(int phoneId, const QString &body, const QString &picture = QString(), const QByteArray &extra = QByteArray())
{
    QByteArray x = "<insert><id>" + QByteArray::number(phoneId) + "</id><vb>none</vb><pn>org.example.chat</pn><an>Chat</an>"
            "<su>Robin</su><bo>" + body.toUtf8() + "</bo><ac>reply,open,mute</ac>";
    if(!picture.isEmpty())
        x += "<pc><i>" + picture.toLatin1() + "</i><w>384</w><h>288</h></pc>";
    return x + extra + "</insert>";
}

static QString feedbackFor(int phoneId, const QString &id)
{
    return QString("<action><id>%1</id><ty>picture</ty><tx>%2</tx></action>").arg(phoneId).arg(id);
}

static int pictureState(QDBusConnection &driver, const QString &path)
{
    QDBusMessage m = QDBusMessage::createMethodCall(SERVICE_NAME, BTSYNCD_NOTIF_PATH, BTSYNCD_NOTIF_IFACE, "PictureState");
    m << path;
    const QDBusMessage r = driver.call(m, QDBus::BlockWithGui, 5000);
    return r.type() == QDBusMessage::ReplyMessage ? r.arguments().value(0).toInt() : -1;
}

static QByteArray fileContent(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

static void emitClosed(QDBusConnection &fakeConn, FakeLipstick &fake, uint id, uint reason)
{
    {
        QMutexLocker l(&fake.lock);
        fake.shown.remove(id);
    }
    QDBusMessage s = QDBusMessage::createSignal("/org/freedesktop/Notifications", "org.freedesktop.Notifications", "NotificationClosed");
    s << id << reason;
    fakeConn.send(s);
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if(qEnvironmentVariableIsEmpty("DBUS_SESSION_BUS_ADDRESS") || qEnvironmentVariableIsEmpty("DBUS_SYSTEM_BUS_ADDRESS")) {
        fprintf(stderr, "needs a private bus as session and system bus (see the top of the source)\n");
        return 2;
    }
    qDBusRegisterMetaType<Listed>();
    qDBusRegisterMetaType<QList<Listed>>();

    // A cache with files of an earlier run: none of them a card's
    const QString dir = NotificationService::pictureDir();
    CHECK(dir.startsWith(QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation)));
    QDir(dir).removeRecursively();
    QDir().mkpath(dir);
    {
        QFile orphan(dir + "/00000000000000aa.jpg");
        CHECK(orphan.open(QIODevice::WriteOnly) && orphan.write(jpeg(500, 1)) > 0);
        QFile junk(dir + "/leftover.jpg.Xq1234");
        CHECK(junk.open(QIODevice::WriteOnly) && junk.write("x") == 1);
    }

    QThread fakeThread;
    fakeThread.start();
    FakeLipstick fake;
    fake.moveToThread(&fakeThread);
    QDBusConnection fakeConn = QDBusConnection::connectToBus(QDBusConnection::SessionBus, "fake-lipstick");
    CHECK(fakeConn.registerObject("/org/freedesktop/Notifications", &fake, QDBusConnection::ExportAllSlots));
    CHECK(fakeConn.registerService("org.freedesktop.Notifications"));

    NotificationService service(0, QDBusConnection::systemBus());
    btsyncdName = QDBusConnection::systemBus().baseService();
    service.setSyncGraceMs(0);

    QDBusConnection driver = QDBusConnection::connectToBus(QDBusConnection::SessionBus, "driver");
    Collector collector;
    CHECK(driver.connect(btsyncdName, CHR_FEEDBACK, "org.freedesktop.DBus.Properties", "PropertiesChanged",
                         &collector, SLOT(propertiesChanged(QString, QVariantMap, QStringList))));
    CHECK(driver.connect(SERVICE_NAME, BTSYNCD_NOTIF_PATH, BTSYNCD_NOTIF_IFACE, "PictureChanged",
                         &collector, SLOT(pictureChanged(QString, bool))));
    spin(200);

    // lipstick's (empty) list was read at the start: the files of no card go
    CHECK(waitFor([&] { return !QFile::exists(dir + "/00000000000000aa.jpg") && !QFile::exists(dir + "/leftover.jpg.Xq1234"); },
                  NotificationService::PicturePruneDelayMs + 2000));
    service.setPicturePruneDelayMs(200);

    // Capabilities
    {
        const QDBusMessage r = gattCall(driver, CHR_CAPS, "ReadValue", {QVariantMap()});
        CHECK(r.type() == QDBusMessage::ReplyMessage);
        const QByteArray caps = r.arguments().value(0).toByteArray();
        CHECK(caps.contains("<version>4</version>"));
        CHECK(caps.contains("<pictures>384</pictures>"));
        CHECK(caps.contains("<picturebytes>24576</picturebytes>"));
        fprintf(stderr, "capabilities: %s\n", caps.constData());
    }

    // An insert without a picture (an older phone): no hints, no request
    CHECK(gattCall(driver, CHR_FEEDBACK, "StartNotify", {}).type() == QDBusMessage::ReplyMessage);
    spin(100);
    collector.feedback.clear();
    writeTo(driver, CHR_UPDATE, insert(59, "Hello"));
    CHECK(waitFor([&] { return postedCount(fake) == 1; }));
    CHECK(!lastPosted(fake).hints.contains(NotificationService::PictureHint));
    CHECK(!lastPosted(fake).hints.contains(NotificationService::PictureSizeHint));
    spin(200);
    CHECK(collector.feedback.isEmpty());

    // A message with a picture: the hints, and the watch asks for it
    const QString idA = "0123456789abcdef";
    const QString pathA = NotificationService::picturePath(idA);
    CHECK(pathA == dir + "/" + idA + ".jpg");
    collector.feedback.clear();
    writeTo(driver, CHR_UPDATE, insert(60, "Photo", idA));
    CHECK(waitFor([&] { return postedCount(fake) == 2; }));
    const uint cardA = lastPosted(fake).id;
    CHECK(lastPosted(fake).hints.value(NotificationService::PictureHint).toString() == pathA);
    CHECK(lastPosted(fake).hints.value(NotificationService::PictureSizeHint).toString() == "384x288");
    CHECK(waitFor([&] { return collector.feedback.endsWith("</action>"); }));
    CHECK(QString::fromLatin1(collector.feedback) == feedbackFor(60, idA));
    fprintf(stderr, "request: %s\n", collector.feedback.constData());
    CHECK(pictureState(driver, pathA) == NotificationService::PicturePending);

    // An update of the card with the same picture while it is on its way: not asked again
    collector.feedback.clear();
    writeTo(driver, CHR_UPDATE, insert(60, "Photo", idA));
    CHECK(waitFor([&] { return postedCount(fake) == 3; }));
    spin(200);
    CHECK(collector.feedback.isEmpty());

    // The phone disconnects in the middle of the picture: the part is dropped
    const QByteArray picA = jpeg(16000, 2);
    const QByteArray msgA = picMessage(60, idA, picA);
    CHECK(msgA.size() > 20000);
    collector.pictureChanges.clear();
    writeChunks(driver, CHR_PICTURE, msgA, 10000);
    CHECK(gattCall(driver, CHR_FEEDBACK, "StopNotify", {}).type() == QDBusMessage::ReplyMessage);
    CHECK(waitFor([&] { return !collector.pictureChanges.isEmpty(); }));
    CHECK(collector.pictureChanges.value(0) == qMakePair(pathA, false));
    CHECK(!QFile::exists(pathA));
    CHECK(pictureState(driver, pathA) == NotificationService::PictureUnavailable);

    // Back: the phone's resync names the picture again, the watch asks again at once
    CHECK(gattCall(driver, CHR_FEEDBACK, "StartNotify", {}).type() == QDBusMessage::ReplyMessage);
    spin(100);
    collector.feedback.clear();
    writeTo(driver, CHR_UPDATE, insert(60, "Photo", idA, "<rs>1</rs>"));
    CHECK(waitFor([&] { return collector.feedback.endsWith("</action>"); }));
    CHECK(QString::fromLatin1(collector.feedback) == feedbackFor(60, idA));
    // the rest of the lost transfer does not make a picture with the new one
    writeTo(driver, CHR_PICTURE, msgA.mid(10000, 500));
    collector.pictureChanges.clear();
    writeChunks(driver, CHR_PICTURE, msgA);
    CHECK(waitFor([&] { return !collector.pictureChanges.isEmpty(); }));
    CHECK(collector.pictureChanges.value(0) == qMakePair(pathA, true));
    CHECK(fileContent(pathA) == picA);
    CHECK(pictureState(driver, pathA) == NotificationService::PictureReady);

    // The same picture in an update: nothing asked
    collector.feedback.clear();
    writeTo(driver, CHR_UPDATE, insert(60, "Photo!", idA));
    spin(300);
    CHECK(collector.feedback.isEmpty());
    CHECK(lastPosted(fake).hints.value(NotificationService::PictureHint).toString() == pathA);

    // A long write (BlueZ: chunks with offsets) of another card's picture
    const QString idB = "00000000000000b1";
    const QString pathB = NotificationService::picturePath(idB);
    collector.feedback.clear();
    writeTo(driver, CHR_UPDATE, insert(61, "Look", idB));
    CHECK(waitFor([&] { return collector.feedback.endsWith("</action>"); }));
    const QByteArray picB = jpeg(3000, 3);
    const QByteArray msgB = picMessage(61, idB, picB);
    collector.pictureChanges.clear();
    for(int pos = 0; pos < msgB.size(); pos += 509)
        writeTo(driver, CHR_PICTURE, msgB.mid(pos, 509), pos);
    CHECK(waitFor([&] { return !collector.pictureChanges.isEmpty(); }));
    CHECK(collector.pictureChanges.value(0) == qMakePair(pathB, true));
    CHECK(fileContent(pathB) == picB);

    // Broken pictures: a wrong hash is asked for once more, a wrong size then gives up
    const QString idC = "00000000000000c1";
    const QString pathC = NotificationService::picturePath(idC);
    collector.feedback.clear();
    writeTo(driver, CHR_UPDATE, insert(62, "Pic", idC));
    CHECK(waitFor([&] { return collector.feedback.endsWith("</action>"); }));
    const QByteArray picC = jpeg(2000, 4);
    collector.feedback.clear();
    collector.pictureChanges.clear();
    writeChunks(driver, CHR_PICTURE, picMessage(62, idC, picC, "ffffffffffffffff"));
    CHECK(waitFor([&] { return collector.feedback.endsWith("</action>"); }));
    CHECK(QString::fromLatin1(collector.feedback) == feedbackFor(62, idC));
    CHECK(!QFile::exists(pathC));
    writeChunks(driver, CHR_PICTURE, picMessage(62, idC, picC, QString(), picC.size() + 1));
    CHECK(waitFor([&] { return !collector.pictureChanges.isEmpty(); }));
    CHECK(collector.pictureChanges.value(0) == qMakePair(pathC, false));
    CHECK(!QFile::exists(pathC));
    CHECK(pictureState(driver, pathC) == NotificationService::PictureUnavailable);

    // Not a JPEG; and the phone cannot send one
    const QString idD = "00000000000000d1";
    collector.feedback.clear();
    writeTo(driver, CHR_UPDATE, insert(63, "Pic", idD));
    CHECK(waitFor([&] { return collector.feedback.endsWith("</action>"); }));
    collector.feedback.clear();
    writeChunks(driver, CHR_PICTURE, picMessage(63, idD, QByteArray("\x89PNG\r\n\x1a\n") + jpeg(500, 5)));
    CHECK(waitFor([&] { return collector.feedback.endsWith("</action>"); }));
    collector.pictureChanges.clear();
    writeTo(driver, CHR_PICTURE, "<pic><id>63</id><pc>" + idD.toLatin1() + "</pc></pic>");
    CHECK(waitFor([&] { return !collector.pictureChanges.isEmpty(); }));
    CHECK(collector.pictureChanges.value(0) == qMakePair(NotificationService::picturePath(idD), false));
    CHECK(!QFile::exists(NotificationService::picturePath(idD)));

    // An update with another picture: asked for, and the old one goes
    const QString idE = "00000000000000e1";
    collector.feedback.clear();
    writeTo(driver, CHR_UPDATE, insert(61, "Another", idE));
    CHECK(waitFor([&] { return collector.feedback.endsWith("</action>"); }));
    CHECK(QString::fromLatin1(collector.feedback) == feedbackFor(61, idE));
    CHECK(waitFor([&] { return !QFile::exists(pathB); }));
    CHECK(QFile::exists(pathA));

    // A conversation posted anew (new content) keeps its picture
    const QString idF = "00000000000000f1";
    const QString pathF = NotificationService::picturePath(idF);
    writeTo(driver, CHR_UPDATE, insert(70, "First", idF, "<th>0badc0de</th><pr>4</pr>"));
    CHECK(waitFor([&] { return collector.feedback.endsWith(feedbackFor(70, idF).toLatin1()); }));
    writeChunks(driver, CHR_PICTURE, picMessage(70, idF, jpeg(1500, 6)));
    CHECK(waitFor([&] { return QFile::exists(pathF); }));
    int before = postedCount(fake);
    writeTo(driver, CHR_UPDATE, insert(71, "Second", idF, "<th>0badc0de</th><pr>4</pr>"));
    CHECK(waitFor([&] { return postedCount(fake) == before + 1; }));
    const uint cardF = lastPosted(fake).id;
    spin(600);
    CHECK(QFile::exists(pathF));
    CHECK(lastPosted(fake).hints.value(NotificationService::PictureHint).toString() == pathF);

    // The phone removes a card: its picture goes
    writeTo(driver, CHR_UPDATE, "<removed><id>60</id></removed>");
    CHECK(waitFor([&] { QMutexLocker l(&fake.lock); return fake.closed.contains(cardA); }));
    CHECK(waitFor([&] { return !QFile::exists(pathA); }));
    // and a picture of a card that went is not stored
    writeChunks(driver, CHR_PICTURE, picMessage(60, idA, picA));
    spin(400);
    CHECK(!QFile::exists(pathA));

    // The user dismisses a card (or lipstick expires it): its picture goes
    emitClosed(fakeConn, fake, cardF, 2);
    CHECK(waitFor([&] { return !QFile::exists(pathF); }));
    const QString idG = "0000000000000a01";
    const QString pathG = NotificationService::picturePath(idG);
    writeTo(driver, CHR_UPDATE, insert(72, "Expiring", idG));
    CHECK(waitFor([&] { return collector.feedback.endsWith(feedbackFor(72, idG).toLatin1()); }));
    const uint cardG = lastPosted(fake).id;
    writeChunks(driver, CHR_PICTURE, picMessage(72, idG, jpeg(1500, 7)));
    CHECK(waitFor([&] { return QFile::exists(pathG); }));
    emitClosed(fakeConn, fake, cardG, 1);
    CHECK(waitFor([&] { return !QFile::exists(pathG); }));

    // After a restart (lipstick's list read again): a card closed behind btsyncd's back
    // loses its picture, the others keep theirs
    const QString idH = "0000000000000b01", idI = "0000000000000b02";
    writeTo(driver, CHR_UPDATE, insert(80, "One", idH));
    CHECK(waitFor([&] { return collector.feedback.endsWith(feedbackFor(80, idH).toLatin1()); }));
    const uint cardH = lastPosted(fake).id;
    writeChunks(driver, CHR_PICTURE, picMessage(80, idH, jpeg(1200, 8)));
    writeTo(driver, CHR_UPDATE, insert(81, "Two", idI));
    CHECK(waitFor([&] { return collector.feedback.endsWith(feedbackFor(81, idI).toLatin1()); }));
    writeChunks(driver, CHR_PICTURE, picMessage(81, idI, jpeg(1200, 9)));
    CHECK(waitFor([&] { return QFile::exists(NotificationService::picturePath(idH)) && QFile::exists(NotificationService::picturePath(idI)); }));
    {
        QMutexLocker l(&fake.lock);
        fake.shown.remove(cardH);
    }
    service.restoreKnown();
    CHECK(waitFor([&] { return !QFile::exists(NotificationService::picturePath(idH)); }));
    CHECK(QFile::exists(NotificationService::picturePath(idI)));

    // The cache's limits: the oldest go, a card's picture then is not available
    service.setPictureCacheLimits(2, 1024 * 1024);
    QList<QString> ids;
    for(int i = 0; i < 3; i++) {
        const QString id = QString("0000000000000c0%1").arg(i);
        ids << id;
        writeTo(driver, CHR_UPDATE, insert(90 + i, "Many", id));
        CHECK(waitFor([&] { return collector.feedback.endsWith(feedbackFor(90 + i, id).toLatin1()); }));
        writeChunks(driver, CHR_PICTURE, picMessage(90 + i, id, jpeg(1000, 10 + i)));
        CHECK(waitFor([&] { return QFile::exists(NotificationService::picturePath(id)); }));
        spin(1100);     // the files' times differ
    }
    CHECK(waitFor([&] { return QDir(dir).entryList(QDir::Files).size() == 2; }));
    CHECK(QFile::exists(NotificationService::picturePath(ids.at(2))));
    CHECK(QFile::exists(NotificationService::picturePath(ids.at(1))));
    fprintf(stderr, "cache: %s\n", qPrintable(QDir(dir).entryList(QDir::Files).join(' ')));

    // Ids that are not ones: no hint, no path
    CHECK(NotificationService::picturePath("../../etc/passwd").isEmpty());
    CHECK(NotificationService::picturePath("0123456789ABCDEF").isEmpty());
    CHECK(pictureState(driver, "/etc/passwd") == NotificationService::PictureUnavailable);
    writeTo(driver, CHR_UPDATE, insert(99, "Bad", "", "<pc><i>../x</i><w>1</w><h>1</h></pc>"));
    CHECK(waitFor([&] { return lastPosted(fake).body == "Bad"; }));
    CHECK(!lastPosted(fake).hints.contains(NotificationService::PictureHint));

    fakeThread.quit();
    fakeThread.wait();
    if(failures)
        fprintf(stderr, "%d check(s) failed\n", failures);
    else
        fprintf(stderr, "pictures_test: all checks passed\n");
    return failures ? 1 : 0;
}

#include "pictures_test.moc"
