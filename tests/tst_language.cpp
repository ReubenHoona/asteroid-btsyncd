/*
 * Copyright (C) 2026 - eos
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

/*
 * R6 K-05: the daemon's language (src/languagefollower.h, src/eoscatalogues.h). The English catalogue first (an id
 * a language lacks shows English), the locale's chosen by the shared rule (en_NZ, en_AU, en_IE, en_IN: en_GB's;
 * en_US, en_PR: the code's English; de_AT: de_DE's), a change announced by localed followed: handed to the slot
 * and sent as a real PropertiesChanged signal on the private system bus. The daemon's own German and en_GB
 * catalogues at the end. Catalogue cases need TST_I18N_DIR (lrelease: tests/CMakeLists.txt).
 */

#include "languagefollower.h"
#include "eoscatalogues.h"
#include "testmain.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QLocale>
#include <QSignalSpy>
#include <QtTest>

class TstLanguage : public QObject
{
    Q_OBJECT

    static QString hello() { return qtTrId("id-tst-hello"); }
    static QString colour() { return qtTrId("id-tst-colour"); }
    static void say(LanguageFollower &f, const char *lang)
    {
        f.propertiesChanged(QStringLiteral("org.freedesktop.locale1"),
                            { { QStringLiteral("Locale"), QStringList{ QStringLiteral("LANG=") + QLatin1String(lang) } } },
                            {});
    }

private slots:
    void initTestCase()
    {
        qputenv("LANG", "en_GB.utf8");
        QLocale::setDefault(QLocale(QStringLiteral("en_GB")));
    }

    void langOf()
    {
        QCOMPARE(LanguageFollower::langOf({ QStringLiteral("LANG=fr_FR.utf8") }), QStringLiteral("fr_FR.utf8"));
        QCOMPARE(LanguageFollower::langOf({ QStringLiteral("LC_TIME=en_GB.utf8"), QStringLiteral("LANG=de_DE.utf8") }),
                 QStringLiteral("de_DE.utf8"));
        QCOMPARE(LanguageFollower::langOf({ QStringLiteral("LC_TIME=en_GB.utf8") }), QString());
    }

    void rule()
    {
        QVERIFY(EosCatalogues::britishEnglish(QLocale(QStringLiteral("en_NZ"))));
        QVERIFY(EosCatalogues::britishEnglish(QLocale(QStringLiteral("en_IN"))));
        QVERIFY(!EosCatalogues::britishEnglish(QLocale(QStringLiteral("en_US"))));
        QVERIFY(!EosCatalogues::britishEnglish(QLocale(QStringLiteral("en_PR"))));
        QVERIFY(!EosCatalogues::britishEnglish(QLocale(QStringLiteral("de_AT"))));
    }

    void followsTheLanguage()
    {
#ifndef TST_I18N_DIR
        QSKIP("no lrelease: catalogue cases not built");
#else
        LanguageFollower language(QStringLiteral("tst-bt"), QStringLiteral(TST_I18N_DIR));
        QCOMPARE(language.install(), 2); // en_GB: the English one and en_GB's
        QCOMPARE(hello(), QStringLiteral("Hello"));
        QCOMPARE(colour(), QStringLiteral("Colour"));
        QSignalSpy changed(&language, &LanguageFollower::languageChanged);
        language.propertiesChanged(QStringLiteral("org.freedesktop.timedate1"),
                                   { { QStringLiteral("Locale"), QStringList{ QStringLiteral("LANG=de_DE.utf8") } } }, {});
        QCOMPARE(changed.count(), 0);
        say(language, "de_DE.utf8");
        QCOMPARE(changed.count(), 1);
        QCOMPARE(QLocale().name(), QStringLiteral("de_DE"));
        QCOMPARE(qgetenv("LANG"), QByteArray("de_DE.utf8"));
        QCOMPARE(hello(), QStringLiteral("Hallo"));
        QCOMPARE(colour(), QStringLiteral("Color")); // German lacks it: the English catalogue's
        QVERIFY(!language.apply(QStringLiteral("de_DE.utf8")));
        say(language, "de_AT.utf8");
        QCOMPARE(hello(), QStringLiteral("Hallo"));
        // English regions: en_001's read en_GB's, the US and its territories the code's English
        for (const char *lang : { "en_NZ.utf8", "en_AU.utf8", "en_IE.utf8", "en_IN.utf8" }) {
            say(language, lang);
            QVERIFY2(colour() == QStringLiteral("Colour"), lang);
            QCOMPARE(hello(), QStringLiteral("Hello"));
        }
        for (const char *lang : { "en_US.utf8", "en_PR.utf8" }) {
            say(language, lang);
            QVERIFY2(colour() == QStringLiteral("Color"), lang);
        }
        say(language, "sw_KE.utf8");
        QCOMPARE(hello(), QStringLiteral("Hello"));
        say(language, "en_GB.utf8");
        QCOMPARE(colour(), QStringLiteral("Colour"));
#endif
    }

    // localed's signal as it comes: a PropertiesChanged on the system bus
    void overTheBus()
    {
#ifndef TST_I18N_DIR
        QSKIP("no lrelease: catalogue cases not built");
#else
        QDBusConnection bus = QDBusConnection::systemBus();
        QVERIFY(bus.isConnected());
        LanguageFollower language(QStringLiteral("tst-bt"), QStringLiteral(TST_I18N_DIR));
        language.install();
        QVERIFY(language.watch(bus));
        QSignalSpy changed(&language, &LanguageFollower::languageChanged);
        QDBusMessage signal = QDBusMessage::createSignal(QStringLiteral("/org/freedesktop/locale1"),
                                                         QStringLiteral("org.freedesktop.DBus.Properties"),
                                                         QStringLiteral("PropertiesChanged"));
        QVariantMap props;
        props.insert(QStringLiteral("Locale"), QStringList{ QStringLiteral("LANG=de_DE.utf8") });
        signal << QStringLiteral("org.freedesktop.locale1") << props << QStringList();
        QVERIFY(bus.send(signal));
        QTRY_COMPARE(changed.count(), 1);
        QCOMPARE(language.language(), QStringLiteral("de_DE.utf8"));
        QCOMPARE(hello(), QStringLiteral("Hallo"));
        QVERIFY(language.apply(QStringLiteral("en_GB.utf8")));
        QCOMPARE(hello(), QStringLiteral("Hello"));
#endif
    }

    // the daemon's catalogues: the call actions and the connection notice
    void daemonCatalogues()
    {
#ifndef TST_I18N_DIR
        QSKIP("no lrelease: catalogue cases not built");
#else
        LanguageFollower language(QStringLiteral("asteroid-btsyncd"), QStringLiteral(TST_I18N_DIR));
        say(language, "de_DE.utf8");
        QCOMPARE(qtTrId("id-connected"), QStringLiteral("Verbunden"));
        QCOMPARE(QCoreApplication::translate("QObject", "Answer"), QStringLiteral("Annehmen"));
        QCOMPARE(QCoreApplication::translate("QObject", "Phone disconnected"),
                 QStringLiteral("Verbindung zum Smartphone abgebrochen"));
        // the phone's en-NZ reads the en_GB catalogue
        QCOMPARE(EosCatalogues::english(QStringLiteral("asteroid-btsyncd"), QStringLiteral(TST_I18N_DIR),
                                        QLocale(QStringLiteral("en_NZ"))),
                 QStringLiteral(TST_I18N_DIR "/asteroid-btsyncd.en_GB.qm"));
        say(language, "en_NZ.utf8");
        QCOMPARE(QCoreApplication::translate("QObject", "Answer"), QStringLiteral("Answer"));
        say(language, "en_GB.utf8");
#endif
    }
};

BTSYNCD_TEST_MAIN(TstLanguage)
#include "tst_language.moc"
