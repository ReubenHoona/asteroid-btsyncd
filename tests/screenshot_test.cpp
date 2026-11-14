/*
 * Copyright (C) 2026 eos-mainline contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Tests of the screenshot hand-off of the wearui-w2 wave (W2-btsyncd-notif) on a private bus
 * (cmake -DBTSYNCD_TESTS=ON), run like notifications_test:
 *   dbus-run-session -- sh -c 'DBUS_SYSTEM_BUS_ADDRESS=$DBUS_SESSION_BUS_ADDRESS screenshot_test'
 * org.asteroidos.btsyncd.Screenshot.Push(path) on the session bus sends a screenshot the watch took itself
 * on the screenshot content characteristic, the way a requested one goes (4 bytes of size, little
 * endian, then the file in pieces of 20 bytes), only to a phone that listens, and only a small JPEG or PNG.
 */

#include "screenshotservice.h"
#include "common.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QVariantMap>
#include <cstdio>
#include <functional>

static int failures = 0;

#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static const char *CHR_CONTENT = "/org/asteroidos/btsyncd/service4/char1";

class Collector : public QObject
{
    Q_OBJECT
public:
    QList<QByteArray> pieces;
public slots:
    void propertiesChanged(const QString &, const QVariantMap &changed, const QStringList &)
    {
        if(changed.contains("Value"))
            pieces << changed.value("Value").toByteArray();
    }
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

static QString writeFile(const QString &name, const QByteArray &content)
{
    const QString path = QDir::tempPath() + "/btsyncd-screenshot-test-" + name;
    QFile f(path);
    CHECK(f.open(QIODevice::WriteOnly));
    f.write(content);
    return path;
}

static QByteArray fill(const QByteArray &head, int size)
{
    QByteArray b = head;
    for(int i = head.size(); i < size; i++)
        b += char(i * 13);
    return b;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if(qEnvironmentVariableIsEmpty("DBUS_SESSION_BUS_ADDRESS") || qEnvironmentVariableIsEmpty("DBUS_SYSTEM_BUS_ADDRESS")) {
        fprintf(stderr, "needs a private bus as session and system bus (see the top of the source)\n");
        return 2;
    }

    ScreenshotService service(4, QDBusConnection::systemBus());
    const QString gatt = QDBusConnection::systemBus().baseService();
    const QString session = QDBusConnection::sessionBus().baseService();
    QDBusConnection driver = QDBusConnection::connectToBus(QDBusConnection::SessionBus, "driver");
    Collector collector;
    CHECK(driver.connect(gatt, CHR_CONTENT, "org.freedesktop.DBus.Properties", "PropertiesChanged",
                         &collector, SLOT(propertiesChanged(QString, QVariantMap, QStringList))));
    spin(200);

    const auto push = [&](const QString &path) {
        QDBusMessage m = QDBusMessage::createMethodCall(session, BTSYNCD_SCREENSHOT_PATH, BTSYNCD_SCREENSHOT_IFACE, "Push");
        m << path;
        const QDBusMessage r = driver.call(m, QDBus::BlockWithGui, 5000);
        CHECK(r.type() == QDBusMessage::ReplyMessage);
        return r.arguments().value(0).toBool();
    };
    const auto received = [&]() {
        QByteArray all;
        for(int i = 1; i < collector.pieces.size(); i++)
            all += collector.pieces.at(i);
        return all;
    };

    const QByteArray jpeg = fill(QByteArray("\xff\xd8\xff\xe0", 4), 1234);
    const QByteArray png = fill(QByteArray("\x89PNG\r\n\x1a\n", 8), 20000);
    const QString jpegPath = writeFile("a.jpg", jpeg);
    const QString pngPath = writeFile("b.png", png);

    // nobody listens: nothing is sent
    CHECK(push(jpegPath) == false);
    spin(100);
    CHECK(collector.pieces.isEmpty());

    // the phone enables notifications (Gadgetbridge does at its connection)
    {
        QDBusMessage m = QDBusMessage::createMethodCall(gatt, CHR_CONTENT, GATT_CHRC_IFACE, "StartNotify");
        CHECK(driver.call(m, QDBus::BlockWithGui, 5000).type() == QDBusMessage::ReplyMessage);
    }
    CHECK(push(jpegPath) == true);
    CHECK(waitFor([&] { return received().size() >= jpeg.size(); }));
    CHECK(collector.pieces.value(0) == QByteArray("\xd2\x04\x00\x00", 4));          // 1234, little endian
    CHECK(received() == jpeg);
    for(int i = 1; i < collector.pieces.size(); i++)
        CHECK(collector.pieces.at(i).size() <= 20);

    // a PNG the same way
    collector.pieces.clear();
    CHECK(push(pngPath) == true);
    CHECK(waitFor([&] { return received().size() >= png.size(); }, 8000));
    CHECK(collector.pieces.value(0) == QByteArray("\x20\x4e\x00\x00", 4));          // 20000
    CHECK(received() == png);

    // what is not a screenshot to send: refused, nothing sent
    collector.pieces.clear();
    CHECK(push(writeFile("c.txt", "secret = hunter2\n")) == false);
    CHECK(push(writeFile("d.jpg", fill(QByteArray("\xff\xd8\xff\xe0", 4), 1) + QByteArray())) == true);   // tiny, but a JPEG
    spin(300);
    collector.pieces.clear();
    CHECK(push(writeFile("e.png", QByteArray())) == false);                                              // empty
    CHECK(push(writeFile("f.jpg", fill(QByteArray("\xff\xd8\xff", 3), int(ScreenshotContentChrc::MaxPushBytes) + 1))) == false);   // too large
    CHECK(push(QDir::tempPath() + "/btsyncd-screenshot-test-missing.png") == false);
    CHECK(push(QDir::tempPath()) == false);                                                              // a directory
    CHECK(push(QString()) == false);
    spin(200);
    CHECK(collector.pieces.isEmpty());
    CHECK(QString(ScreenshotContentChrc::notAnImage(writeFile("g.bin", "not an image at all"))) == "not a JPEG or PNG");
    CHECK(ScreenshotContentChrc::notAnImage(jpegPath) == nullptr && ScreenshotContentChrc::notAnImage(pngPath) == nullptr);

    // BlueZ keeps a subscription over a disconnection: the application says whether the phone is there
    bool there = false;
    service.content()->setPhoneCheck([&there] { return there; });
    CHECK(push(jpegPath) == false);
    there = true;
    collector.pieces.clear();
    CHECK(push(jpegPath) == true);
    CHECK(waitFor([&] { return received().size() >= jpeg.size(); }));

    // the phone stops listening
    {
        QDBusMessage m = QDBusMessage::createMethodCall(gatt, CHR_CONTENT, GATT_CHRC_IFACE, "StopNotify");
        CHECK(driver.call(m, QDBus::BlockWithGui, 5000).type() == QDBusMessage::ReplyMessage);
    }
    CHECK(push(jpegPath) == false);

    for(const char *n : {"a.jpg", "b.png", "c.txt", "d.jpg", "e.png", "f.jpg", "g.bin"})
        QFile::remove(QDir::tempPath() + "/btsyncd-screenshot-test-" + n);

    if(failures)
        fprintf(stderr, "%d check(s) failed\n", failures);
    else
        fprintf(stderr, "screenshot_test: all checks passed\n");
    return failures ? 1 : 0;
}

#include "screenshot_test.moc"
