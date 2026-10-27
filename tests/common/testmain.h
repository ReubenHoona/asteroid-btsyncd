/*
 * main() for the Qt Test suites: a private D-Bus (see fakepeers.h) as the
 * system bus too, UTC, and HOME / XDG_CONFIG_HOME in a temporary directory
 * so nothing reads or writes the user's own settings.
 */
#ifndef TESTMAIN_H
#define TESTMAIN_H

#include "fakepeers.h"

#include <QCoreApplication>
#include <QTemporaryDir>
#include <QTest>
#include <ctime>

#define BTSYNCD_TEST_MAIN(TestClass)                                              \
    int main(int argc, char **argv)                                               \
    {                                                                             \
        initTestBus();                                                            \
        qputenv("TZ", "UTC");                                                     \
        tzset();                                                                  \
        QTemporaryDir home;                                                       \
        qputenv("HOME", home.path().toUtf8());                                    \
        qputenv("XDG_CONFIG_HOME", (home.path() + "/.config").toUtf8());          \
        qunsetenv("BTSYNCD_VOICE_SOURCE");                                        \
        QCoreApplication app(argc, argv);                                         \
        TestClass tc;                                                             \
        return QTest::qExec(&tc, argc, argv);                                     \
    }

#endif // TESTMAIN_H
