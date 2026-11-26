/*
 * The phone's <insert> as btsyncd reads it (PhoneNotification::parse), element by element, for
 * the elements of protocol versions 4 and 5 and CR-52, and every insert Gadgetbridge records
 * (frames/notification-*.frames.txt, fixtures/missed-call-card.frames.txt). No bus needed.
 * Names are invented.
 */
#include "fixtures.h"
#include "frames.h"
#include "notificationservice.h"
#include "testmain.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>
#include <QXmlStreamReader>

static PhoneNotification parse(const QByteArray &inner, bool *error = nullptr)
{
    QXmlStreamReader r("<insert><id>7</id>" + inner + "</insert>");
    r.readNextStartElement();
    const PhoneNotification n = PhoneNotification::parse(r);
    if (error)
        *error = r.hasError();
    return n;
}

class TestNotifParse : public QObject
{
    Q_OBJECT

private slots:
    void basics()
    {
        bool error = true;
        const PhoneNotification n = parse("<pn>com.example.chat</pn><an>Example Chat</an><ai>ios-chatbubbles</ai>"
                                          "<su>Ben &amp; Cleo</su><bo>a &lt;b&gt;</bo><vb>normal</vb>"
                                          "<ac> reply , open,,dismiss </ac><rs>1</rs><x-new><y>skipped</y></x-new>",
                                          &error);
        QVERIFY(!error);
        QCOMPARE(n.id, 7);
        QCOMPARE(n.packageName, QStringLiteral("com.example.chat"));
        QCOMPARE(n.appName, QStringLiteral("Example Chat"));
        QCOMPARE(n.appIcon, QStringLiteral("ios-chatbubbles"));
        QCOMPARE(n.summary, QStringLiteral("Ben & Cleo"));
        QCOMPARE(n.body, QStringLiteral("a <b>"));
        QCOMPARE(n.vibrate, QStringLiteral("normal"));
        QCOMPARE(n.phoneActions, (QStringList{"reply", "open", "dismiss"}));
        QVERIFY(n.resync);
        QVERIFY(!n.isCall());
        // an old phone: no <vb> is a null string (the default pattern)
        QVERIFY(parse("<su>S</su>").vibrate.isNull());
        QVERIFY(parse("<ac>answer,reject</ac>").isCall());
    }

    void appActionsAndSmartReplies()
    {
        QByteArray ax = "<ax>";
        for (int i = 1; i <= 12; i++)
            ax += "<a>Action " + QByteArray::number(i) + "</a>";
        ax += "<a>   </a><b>not one</b></ax>";
        const PhoneNotification n = parse(ax + "<sr><r>Yes</r><r>&amp;#x1F603;</r><r>&amp;#128077; ok</r><r>No</r>"
                                               "<r>&#x1F44D;</r><r>Sixth</r></sr>");
        QCOMPARE(n.appActions.size(), 10);              // MaxAppActions
        QCOMPARE(n.appActions.first(), QStringLiteral("Action 1"));
        QCOMPARE(n.appActions.last(), QStringLiteral("Action 10"));
        QCOMPARE(n.smartReplies.size(), 5);             // MaxSmartReplies
        QCOMPARE(n.smartReplies.value(1), QString::fromUcs4(U"\U0001F603"));
        QCOMPARE(n.smartReplies.value(2), QString::fromUcs4(U"\U0001F44D ok"));
        // as HTML character references in the text (some notification assistants): decoded, counted;
        // an XML character reference is the parser's
        QCOMPARE(n.smartReplies.value(4), QString::fromUcs4(U"👍"));
        QCOMPARE(n.smartReplyEntities, 2);
        // long labels end with an ellipsis (40 characters)
        const PhoneNotification l = parse("<ax><a>" + QByteArray(50, 'x') + "</a></ax>");
        QCOMPARE(l.appActions.value(0).size(), 40);
        QVERIFY(l.appActions.value(0).endsWith(QChar(0x2026)));
    }

    void conversation()
    {
        const PhoneNotification n = parse(
            "<su>Ben Sample</su><th> t42 </th><ky> k9 </ky><cv>t21</cv><ca>msg</ca>"
            "<ms><m><s>Ben Sample</s><t>Dinner?</t><ts>1793199900</ts></m>"
            "<m><s>Me</s><t> Yes </t><me>1</me><ts>1793199950123</ts></m>"
            "<m><s>Ben Sample</s><t></t></m>"           // nothing to show: dropped
            "<m><s>Ben Sample</s><t>" + QByteArray(320, 'w') + "</t><ts>12</ts></m>"
            "<x>skipped</x></ms>");
        QCOMPARE(n.thread, QStringLiteral("t42"));
        QCOMPARE(n.key, QStringLiteral("k9"));
        QCOMPARE(n.conversation, QStringLiteral("t21"));
        QCOMPARE(n.category, QStringLiteral("msg"));
        QCOMPARE(n.messages.size(), 3);
        QCOMPARE(n.messages.value(0), qMakePair(QStringLiteral("Ben Sample"), QStringLiteral("Dinner?")));
        QCOMPARE(n.messages.value(1).second, QStringLiteral("Yes"));
        QCOMPARE(n.ownMessages, (QList<bool>{false, true, false}));
        // seconds, milliseconds taken as seconds, a time outside this century as unknown
        QCOMPARE(n.messageTimes, (QList<qint64>{1793199900, 1793199950, 0}));
        // a message is at most 300 characters
        QCOMPARE(n.messages.value(2).second.size(), 300);
        QVERIFY(n.messages.value(2).second.endsWith(QChar(0x2026)));
        // more than one sender: each line says whose it is; the banner shows the newest
        QVERIFY(n.shownBody().startsWith(QStringLiteral("Ben Sample: Dinner?\nMe: Yes\n")));
        QVERIFY(n.previewBody().startsWith(QStringLiteral("www")));
        const QJsonArray json = QJsonDocument::fromJson(n.messagesJson().toUtf8()).array();
        QCOMPARE(json.size(), 3);
        QCOMPARE(json.at(1).toObject().value("me").toBool(), true);
        QCOMPARE(json.at(0).toObject().value("ts").toInteger(), qint64(1793199900));
        QVERIFY(!json.at(2).toObject().contains("ts"));
        // keys: at most 64 characters; a conversation key only of the convs act's shape (CR-52)
        QCOMPARE(parse("<th>" + QByteArray(80, 't') + "</th>").thread.size(), 64);
        QCOMPARE(parse("<cv>n_ab-9</cv>").conversation, QStringLiteral("n_ab-9"));
        QCOMPARE(parse("<cv>x21</cv>").conversation, QString());
        QCOMPARE(parse("<cv>t 21</cv>").conversation, QString());
        QCOMPARE(parse("<cv>t" + QByteArray(64, '1') + "</cv>").conversation, QString());
    }

    void alertFlags()
    {
        QCOMPARE(parse("<pr>3</pr>").importance, 3);
        QCOMPARE(parse("<pr>9</pr>").importance, 5);
        QCOMPARE(parse("<pr>-2</pr>").importance, 0);
        QCOMPARE(parse("<pr>high</pr>").importance, 0);
        QVERIFY(parse("<oa>1</oa>").onlyAlertOnce);
        QVERIFY(!parse("<oa>0</oa>").onlyAlertOnce);
        QVERIFY(parse("<og> 1 </og>").ongoing);
        QVERIFY(parse("<sd>1</sd>").starred);
        QVERIFY(parse("<ne>1</ne>").noEmoji);
        QCOMPARE(parse("<ca>" + QByteArray(40, 'c') + "</ca>").category.size(), 32);
        QCOMPARE(parse("<ic> abc123 </ic>").iconHash, QStringLiteral("abc123"));
        // the sub text: one line, at most 60 characters
        QCOMPARE(parse("<st>  Inbox \n work  </st>").subText, QStringLiteral("Inbox work"));
        const QString longSub = parse("<st>" + QByteArray(70, 's') + "</st>").subText;
        QCOMPARE(longSub.size(), 60);
        QVERIFY(longSub.endsWith(QChar(0x2026)));
        // <wh>: seconds of this century, milliseconds taken
        QCOMPARE(parse("<wh>1793200000</wh>").when, qint64(1793200000));
        QCOMPARE(parse("<wh>1793200000999</wh>").when, qint64(1793200000));
        QCOMPARE(parse("<wh>999</wh>").when, qint64(0));
        QCOMPARE(parse("<wh>4102444801</wh>").when, qint64(0));
        QCOMPARE(parse("<wh>soon</wh>").when, qint64(0));
    }

    void progress()
    {
        PhoneNotification n = parse("<pg><p>40</p><x>100</x><sg>25:ff0000,75:-</sg></pg>");
        QVERIFY(n.progress.present && !n.progress.indeterminate);
        QCOMPARE(n.progress.value, 40);
        QCOMPARE(n.progress.max, 100);
        QCOMPARE(n.progress.segments, QStringLiteral("25:ff0000,75:-"));
        n = parse("<pg><p>140</p><x>100</x><sg>bad</sg></pg>");
        QCOMPARE(n.progress.value, 100);
        QCOMPARE(n.progress.segments, QString());
        n = parse("<pg><i>1</i></pg>");
        QVERIFY(n.progress.present && n.progress.indeterminate);
        QVERIFY(!parse("<pg><x>100</x></pg>").progress.present);       // no value: no bar
        QCOMPARE(parse("<pg><p>5</p></pg>").progress.max, 100);
        QCOMPARE(parse("<pg><p>5</p><x>5000000</x></pg>").progress.max, 1000000);
    }

    void pictures()
    {
        const PhoneNotification n = parse("<pc><i>0123456789abcdef</i><w>400</w><h>9000</h></pc>"
                                          "<ms><m><t>look</t><pc><i>00000000000000aa</i></pc>"
                                          "<av><i>00000000000000bb</i><w>48</w><h>48</h></av></m></ms>");
        QCOMPARE(n.pictureId, QStringLiteral("0123456789abcdef"));
        QCOMPARE(n.pictureWidth, 400);
        QCOMPARE(n.pictureHeight, 4096);
        QCOMPARE(n.newestMessagePicture(), QStringLiteral("00000000000000aa"));
        QCOMPARE(n.pictureIds(), (QStringList{"0123456789abcdef", "00000000000000aa", "00000000000000bb"}));
        // only ids of the picture channel's shape (16 hex digits)
        QCOMPARE(parse("<pc><i>../../etc</i></pc>").pictureId, QString());
        QCOMPARE(parse("<pc><i>0123456789ABCDEF</i></pc>").pictureId, QString());
    }

    // Gadgetbridge's own inserts parse whole (CR-52 elements included)
    void gadgetbridgeFrames_data()
    {
        QTest::addColumn<QByteArray>("frame");
        int line = 0;
        for (const char *name : {"notification-w2", "notification-fp-g1", "notification-capabilities-w2"}) {
            line = 0;
            for (const QByteArray &f : loadFrames(name)) {
                ++line;
                if (f.startsWith("<insert>"))
                    QTest::newRow(qPrintable(QStringLiteral("%1:%2").arg(QLatin1String(name)).arg(line))) << f;
            }
        }
        line = 0;
        for (const QByteArray &f : fixtureFrames("missed-call-card.frames.txt")) {
            ++line;
            if (f.startsWith("<insert>"))
                QTest::newRow(qPrintable(QStringLiteral("missed-call-card:%1").arg(line))) << f;
        }
    }

    void gadgetbridgeFrames()
    {
        QFETCH(QByteArray, frame);
        QXmlStreamReader r(frame);
        QVERIFY(r.readNextStartElement());
        const PhoneNotification n = PhoneNotification::parse(r);
        QVERIFY2(!r.hasError(), qPrintable(r.errorString()));
        QVERIFY(n.id != 0);
        QVERIFY(!n.appName.isEmpty());
        if (frame.contains("<wh>"))
            QVERIFY(n.when > 0);
        if (frame.contains("<cv>"))
            QVERIFY(!n.conversation.isEmpty());
        if (frame.contains("<sd>1</sd>"))
            QVERIFY(n.starred);
    }
};

BTSYNCD_TEST_MAIN(TestNotifParse)
#include "tst_notifparse.moc"
