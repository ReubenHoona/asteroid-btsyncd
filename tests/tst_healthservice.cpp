/*
 * The health GATT service: request parsing (HealthRequestChrc), the answer
 * on the data characteristic (HealthDataChrc, MTU-sized pieces), the
 * capabilities read, where the records are found (dconf, sensorlogd's
 * settings, the default), and the record paging of HealthRecords::query
 * over a long history (every record exactly once, any page size).
 */
#include "healthservice.h"
#include "healthrecords.h"
#include "mdconfitem.h"
#include "testmain.h"

#include <QDBusMessage>
#include <QDBusVariant>
#include <QDate>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>

using namespace HealthRecords;

static const char DATA_DIR_KEY[] = "/org/asteroidos/btsyncd/health-data-dir";

static qint64 at(const QDate &date, int h, int m, int s = 0)
{
    return QDateTime(date, QTime(h, m, s)).toSecsSinceEpoch();
}

static void writeDay(const QString &root, const QString &sensor, const QDate &date, const QByteArray &content)
{
    QDir().mkpath(root + "/" + sensor);
    QFile f(root + "/" + sensor + "/" + date.toString("yyyy-MM-dd") + ".log");
    QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write(content);
}

static QByteArray rec(qint64 t, qint64 v)
{
    return QByteArray::number(t) + ":" + QByteArray::number(v) + "\n";
}

class TestHealthService : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir mDir;
    HealthService *mService = nullptr;
    Characteristic *mRequest = nullptr;
    HealthDataChrc *mData = nullptr;
    Characteristic *mCaps = nullptr;
    QList<QByteArray> mChunks;
    const QDate mDay = QDate(2026, 3, 10);

    QByteArray answer()
    {
        // The answer goes out after the write returns
        (void)QTest::qWaitFor([this] { return !mChunks.isEmpty(); }, 2000);
        QByteArray all;
        for (const QByteArray &c : std::as_const(mChunks))
            all += c;
        return all;
    }

    void write(const QByteArray &value, int mtu = 100)
    {
        QVariantMap options;
        if (mtu > 0)
            options.insert("mtu", mtu);
        mRequest->WriteValue(value, options);
    }

private slots:
    void initTestCase()
    {
        QVERIFY(mDir.isValid());
        mService = new HealthService(6, QDBusConnection::systemBus(), this);
        const QList<Characteristic *> chrcs = mService->getCharacteristics();
        QCOMPARE(chrcs.size(), 3);
        mRequest = chrcs[0];
        mData = qobject_cast<HealthDataChrc *>(chrcs[1]);
        mCaps = chrcs[2];
        QVERIFY(mData);
        QCOMPARE(mRequest->getUuid(), QStringLiteral(HEALTH_REQ_UUID));
        QCOMPARE(mData->getUuid(), QStringLiteral(HEALTH_DATA_UUID));
        QCOMPARE(mCaps->getUuid(), QStringLiteral(HEALTH_CAP_UUID));
        QCOMPARE(mService->getUuid(), QStringLiteral(HEALTH_UUID));
        connect(mData, &HealthDataChrc::valueChanged, this, [this] {
            mChunks << mData->property("Value").toByteArray();
        });

        const QString root = mDir.path();
        writeDay(root, "heartrateMonitor", mDay,
                 rec(at(mDay, 8, 0), 70) + rec(at(mDay, 8, 1), 72) + rec(at(mDay, 8, 2), 75));
        writeDay(root, "stepCounter", mDay,
                 rec(at(mDay, 9, 0), 100) + rec(at(mDay, 10, 0), 250) + rec(at(mDay, 23, 59, 59), 300));
    }

    void init()
    {
        mChunks.clear();
        MDConfItem::reset();
        MDConfItem(DATA_DIR_KEY).set(mDir.path());   // no trailing slash: dataRoot adds it
    }

    void capabilities()
    {
        const QByteArray caps = "<health><version>1</version><types>hr,steps</types><mx>1000</mx></health>";
        QCOMPARE(mCaps->ReadValue({}), caps);
        QCOMPARE(mCaps->ReadValue({{"offset", 9}}), caps.mid(9));
        QCOMPARE(mCaps->ReadValue({{"offset", 0}}), caps);
    }

    void heartRateInOneWrite()
    {
        write("<req><ty>hr</ty><si>0</si><mx>1000</mx></req>");
        const qint64 t0 = at(mDay, 8, 0);
        QCOMPARE(answer(), "<data><ty>hr</ty><t0>" + QByteArray::number(t0) + "</t0><n>3</n><mo>0</mo>"
                           "<r>0,70;60,72;60,75</r></data>");
        for (const QByteArray &c : std::as_const(mChunks))
            QVERIFY(c.size() <= 97);
    }

    void requestSplitOverWrites()
    {
        write("<req><ty>st", 0);
        write("eps</ty><si>", 0);
        QTest::qWait(10);
        QVERIFY(mChunks.isEmpty());     // nothing until </req>
        write("0</si></req>", 0);
        const QByteArray xml = answer();
        QVERIFY2(xml.startsWith("<data><ty>steps</ty><t0>" + QByteArray::number(at(mDay, 0, 0)) + "</t0><n>3</n>"), xml);
        QVERIFY(xml.contains("<r>32400,100;3600,150;50399,50</r>"));
        // No MTU in the options: the last one the phone wrote with (100)
        for (const QByteArray &c : std::as_const(mChunks))
            QVERIFY(c.size() <= 97);
    }

    void pagedRequest()
    {
        write("<req><ty>hr</ty><si>" + QByteArray::number(at(mDay, 8, 0)) + "</si><mx>1</mx></req>");
        QCOMPARE(answer(), "<data><ty>hr</ty><t0>" + QByteArray::number(at(mDay, 8, 1))
                           + "</t0><n>1</n><mo>1</mo><r>0,72</r></data>");
    }

    void largeMtuIsCapped()
    {
        write("<req><ty>hr</ty><si>0</si></req>", 1000);
        answer();
        QCOMPARE(mChunks.size(), 1);    // < 509 bytes: one notification
    }

    void unknownType()
    {
        write("<req><ty>spo2</ty><si>0</si></req>");
        QCOMPARE(answer(), QByteArray("<data><ty>spo2</ty><er>type</er></data>"));
    }

    void missingType()
    {
        write("<req><si>0</si></req>");
        QCOMPARE(answer(), QByteArray("<data><ty>?</ty><er>type</er></data>"));
    }

    void escapedType()
    {
        write("<req><ty>a&lt;b</ty></req>");
        QCOMPARE(answer(), QByteArray("<data><ty>a&lt;b</ty><er>type</er></data>"));
    }

    void badNumber_data()
    {
        QTest::addColumn<QByteArray>("request");
        QTest::newRow("since") << QByteArray("<req><ty>hr</ty><si>yesterday</si></req>");
        QTest::newRow("max") << QByteArray("<req><ty>hr</ty><mx>lots</mx></req>");
        QTest::newRow("root") << QByteArray("<request><ty>hr</ty></request></req>");
        QTest::newRow("broken xml") << QByteArray("<req><ty>hr</si></req>");
    }

    void badNumber()
    {
        QFETCH(QByteArray, request);
        write(request);
        QCOMPARE(answer(), QByteArray("<data><ty></ty><er>request</er></data>"));
    }

    void unknownElementsAreSkipped()
    {
        write("<req><v>2</v><ty>hr</ty><x><y>1</y></x><si>0</si></req>");
        QVERIFY(answer().startsWith("<data><ty>hr</ty><t0>"));
    }

    void endlessRequestIsDropped()
    {
        write("<req><ty>hr</ty><si>" + QByteArray(600, '1'), 0);
        QTest::qWait(10);
        QVERIFY(mChunks.isEmpty());
        write(QByteArray(600, '2'), 0);     // > 1024 bytes without </req>
        QCOMPARE(answer(), QByteArray("<data><ty></ty><er>request</er></data>"));
        // and the buffer starts afresh
        mChunks.clear();
        write("<req><ty>hr</ty><si>0</si></req>");
        QVERIFY(answer().startsWith("<data><ty>hr</ty><t0>"));
    }

    void newerRequestReplacesAnUnansweredOne()
    {
        write("<req><ty>steps</ty><si>0</si></req>");
        write("<req><ty>hr</ty><si>0</si></req>");
        const QByteArray xml = answer();
        QTest::qWait(20);
        QCOMPARE(xml.count("<data>"), 1);
        QVERIFY(xml.startsWith("<data><ty>hr</ty>"));
    }

    void trailingBytesAfterTheEndAreDropped()
    {
        write("<req><ty>hr</ty><si>0</si></req><req><ty>st");
        QVERIFY(answer().startsWith("<data><ty>hr</ty>"));
        mChunks.clear();
        write("<req><ty>steps</ty></req>");
        QVERIFY(answer().startsWith("<data><ty>steps</ty>"));
    }

    void rootFromSensorlogdSettings()
    {
        MDConfItem::reset();    // no dconf key
        QTemporaryDir other;
        writeDay(other.path(), "heartrateMonitor", mDay, rec(at(mDay, 12, 0), 66));
        {
            QSettings settings("asteroid", "sensorlogd");
            settings.setValue("loggerRootPath", other.path() + "/");
            settings.sync();
        }
        write("<req><ty>hr</ty></req>");
        QCOMPARE(answer(), "<data><ty>hr</ty><t0>" + QByteArray::number(at(mDay, 12, 0))
                           + "</t0><n>1</n><mo>0</mo><r>0,66</r></data>");
        QSettings("asteroid", "sensorlogd").remove("loggerRootPath");
    }

    void defaultRoot()
    {
        MDConfItem::reset();
        // HOME is a temporary directory (testmain.h): ~/.asteroid-sensorlogd/
        writeDay(QDir::homePath() + "/.asteroid-sensorlogd", "heartrateMonitor", mDay, rec(at(mDay, 13, 0), 61));
        write("<req><ty>hr</ty></req>");
        QCOMPARE(answer(), "<data><ty>hr</ty><t0>" + QByteArray::number(at(mDay, 13, 0))
                           + "</t0><n>1</n><mo>0</mo><r>0,61</r></data>");
    }

    void overDBus()
    {
        if (!haveTestBus())
            QSKIP("no D-Bus session bus (run under dbus-run-session)");
        // BlueZ's view: the objects btsyncd registered on the (system) bus
        const QString me = QDBusConnection::systemBus().baseService();
        QDBusMessage get = QDBusMessage::createMethodCall(me, mCaps->getPath().path(), DBUS_PROPERTIES_IFACE, "Get");
        get << QString(GATT_CHRC_IFACE) << QString("UUID");
        QDBusMessage reply = callAsPeer(get);
        QCOMPARE(reply.type(), QDBusMessage::ReplyMessage);
        QCOMPARE(reply.arguments().value(0).value<QDBusVariant>().variant().toString(), QStringLiteral(HEALTH_CAP_UUID));

        QDBusMessage read = QDBusMessage::createMethodCall(me, mCaps->getPath().path(), GATT_CHRC_IFACE, "ReadValue");
        read << QVariantMap();
        reply = callAsPeer(read);
        QCOMPARE(reply.type(), QDBusMessage::ReplyMessage);
        QVERIFY(reply.arguments().value(0).toByteArray().startsWith("<health>"));

        QDBusMessage req = QDBusMessage::createMethodCall(me, mRequest->getPath().path(), GATT_CHRC_IFACE, "WriteValue");
        req << QByteArray("<req><ty>hr</ty><si>0</si></req>") << QVariantMap{{"mtu", 185}};
        reply = callAsPeer(req);
        QCOMPARE(reply.type(), QDBusMessage::ReplyMessage);
        QVERIFY(answer().startsWith("<data><ty>hr</ty>"));
    }

    /* HealthRecords beyond the existing healthrecords_test */

    void malformedLinesAreSkipped()
    {
        QTemporaryDir dir;
        const QDate d(2026, 4, 1);
        writeDay(dir.path(), "heartrateMonitor", d,
                 "garbage\n:70\n" + QByteArray::number(at(d, 1, 0)) + ":\n-5:70\n0:70\n"
                 + QByteArray::number(at(d, 1, 1)) + ":x\n"
                 + rec(at(d, 1, 2), 0) + rec(at(d, 1, 3), 300) + rec(at(d, 1, 4), 299)
                 + "  " + rec(at(d, 1, 5), 80));
        const Page p = query(dir.path() + "/", HeartRate, 0);
        QCOMPARE(p.records.size(), 2);
        QCOMPARE(p.records[0].value, 299);
        QCOMPARE(p.records[1].value, 80);
    }

    void oddFileNamesAreIgnored()
    {
        QTemporaryDir dir;
        const QDate d(2026, 4, 2);
        writeDay(dir.path(), "heartrateMonitor", d, rec(at(d, 2, 0), 70));
        QFile bad(dir.path() + "/heartrateMonitor/2026-13-45.log");
        QVERIFY(bad.open(QIODevice::WriteOnly));
        bad.write(rec(at(d, 3, 0), 71));
        bad.close();
        QFile notes(dir.path() + "/heartrateMonitor/notes.log");
        QVERIFY(notes.open(QIODevice::WriteOnly));
        notes.write(rec(at(d, 4, 0), 72));
        notes.close();
        const Page p = query(dir.path() + "/", HeartRate, 0);
        QCOMPARE(p.records.size(), 1);
        QCOMPARE(p.records[0].value, 70);
    }

    void maxIsClampedToMaxRecords()
    {
        QTemporaryDir dir;
        const QDate d(2026, 4, 3);
        QByteArray day;
        for (int i = 0; i < MaxRecords + 200; i++)
            day += rec(at(d, 0, 0) + i * 30, 60 + i % 50);
        writeDay(dir.path(), "heartrateMonitor", d, day);
        Page p = query(dir.path() + "/", HeartRate, 0, 5000);
        QCOMPARE(p.records.size(), qsizetype(MaxRecords));
        QVERIFY(p.more);
        p = query(dir.path() + "/", HeartRate, 0, -1);
        QCOMPARE(p.records.size(), qsizetype(MaxRecords));
    }

    void pagingVisitsEveryRecordOnce_data()
    {
        QTest::addColumn<QByteArray>("tz");
        QTest::addColumn<int>("max");
        QTest::addColumn<int>("type");
        // POSIX offsets: UTC, UTC+14 and UTC-12 (the extremes a day file can be written in)
        for (const char *tz : {"UTC0", "<+14>-14", "<-12>+12"}) {
            for (int max : {1, 7, 50, 1000}) {
                QTest::addRow("%s hr max %d", tz, max) << QByteArray(tz) << max << int(HeartRate);
                QTest::addRow("%s steps max %d", tz, max) << QByteArray(tz) << max << int(Steps);
            }
        }
    }

    void pagingVisitsEveryRecordOnce()
    {
        // Two weeks of a day file each, ties included, and the phone asking
        // page after page from the last time it got: every record exactly
        // once, in order, whatever the page size (the shortened reading of
        // 6a0bf2b must not lose the days it skips)
        QFETCH(QByteArray, tz);
        QFETCH(int, max);
        QFETCH(int, type);
        qputenv("TZ", tz);
        tzset();

        QTemporaryDir dir;
        const QString sensor = type == HeartRate ? "heartrateMonitor" : "stepCounter";
        const QDate start(2026, 5, 1);
        QList<qint64> expected;
        for (int day = 0; day < 14; day++) {
            const QDate d = start.addDays(day);
            if (day == 5)
                continue;   // a day without records
            QByteArray content;
            const int n = 3 + (day * 7) % 11;
            qint64 steps = 0;
            for (int i = 0; i < n; i++) {
                const qint64 t = at(d, 6 + i, 0);
                const int copies = (i == 2) ? 3 : 1;   // a time with several records
                for (int c = 0; c < copies; c++) {
                    steps += 10 + i;
                    content += rec(t, type == HeartRate ? 60 + i : steps);
                    expected << t;
                }
            }
            writeDay(dir.path(), sensor, d, content);
        }

        QList<qint64> seen;
        qint64 since = 0;
        int pages = 0;
        for (;;) {
            const Page p = query(dir.path() + "/", Type(type), since, max);
            QVERIFY(pages++ < 1000);
            for (const Record &r : p.records) {
                QVERIFY2(r.time > since, "a record at or before since");
                seen << r.time;
            }
            if (!p.records.isEmpty()) {
                since = p.records.last().time;
                // The XML the phone gets for this page decodes to the same times
                const QByteArray xml = toXml(Type(type), p);
                QVERIFY(xml.contains("<n>" + QByteArray::number(p.records.size()) + "</n>"));
            }
            if (!p.more)
                break;
            QVERIFY2(!p.records.isEmpty(), "more without records: the phone would ask the same page again");
        }
        QCOMPARE(seen, expected);

        qputenv("TZ", "UTC");
        tzset();
    }

    void pageXmlDecodes()
    {
        // t0 + the deltas in <r> give back every record's time
        Page p;
        p.t0 = 1000;
        p.records = {{1000, 70}, {1060, 71}, {1060, 72}, {1200, 73}};
        p.more = true;
        QCOMPARE(toXml(HeartRate, p),
                 QByteArray("<data><ty>hr</ty><t0>1000</t0><n>4</n><mo>1</mo><r>0,70;60,71;0,72;140,73</r></data>"));
        Page steps;
        steps.t0 = 900;     // the interval the first value covers began earlier
        steps.records = {{1000, 5}};
        QCOMPARE(toXml(Steps, steps),
                 QByteArray("<data><ty>steps</ty><t0>900</t0><n>1</n><mo>0</mo><r>100,5</r></data>"));
        QCOMPARE(toXml(Unknown, Page()), QByteArray("<data><ty></ty><n>0</n><mo>0</mo></data>"));
        QCOMPARE(errorXml("x", "a&b"), QByteArray("<data><ty>x</ty><er>a&amp;b</er></data>"));
    }
};

BTSYNCD_TEST_MAIN(TestHealthService)
#include "tst_healthservice.moc"
