/*
 * Tests of the pictures of a conversation's messages (capability <msgpictures>, <pc> in an
 * <m>) on a private D-Bus (cmake -DBTSYNCD_TESTS=ON), run like notifications_test:
 *   dbus-run-session -- sh -c 'DBUS_SYSTEM_BUS_ADDRESS=$DBUS_SESSION_BUS_ADDRESS msgpictures_test'
 * The scaffolding (a fake lipstick with GetNotifications, the picture messages) is
 * pictures_test's. As stock Wear OS keeps a message's image with the message:
 *   - the hint x-asteroid-messages names each message's picture (p, w, h)
 *   - the newest message's picture is asked for at once, an older one when shown
 *     (PictureState), once a minute at most
 *   - the next message, text only (the field report: a WhatsApp picture gone 38 s later):
 *     the card has no picture of its own any more, the message's is kept, not asked again
 *   - a message with a picture and no text is kept; a bad id is no picture
 *   - the picture goes when its message leaves the conversation, or the card goes
 *   - an older phone (no <pc> in <m>): nothing new
 * Names are made up.
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
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
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

// A conversation of the thread 00000000000000c1 from Example Chat, its messages, and maybe its
// own picture
static QByteArray conv(int phoneId, const QByteArray &messages, const QString &cardPicture = QString())
{
    QByteArray x = "<insert><id>" + QByteArray::number(phoneId) + "</id><vb>none</vb><pn>com.example.chat</pn>"
            "<an>Example Chat</an><su>Robin Example</su><ac>reply,open,mute</ac><th>00000000000000c1</th>";
    if(!cardPicture.isEmpty())
        x += "<pc><i>" + cardPicture.toLatin1() + "</i><w>384</w><h>288</h></pc>";
    return x + "<ms>" + messages + "</ms></insert>";
}

static QByteArray msg(const char *sender, const char *text, const QString &picture = QString(), bool me = false)
{
    QByteArray m = QByteArray("<m><s>") + sender + "</s><t>" + text + "</t>";
    if(me)
        m += "<me>1</me>";
    if(!picture.isEmpty())
        m += "<pc><i>" + picture.toLatin1() + "</i><w>384</w><h>288</h></pc>";
    return m + "</m>";
}

static QJsonArray messagesOf(const Posted &p)
{
    return QJsonDocument::fromJson(p.hints.value(NotificationService::MessagesHint).toString().toUtf8()).array();
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

    // Capabilities: each message's own picture
    {
        const QDBusMessage r = gattCall(driver, CHR_CAPS, "ReadValue", {QVariantMap()});
        CHECK(r.type() == QDBusMessage::ReplyMessage);
        CHECK(r.arguments().value(0).toByteArray().contains("<msgpictures>1</msgpictures>"));
    }
    CHECK(gattCall(driver, CHR_FEEDBACK, "StartNotify", {}).type() == QDBusMessage::ReplyMessage);
    spin(100);

    // A picture message (the 19:27:40 update of the field report): the card's picture and the
    // message's are the same; the hint names it on the message; asked for once
    const QString idM1 = "1111111111111111";
    const QString pathM1 = NotificationService::picturePath(idM1);
    collector.feedback.clear();
    writeTo(driver, CHR_UPDATE, conv(70, msg("Robin Example", "Photo", idM1), idM1));
    CHECK(waitFor([&] { return postedCount(fake) == 1; }));
    {
        const QJsonArray list = messagesOf(lastPosted(fake));
        CHECK(list.size() == 1);
        CHECK(list.at(0).toObject().value("p").toString() == pathM1);
        CHECK(list.at(0).toObject().value("w").toInt() == 384 && list.at(0).toObject().value("h").toInt() == 288);
        CHECK(lastPosted(fake).hints.value(NotificationService::PictureHint).toString() == pathM1);
    }
    CHECK(waitFor([&] { return collector.feedback.endsWith("</action>"); }));
    CHECK(QString::fromLatin1(collector.feedback) == feedbackFor(70, idM1));
    writeChunks(driver, CHR_PICTURE, picMessage(70, idM1, jpeg(9000, 7)));
    CHECK(waitFor([&] { return QFile::exists(pathM1); }));

    // The next message of the chat, text only (19:28:18): the card has no picture of its own
    // any more, the message keeps it: not deleted, not asked for again
    collector.feedback.clear();
    writeTo(driver, CHR_UPDATE, conv(71, msg("Robin Example", "Photo", idM1) + msg("Robin Example", "Nice, isn't it?")));
    CHECK(waitFor([&] { return postedCount(fake) == 2; }));
    {
        const QJsonArray list = messagesOf(lastPosted(fake));
        CHECK(list.size() == 2);
        CHECK(list.at(0).toObject().value("p").toString() == pathM1);
        CHECK(!list.at(1).toObject().contains("p"));
        CHECK(!lastPosted(fake).hints.contains(NotificationService::PictureHint));
    }
    spin(1200);     // past the prune delay
    CHECK(QFile::exists(pathM1));
    CHECK(collector.feedback.isEmpty());
    CHECK(pictureState(driver, pathM1) == NotificationService::PictureReady);

    // Older messages' pictures: asked for when shown (PictureState), the newest at once; a
    // message with a picture and no text is kept
    const QString idM2 = "2222222222222222", idM3 = "3333333333333333";
    const QString pathM2 = NotificationService::picturePath(idM2), pathM3 = NotificationService::picturePath(idM3);
    collector.feedback.clear();
    writeTo(driver, CHR_UPDATE, conv(72, msg("Robin Example", "Photo", idM1) + msg("Robin Example", "", idM2)
                                         + msg("You", "Mine", idM3, true) + msg("Robin Example", "Great")));
    CHECK(waitFor([&] { return postedCount(fake) == 3; }));
    {
        const QJsonArray list = messagesOf(lastPosted(fake));
        CHECK(list.size() == 4);
        CHECK(list.at(1).toObject().value("t").toString().isEmpty());
        CHECK(list.at(1).toObject().value("p").toString() == pathM2);
        CHECK(list.at(2).toObject().value("p").toString() == pathM3);
        CHECK(list.at(2).toObject().value("me").toBool());
    }
    CHECK(waitFor([&] { return collector.feedback.endsWith("</action>"); }));
    spin(300);
    CHECK(QString::fromLatin1(collector.feedback) == feedbackFor(72, idM3));
    collector.feedback.clear();
    CHECK(pictureState(driver, pathM2) == NotificationService::PicturePending);
    CHECK(waitFor([&] { return collector.feedback.endsWith("</action>"); }));
    CHECK(QString::fromLatin1(collector.feedback) == feedbackFor(72, idM2));
    // asked once: shown again within the minute, not asked again
    collector.feedback.clear();
    CHECK(pictureState(driver, pathM2) == NotificationService::PicturePending);
    spin(300);
    CHECK(collector.feedback.isEmpty());
    writeChunks(driver, CHR_PICTURE, picMessage(72, idM2, jpeg(7000, 8)));
    writeChunks(driver, CHR_PICTURE, picMessage(72, idM3, jpeg(6000, 9)));
    CHECK(waitFor([&] { return QFile::exists(pathM2) && QFile::exists(pathM3); }));

    // The picture message leaves the conversation (the phone sends only its newest): deleted;
    // the others stay
    writeTo(driver, CHR_UPDATE, conv(73, msg("Robin Example", "", idM2) + msg("You", "Mine", idM3, true)
                                         + msg("Robin Example", "Great") + msg("Robin Example", "Later?")));
    CHECK(waitFor([&] { return postedCount(fake) == 4; }));
    CHECK(waitFor([&] { return !QFile::exists(pathM1); }));
    CHECK(QFile::exists(pathM2) && QFile::exists(pathM3));

    // The card goes (removed on the phone): its messages' pictures go too
    writeTo(driver, CHR_UPDATE, "<removed><id>73</id></removed>");
    CHECK(waitFor([&] { return !QFile::exists(pathM2) && !QFile::exists(pathM3); }, NotificationService::CloseDelayMs + 3000));

    // An older phone (no <pc> in a message): no picture on the message, nothing asked
    collector.feedback.clear();
    writeTo(driver, CHR_UPDATE, "<insert><id>80</id><vb>none</vb><an>Example Chat</an><su>Kim Example</su><ac>reply,mute</ac>"
                                "<th>00000000000000c2</th><ms><m><s>Kim Example</s><t>Hi</t></m></ms></insert>");
    CHECK(waitFor([&] { return postedCount(fake) == 5; }));
    CHECK(!messagesOf(lastPosted(fake)).at(0).toObject().contains("p"));
    spin(300);
    CHECK(collector.feedback.isEmpty());

    // A bad id on a message is no picture; a message without text or picture is dropped
    writeTo(driver, CHR_UPDATE, conv(81, msg("Kim Example", "Look", "../../etc/passwd") + msg("Kim Example", "")));
    CHECK(waitFor([&] { return postedCount(fake) == 6; }));
    CHECK(messagesOf(lastPosted(fake)).size() == 1);
    CHECK(!messagesOf(lastPosted(fake)).at(0).toObject().contains("p"));

    fakeThread.quit();
    fakeThread.wait();
    if(failures)
        fprintf(stderr, "%d failure(s)\n", failures);
    else
        fprintf(stderr, "msgpictures_test: all checks passed\n");
    return failures ? 1 : 0;
}

#include "msgpictures_test.moc"
