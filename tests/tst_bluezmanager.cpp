/*
 * BlueZManager against a fake bluetoothd (org.bluez on the private test bus):
 *   - registering the GATT application and the advertisement when an
 *     adapter is found, again when bluetoothd or the adapter comes back
 *   - re-registering the advertisement after a power cycle and when the
 *     periodic check finds no active advertisement (not while connected)
 *   - refreshing only on the BlueZ properties it reads (a device's RSSI
 *     while discovering must not fetch the whole object tree)
 *   - the connected / disconnected toast, a failing GetManagedObjects
 * Addresses are synthetic (00:00:5E:00:53:xx).
 */
#include "bluezmanager.h"
#include "testmain.h"

#include <QDBusMetaType>
#include <QSignalSpy>
#include <QTest>

static const char ADAPTER[] = "org.bluez.Adapter1";
static const char DEVICE[] = "org.bluez.Device1";

class TestBlueZManager : public QObject
{
    Q_OBJECT

private:
    FakePeers *mPeers = nullptr;
    FakeBluez *mBluez = nullptr;
    FakeLipstick *mLip = nullptr;
    BlueZManager *mManager = nullptr;

    int calls(const char *member) const { return mBluez->calls(QString::fromLatin1(member)); }

    void create()
    {
        mManager = new BlueZManager(QDBusObjectPath("/"), QDBusObjectPath("/org/asteroidos/btsyncd/advertisement0"));
    }

    // Lets queued D-Bus traffic settle (for checking that nothing happens)
    static void settle(int ms = 250) { QTest::qWait(ms); }

private slots:
    void initTestCase()
    {
        if (!haveTestBus())
            QSKIP("no D-Bus session bus (run under dbus-run-session)");
        qDBusRegisterMetaType<InterfaceList>();
        mPeers = new FakePeers;
        mBluez = mPeers->bluez();
        mLip = mPeers->lipstick();
        QVERIFY(mPeers->startLipstick());
    }

    void init()
    {
        qunsetenv("BTSYNCD_ADVERT_CHECK_MS");
        mPeers->stopBluez();
        mBluez->removeAll();
        mBluez->resetCalls();
        mBluez->setActiveInstances(1);
        mBluez->setFailGetManagedObjects(false);
        mBluez->setFailRegisterAdvertisement(false);
        mLip->reset();
    }

    void cleanup()
    {
        delete mManager;
        mManager = nullptr;
        settle(50);
    }

    void cleanupTestCase()
    {
        delete mPeers;
    }

    void registersWhenTheAdapterIsThere()
    {
        mBluez->addAdapter(true);
        QVERIFY(mPeers->startBluez());
        create();
        QTRY_COMPARE(calls("RegisterApplication"), 1);
        QTRY_COMPARE(calls("RegisterAdvertisement"), 1);
        QCOMPARE(calls("UnregisterAdvertisement"), 0);
        settle();
        QVERIFY(mLip->notifyCalls().isEmpty());     // no phone connected: no toast
    }

    void waitsForBluetoothd()
    {
        create();
        settle();
        QCOMPARE(calls("GetManagedObjects"), 0);
        mBluez->addAdapter(true);
        QVERIFY(mPeers->startBluez());      // the service watcher
        QTRY_COMPARE(calls("RegisterApplication"), 1);
        QTRY_COMPARE(calls("RegisterAdvertisement"), 1);
    }

    void noAdapterNoRegistration()
    {
        QVERIFY(mPeers->startBluez());      // bluetoothd without a controller
        create();
        QTRY_VERIFY(calls("GetManagedObjects") >= 1);
        settle();
        QCOMPARE(calls("RegisterApplication"), 0);
        // the controller appears
        mBluez->addAdapter(true);
        mBluez->emitInterfacesAdded(FakeBluez::AdapterPath);
        QTRY_COMPARE(calls("RegisterApplication"), 1);
        QTRY_COMPARE(calls("RegisterAdvertisement"), 1);
        // and goes away
        QSignalSpy adapter(mManager, &BlueZManager::adapterChanged);
        mBluez->removeObject(FakeBluez::AdapterPath);
        mBluez->emitInterfacesRemoved(FakeBluez::AdapterPath, {ADAPTER});
        QTRY_COMPARE(adapter.size(), 1);
    }

    void phoneConnectsAndDisconnects()
    {
        mBluez->addAdapter(true);
        mBluez->addDevice(false, false, "Test Phone");
        QVERIFY(mPeers->startBluez());
        create();
        QTRY_COMPARE(calls("RegisterAdvertisement"), 1);
        QSignalSpy connected(mManager, &BlueZManager::connectedChanged);
        QSignalSpy resolved(mManager, &BlueZManager::servicesResolvedChanged);

        mBluez->setDevice(true, false, false);
        mBluez->emitPropertiesChanged(FakeBluez::DevicePath, DEVICE, {{"Connected", true}});
        QTRY_COMPARE(connected.size(), 1);
        QTRY_COMPARE(mLip->notifyCalls().size(), 1);
        LipNotification toast = mLip->notifyCalls().last();
        QCOMPARE(toast.body, QStringLiteral("Test Phone"));
        QCOMPARE(toast.appIcon, QStringLiteral("ios-bluetooth-outline"));
        QCOMPARE(toast.hints.value("transient").toBool(), true);
        QCOMPARE(toast.hints.value("x-nemo-feedback").toString(), QStringLiteral("notif_silent"));

        // services resolved: ANCS and CTS look for the phone's services
        const int gets = calls("GetManagedObjects");
        mBluez->setDevice(true, true, false);
        mBluez->emitPropertiesChanged(FakeBluez::DevicePath, DEVICE, {{"ServicesResolved", true}});
        QTRY_COMPARE(resolved.size(), 1);
        QTRY_COMPARE(calls("GetManagedObjects"), gets + 3);

        mBluez->setDevice(false, false, false);
        mBluez->emitPropertiesChanged(FakeBluez::DevicePath, DEVICE, {{"Connected", false}}, {"ServicesResolved"});
        QTRY_COMPARE(connected.size(), 2);
        QTRY_COMPARE(mLip->notifyCalls().size(), 2);
        toast = mLip->notifyCalls().last();
        QCOMPARE(toast.body, QString());
        QCOMPARE(toast.appIcon, QStringLiteral("ios-bluetooth-off-outline"));
    }

    void onlyThePropertiesItReads()
    {
        mBluez->addAdapter(true);
        mBluez->addDevice(false, false, "Test Phone");
        QVERIFY(mPeers->startBluez());
        create();
        QTRY_COMPARE(calls("RegisterAdvertisement"), 1);
        settle();
        const int gets = calls("GetManagedObjects");
        // discovery noise
        for (int i = 0; i < 20; i++) {
            mBluez->emitPropertiesChanged(FakeBluez::DevicePath, DEVICE, {{"RSSI", -40 - i}, {"TxPower", 4}});
            mBluez->emitPropertiesChanged(FakeBluez::AdapterPath, ADAPTER, {{"Discovering", true}});
            mBluez->emitPropertiesChanged(FakeBluez::DevicePath, "org.bluez.MediaControl1", {{"Connected", true}});
        }
        settle(400);
        QCOMPARE(calls("GetManagedObjects"), gets);
        // a property it reads, also when only invalidated
        mBluez->emitPropertiesChanged(FakeBluez::DevicePath, DEVICE, {}, {"Alias"});
        QTRY_COMPARE(calls("GetManagedObjects"), gets + 1);
        mBluez->emitPropertiesChanged(FakeBluez::AdapterPath, ADAPTER, {{"Powered", true}});
        QTRY_COMPARE(calls("GetManagedObjects"), gets + 2);
    }

    void advertisesAgainAfterAPowerCycle()
    {
        mBluez->addAdapter(true);
        QVERIFY(mPeers->startBluez());
        create();
        QTRY_COMPARE(calls("RegisterAdvertisement"), 1);
        mBluez->setPowered(false, true);
        settle();
        QCOMPARE(calls("UnregisterAdvertisement"), 0);
        mBluez->setPowered(true, true);
        QTRY_COMPARE(calls("UnregisterAdvertisement"), 1);
        QTRY_COMPARE(calls("RegisterAdvertisement"), 2);
        QCOMPARE(calls("RegisterApplication"), 1);  // the GATT application survives a power cycle
    }

    void sameAdapterPathAppearingAgain()
    {
        // hci0 removed and added (controller reset): same path, new object
        mBluez->addAdapter(true);
        QVERIFY(mPeers->startBluez());
        create();
        QTRY_COMPARE(calls("RegisterApplication"), 1);
        mBluez->emitInterfacesAdded(FakeBluez::AdapterPath);
        QTRY_COMPARE(calls("RegisterApplication"), 2);
        QTRY_COMPARE(calls("RegisterAdvertisement"), 2);
        // a device appearing registers nothing
        mBluez->addDevice(false, false, "Other");
        mBluez->emitInterfacesAdded(FakeBluez::DevicePath);
        settle();
        QCOMPARE(calls("RegisterApplication"), 2);
    }

    void bluetoothdRestarts()
    {
        mBluez->addAdapter(true);
        QVERIFY(mPeers->startBluez());
        create();
        QTRY_COMPARE(calls("RegisterApplication"), 1);
        QSignalSpy adapter(mManager, &BlueZManager::adapterChanged);
        mPeers->stopBluez();
        QTRY_COMPARE(adapter.size(), 1);    // no adapter now
        QVERIFY(mPeers->startBluez());
        QTRY_COMPARE(adapter.size(), 2);
        QTRY_COMPARE(calls("RegisterApplication"), 2);
        QTRY_COMPARE(calls("RegisterAdvertisement"), 2);
    }

    void periodicCheck()
    {
        qputenv("BTSYNCD_ADVERT_CHECK_MS", "100");
        mBluez->addAdapter(true);
        mBluez->addDevice(false, false, "Test Phone");
        QVERIFY(mPeers->startBluez());
        create();
        QTRY_COMPARE(calls("RegisterAdvertisement"), 1);
        // BlueZ has it: nothing to do
        QTRY_VERIFY(calls("Get") >= 2);
        QCOMPARE(calls("UnregisterAdvertisement"), 0);
        // BlueZ lost it
        mBluez->setActiveInstances(0);
        QTRY_VERIFY(calls("UnregisterAdvertisement") >= 1);
        QTRY_VERIFY(calls("RegisterAdvertisement") >= 2);
        mBluez->setActiveInstances(1);
        settle();
        // A connected phone: no checks (a peripheral need not advertise)
        mBluez->setDevice(true, true, true);
        settle(300);
        const int gets = calls("Get");
        mBluez->setActiveInstances(0);
        settle(500);
        QCOMPARE(calls("Get"), gets);
        // Powered off: no checks either
        mBluez->setDevice(false, false, true);
        mBluez->setPowered(false, true);
        settle(300);
        const int off = calls("Get");
        settle(400);
        QCOMPARE(calls("Get"), off);
    }

    void failedRegistrationCanBeRetried()
    {
        mBluez->setFailRegisterAdvertisement(true);
        mBluez->addAdapter(true);
        QVERIFY(mPeers->startBluez());
        create();
        QTRY_COMPARE(calls("RegisterAdvertisement"), 1);
        settle();
        mBluez->setFailRegisterAdvertisement(false);
        mManager->reRegisterAdvertisement("test");
        QTRY_COMPARE(calls("RegisterAdvertisement"), 2);
    }

    void oneReRegistrationAtATime()
    {
        mBluez->addAdapter(true);
        QVERIFY(mPeers->startBluez());
        create();
        QTRY_COMPARE(calls("RegisterAdvertisement"), 1);
        mManager->reRegisterAdvertisement("first");
        mManager->reRegisterAdvertisement("second");    // while the first is in flight
        QTRY_COMPARE(calls("RegisterAdvertisement"), 2);
        settle();
        QCOMPARE(calls("UnregisterAdvertisement"), 1);
        // and none without an adapter
        mPeers->stopBluez();
        settle();
        mManager->reRegisterAdvertisement("no adapter");
        settle();
        QCOMPARE(calls("UnregisterAdvertisement"), 1);
    }

    void failingGetManagedObjectsKeepsTheState()
    {
        mBluez->addAdapter(true);
        mBluez->addDevice(true, false, "Test Phone");
        QVERIFY(mPeers->startBluez());
        create();
        QTRY_COMPARE(mLip->notifyCalls().size(), 1);    // connected
        QSignalSpy connected(mManager, &BlueZManager::connectedChanged);
        QSignalSpy adapter(mManager, &BlueZManager::adapterChanged);
        mBluez->setFailGetManagedObjects(true);
        mBluez->emitPropertiesChanged(FakeBluez::DevicePath, DEVICE, {{"Connected", false}});
        QTRY_VERIFY(calls("GetManagedObjects") >= 2);
        settle();
        QCOMPARE(connected.size(), 0);
        QCOMPARE(adapter.size(), 0);
        QCOMPARE(calls("RegisterApplication"), 1);
    }
};

BTSYNCD_TEST_MAIN(TestBlueZManager)
#include "tst_bluezmanager.moc"
