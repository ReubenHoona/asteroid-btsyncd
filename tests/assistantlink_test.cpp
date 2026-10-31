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

// The assistant relay follows the phone's BlueZ link (a bonded phone out of
// range keeps its subscriptions), on a private bus like notifications_test:
// dbus-run-session -- sh -c 'DBUS_SYSTEM_BUS_ADDRESS=$DBUS_SESSION_BUS_ADDRESS assistantlink_test'

#include "assistantservice.h"
#include "contactnames.h"
#include "notificationservice.h"
#include "common.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QTemporaryDir>
#include <sys/stat.h>
#include <unistd.h>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QElapsedTimer>
#include <QVariantMap>
#include <cstdio>
#include <functional>

static int failures = 0;

#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static const char *CHR_FEEDBACK = "/org/asteroidos/btsyncd/service0/char1";
static const char *CHR_REQUEST = "/org/asteroidos/btsyncd/service10/char0";
static const char *CHR_REPLY = "/org/asteroidos/btsyncd/service10/char1";
static const char *CHR_CAPS = "/org/asteroidos/btsyncd/service10/char2";

class Collector : public QObject
{
    Q_OBJECT
public:
    QByteArray requests;
    QList<QPair<uint, QString>> failed;
    QList<QPair<uint, QString>> results;
public slots:
    void propertiesChanged(const QString &, const QVariantMap &changed, const QStringList &)
    {
        if (changed.contains("Value"))
            requests += changed.value("Value").toByteArray();
    }
    void onFailed(uint id, const QString &code) { failed << qMakePair(id, code); }
    void onActResult(uint id, const QString &json) { results << qMakePair(id, json); }
};

static bool waitFor(const std::function<bool()> &cond, int ms = 3000)
{
    QElapsedTimer t;
    t.start();
    while (!cond() && t.elapsed() < ms)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return cond();
}

static void spin(int ms)
{
    waitFor([] { return false; }, ms);
}

static QString btsyncdName;

static QDBusMessage gattCall(QDBusConnection &driver, const char *path, const char *method, const QList<QVariant> &args)
{
    QDBusMessage m = QDBusMessage::createMethodCall(btsyncdName, path, GATT_CHRC_IFACE, method);
    m.setArguments(args);
    return driver.call(m, QDBus::BlockWithGui, 5000);
}

static QVariant api(QDBusConnection &driver, const char *method, const QList<QVariant> &args = {})
{
    QDBusMessage m = QDBusMessage::createMethodCall(SERVICE_NAME, BTSYNCD_ASSISTANT_PATH,
                                                    "org.asteroidos.btsyncd.Assistant", method);
    m.setArguments(args);
    const QDBusMessage r = driver.call(m, QDBus::BlockWithGui, 5000);
    CHECK(r.type() == QDBusMessage::ReplyMessage);
    return r.arguments().value(0);
}

static void writeReply(QDBusConnection &driver, const QByteArray &xml)
{
    QVariantMap options;
    options.insert("mtu", 247);
    CHECK(gattCall(driver, CHR_REPLY, "WriteValue", {xml, options}).type() == QDBusMessage::ReplyMessage);
}

// An invented phone book: 25 first names x 40 surnames = 1000 people, and the first names
static QStringList inventedBook()
{
    const QStringList first = {"Sam", "Priya", "Lena", "Tomasz", "Hana", "Declan", "Aisha", "Mateo", "Freya", "Marcus",
                               "Iris", "Owen", "Nadia", "Ravi", "Elsa", "Kofi", "Yara", "Bruno", "Mei", "Tariq",
                               "Ines", "Joaquin", "Saoirse", "Anouk", "Bjorn"};
    const QStringList last = {"Carter", "Natarajan", "Okafor", "Wielicki", "Moriyama", "Rourke", "Rahman", "Alvarez",
                              "Lindqvist", "Bell", "Quillon", "Ashdown", "Brackley", "Fenwick", "Galloway", "Harrow",
                              "Ingram", "Jessop", "Kestrel", "Lowther", "Marlow", "Northcote", "Ormsby", "Penrose",
                              "Radley", "Sallow", "Thorne", "Underwood", "Varley", "Whitlock", "Yardley", "Zeller",
                              "Amberley", "Birchall", "Coldwell", "Dunmore", "Elstow", "Farrant", "Gisburn", "Hallam"};
    QStringList book;
    for (const QString &f : first)
        for (const QString &l : last)
            book << f + QLatin1Char(' ') + l;
    book += first;
    return book;
}

// The book as Gadgetbridge sends it: parts of at most about 3 kB of XML
static QList<QByteArray> namesParts(const QStringList &book, const QByteArray &generation)
{
    QList<QStringList> groups;
    QStringList cur;
    int bytes = 0;
    for (const QString &n : book) {
        const int b = n.toUtf8().size() + 7;
        if (bytes + b > 3000 && !cur.isEmpty()) {
            groups << cur;
            cur.clear();
            bytes = 0;
        }
        cur << n;
        bytes += b;
    }
    if (!cur.isEmpty())
        groups << cur;
    QList<QByteArray> out;
    for (int i = 0; i < groups.size(); i++) {
        QByteArray x = "<cn><g>" + generation + "</g><i>" + QByteArray::number(i) + "</i><n>" + QByteArray::number(groups.size()) + "</n>";
        for (const QString &n : groups.at(i))
            x += "<v>" + n.toUtf8().replace("&", "&amp;").replace("<", "&lt;") + "</v>";
        out << x + "</cn>";
    }
    return out;
}

// As the phone writes: pieces of at most 244 bytes (MTU 247), a piece may end inside a name
static void writeInPieces(QDBusConnection &driver, const QByteArray &xml)
{
    QVariantMap options;
    options.insert("mtu", 247);
    for (int pos = 0; pos < xml.size(); pos += 244)
        CHECK(gattCall(driver, CHR_REPLY, "WriteValue", {xml.mid(pos, 244), options}).type() == QDBusMessage::ReplyMessage);
}

static QByteArray readCaps(QDBusConnection &driver)
{
    const QDBusMessage r = gattCall(driver, CHR_CAPS, "ReadValue", {QVariantMap()});
    CHECK(r.type() == QDBusMessage::ReplyMessage);
    return r.arguments().value(0).toByteArray();
}

int main(int argc, char **argv)
{
    // the contact names go to a private directory (never ~/.local/share of whoever runs this)
    QTemporaryDir names;
    const QString namesFile = names.path() + QStringLiteral("/share/asteroid-assistant/stt-context.txt");
    qputenv("BTSYNCD_STT_CONTEXT", namesFile.toUtf8());
    QCoreApplication app(argc, argv);
    if (qEnvironmentVariableIsEmpty("DBUS_SESSION_BUS_ADDRESS") || qEnvironmentVariableIsEmpty("DBUS_SYSTEM_BUS_ADDRESS")) {
        fprintf(stderr, "needs a private bus as session and system bus (see the top of the source)\n");
        return 2;
    }

    NotificationService notifications(0, QDBusConnection::systemBus());
    AssistantService assistant(10, QDBusConnection::systemBus(), &notifications);
    btsyncdName = QDBusConnection::systemBus().baseService();

    QDBusConnection driver = QDBusConnection::connectToBus(QDBusConnection::SessionBus, "driver");
    Collector collector;
    CHECK(driver.connect(btsyncdName, CHR_REQUEST, "org.freedesktop.DBus.Properties", "PropertiesChanged",
                         &collector, SLOT(propertiesChanged(QString, QVariantMap, QStringList))));
    CHECK(driver.connect(SERVICE_NAME, BTSYNCD_ASSISTANT_PATH, "org.asteroidos.btsyncd.Assistant", "Failed",
                         &collector, SLOT(onFailed(uint, QString))));
    CHECK(driver.connect(SERVICE_NAME, BTSYNCD_ASSISTANT_PATH, "org.asteroidos.btsyncd.Assistant", "ActResult",
                         &collector, SLOT(onActResult(uint, QString))));
    spin(200);

    // The phone connects: link, feedback and relay subscriptions, "ready"
    notifications.setLinkConnected(true);
    CHECK(gattCall(driver, CHR_FEEDBACK, "StartNotify", {}).type() == QDBusMessage::ReplyMessage);
    CHECK(gattCall(driver, CHR_REQUEST, "StartNotify", {}).type() == QDBusMessage::ReplyMessage);
    CHECK(api(driver, "Available").toBool() == false);
    writeReply(driver, "<asr><st>ready</st></asr>");
    CHECK(api(driver, "Available").toBool() == true);

    // A question goes to the phone
    CHECK(api(driver, "Ask", {7u, QStringLiteral("{\"messages\":[]}")}).toBool() == true);
    CHECK(waitFor([&] { return collector.requests.endsWith("</ask>"); }));

    // The bonded phone goes out of range: BlueZ keeps the subscriptions (no
    // StopNotify), only the link says it. The question waiting fails at once
    // and the relay is not available: asteroid-assistant asks the server.
    notifications.setLinkConnected(false);
    CHECK(waitFor([&] { return !collector.failed.isEmpty(); }));
    CHECK(collector.failed.size() == 1 && collector.failed.value(0).first == 7u
          && collector.failed.value(0).second == QLatin1String("disconnected"));
    CHECK(api(driver, "Available").toBool() == false);
    collector.requests.clear();
    CHECK(api(driver, "Ask", {8u, QStringLiteral("{\"messages\":[]}")}).toBool() == false);
    spin(200);
    CHECK(collector.requests.isEmpty());

    // Back with the same subscriptions: available once the phone says so again
    notifications.setLinkConnected(true);
    spin(200);
    CHECK(api(driver, "Available").toBool() == false);
    writeReply(driver, "<asr><st>ready</st></asr>");
    CHECK(api(driver, "Available").toBool() == true);

    /* Actions on the phone (directions, contacts, text messages) */
    // an older Gadgetbridge (no <ac>): none, and Act() refuses
    CHECK(api(driver, "Actions").toStringList().isEmpty());
    CHECK(api(driver, "Act", {20u, QStringLiteral("{\"a\":\"nav\",\"q\":\"Riverbend Park\"}")}).toBool() == false);
    // Gadgetbridge 41 says what it does, also without an answer server
    writeReply(driver, "<asr><st>unavailable</st><er>no-url</er><ac>nav,contacts,sms</ac></asr>");
    CHECK(api(driver, "Available").toBool() == false);
    CHECK(api(driver, "Actions").toStringList() == QStringList({"nav", "contacts", "sms"}));
    collector.requests.clear();
    CHECK(api(driver, "Act", {21u, QStringLiteral("{\"a\":\"nav\",\"q\":\"Riverbend Park\",\"mode\":\"walking\"}")}).toBool() == true);
    CHECK(waitFor([&] { return collector.requests.endsWith("</act>"); }));
    CHECK(collector.requests.startsWith("<act><id>21</id><js>{&quot;a&quot;:&quot;nav&quot;")
          || collector.requests.startsWith("<act><id>21</id><js>{\"a\":\"nav\""));
    // an action the phone does not do, a second Act() with the same id, an empty one: refused
    CHECK(api(driver, "Act", {22u, QStringLiteral("{\"a\":\"call\",\"q\":\"Sam Carter\"}")}).toBool() == false);
    CHECK(api(driver, "Act", {21u, QStringLiteral("{\"a\":\"nav\",\"q\":\"Mount Alder\"}")}).toBool() == false);
    CHECK(api(driver, "Act", {23u, QStringLiteral("not json")}).toBool() == false);
    // the result, in two writes (a long contact list does not fit in one)
    QVariantMap options;
    options.insert("mtu", 23);
    CHECK(gattCall(driver, CHR_REPLY, "WriteValue", {QByteArray("<acr><id>21</id><js>{\"ok\":tr"), options}).type() == QDBusMessage::ReplyMessage);
    CHECK(gattCall(driver, CHR_REPLY, "WriteValue", {QByteArray("ue,\"how\":\"started\"}</js></acr>"), options}).type() == QDBusMessage::ReplyMessage);
    CHECK(waitFor([&] { return !collector.results.isEmpty(); }));
    CHECK(collector.results.size() == 1 && collector.results.value(0).first == 21u
          && collector.results.value(0).second == QLatin1String("{\"ok\":true,\"how\":\"started\"}"));
    // a result nobody waits for (or a second one) is dropped
    writeReply(driver, "<acr><id>21</id><js>{\"ok\":true}</js></acr>");
    spin(200);
    CHECK(collector.results.size() == 1);
    // a status and a result in one write
    CHECK(api(driver, "Act", {24u, QStringLiteral("{\"a\":\"contacts\",\"q\":\"sam\"}")}).toBool() == true);
    writeReply(driver, "<asr><st>unavailable</st><er>no-url</er><ac>nav,contacts,sms</ac></asr><acr><id>24</id><js>{\"ok\":true,\"c\":[]}</js></acr>");
    CHECK(waitFor([&] { return collector.results.size() == 2; }));
    CHECK(collector.results.value(1).first == 24u);
    // CancelAct: no result comes, the phone is told
    CHECK(api(driver, "Act", {25u, QStringLiteral("{\"a\":\"sms\",\"k\":\"c1\",\"tx\":\"On my way\",\"dry\":true}")}).toBool() == true);
    collector.requests.clear();
    CHECK(api(driver, "CancelAct", {25u}).toBool() == true);
    CHECK(waitFor([&] { return collector.requests.endsWith("<act><id>25</id><cancel/></act>"); }));
    CHECK(api(driver, "CancelAct", {25u}).toBool() == false);
    // the phone goes while an action waits: "disconnected" at once, and no actions
    CHECK(api(driver, "Act", {26u, QStringLiteral("{\"a\":\"sms\",\"k\":\"c1\",\"tx\":\"On my way\",\"dry\":true}")}).toBool() == true);
    notifications.setLinkConnected(false);
    CHECK(waitFor([&] { return collector.results.size() == 3; }));
    CHECK(collector.results.value(2).first == 26u
          && collector.results.value(2).second == QLatin1String("{\"ok\":false,\"er\":\"disconnected\"}"));
    CHECK(api(driver, "Actions").toStringList().isEmpty());
    CHECK(api(driver, "Act", {27u, QStringLiteral("{\"a\":\"nav\",\"q\":\"Riverbend Park\"}")}).toBool() == false);
    // back: actions once the phone says so again
    notifications.setLinkConnected(true);
    spin(200);
    CHECK(api(driver, "Actions").toStringList().isEmpty());
    writeReply(driver, "<asr><st>ready</st><ac>nav,contacts,sms</ac></asr>");
    CHECK(api(driver, "Actions").toStringList().size() == 3);

    /* Contact names for the recogniser (version 3) */
    CHECK(readCaps(driver).contains("<version>3</version>") && readCaps(driver).contains("<names></names>"));
    const QStringList book = inventedBook();
    const QList<QByteArray> parts = namesParts(book, "b1");
    CHECK(parts.size() >= 4);                       // 1025 phrases do not fit in one part
    for (const QByteArray &p : parts)
        CHECK(p.size() < 8192);                     // what the characteristic joins at most
    // an incomplete list writes nothing
    writeInPieces(driver, parts.value(0));
    spin(200);
    CHECK(!QFile::exists(namesFile));
    // all parts, the first again, out of order: one list, written once, in order
    for (int i = parts.size() - 1; i >= 0; i--)
        writeInPieces(driver, parts.at(i));
    CHECK(waitFor([&] { return QFile::exists(namesFile); }));
    QFile f(namesFile);
    CHECK(f.open(QIODevice::ReadOnly));
    const QByteArray data = f.readAll();
    f.close();
    CHECK(data == ContactNames::content(book));
    CHECK(data.count('\n') == 1025);
    struct stat st;
    CHECK(::stat(QFile::encodeName(namesFile).constData(), &st) == 0);
    CHECK((st.st_mode & 0777) == 0600);             // only its owner reads it
    CHECK(st.st_uid == ::getuid());                 // btsyncd's user (ceres on the watch)
    CHECK(::stat(QFile::encodeName(QFileInfo(namesFile).absolutePath()).constData(), &st) == 0 && (st.st_mode & 0777) == 0700);
    // atomic: no temporary file left beside it
    CHECK(QDir(QFileInfo(namesFile).absolutePath()).entryList(QDir::Files | QDir::Hidden | QDir::System).size() == 1);
    // the capabilities say which list the watch has
    const QByteArray expect = QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex().left(16);
    CHECK(readCaps(driver).contains("<names>" + expect + "</names>"));
    // a new list replaces it (a status write and a part in one write as well)
    writeInPieces(driver, "<asr><st>ready</st><ac>nav,contacts,sms</ac></asr><cn><g>b2</g><i>0</i><n>1</n><v>Sam Carter</v><v>Lena Okafor</v></cn>");
    CHECK(waitFor([&] { QFile g(namesFile); return g.open(QIODevice::ReadOnly) && g.readAll() == "Sam Carter\nLena Okafor\n"; }));
    CHECK(api(driver, "Actions").toStringList().size() == 3);
    CHECK(::stat(QFile::encodeName(namesFile).constData(), &st) == 0 && (st.st_mode & 0777) == 0600);
    // the same hash as Gadgetbridge's for this list (AsteroidOSContactNamesTest)
    CHECK(readCaps(driver).contains("<names>2737840c6dedb929</names>"));
    // the setting off on the phone: the list goes
    writeInPieces(driver, "<cn><off/></cn>");
    CHECK(waitFor([&] { return !QFile::exists(namesFile); }));
    CHECK(readCaps(driver).contains("<names></names>"));
    // a list half sent when the phone goes is dropped (it comes again in full)
    writeInPieces(driver, parts.value(0));
    writeInPieces(driver, parts.value(1));
    notifications.setLinkConnected(false);
    spin(200);
    notifications.setLinkConnected(true);
    for (int i = 2; i < parts.size(); i++)
        writeInPieces(driver, parts.at(i));
    spin(300);
    CHECK(!QFile::exists(namesFile));
    // unpaired (no device bonded any more): ContactNames::remove, as main.cpp connects it
    for (const QByteArray &p : namesParts(book, "b3"))
        writeInPieces(driver, p);
    CHECK(waitFor([&] { return QFile::exists(namesFile); }));
    CHECK(ContactNames::remove("test: unpaired"));
    CHECK(!QFile::exists(namesFile) && !ContactNames::remove("again"));

    if (failures)
        fprintf(stderr, "%d check(s) failed\n", failures);
    else
        fprintf(stderr, "assistantlink_test: all checks passed\n");
    return failures ? 1 : 0;
}

#include "assistantlink_test.moc"
