/*
 * The D-Bus peers btsyncd talks to, faked for the tests on a private session
 * bus (run the tests under dbus-run-session):
 *   FakeLipstick  org.freedesktop.Notifications (Notify, CloseNotification,
 *                 GetNotifications, the NotificationClosed / ActionInvoked
 *                 signals)
 *   FakeBluez     org.bluez: the object manager, an adapter with
 *                 GattManager1 / LEAdvertisingManager1 / Adapter1, devices
 * They run on their own connections in their own thread, so btsyncd's
 * blocking calls from the test thread get answered. btsyncd's "system bus"
 * is the same private bus (initTestBus() points DBUS_SYSTEM_BUS_ADDRESS at
 * it), so its GATT objects and PropertiesChanged signals work too.
 *
 * All addresses are synthetic (00:00:5E:00:53:xx, RFC 7042 documentation).
 */
#ifndef FAKEPEERS_H
#define FAKEPEERS_H

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusContext>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusVirtualObject>
#include <QList>
#include <QMap>
#include <QMutex>
#include <QObject>
#include <QStringList>
#include <QThread>
#include <QVariantMap>

// Call before the QCoreApplication: false (tests should QSKIP) without a
// session bus
bool initTestBus();
bool haveTestBus();

// A method call from another peer (BlueZ, the launcher) to btsyncd's objects
// on the test bus, answered while the test thread runs its event loop
QDBusMessage callAsPeer(const QDBusMessage &call, int timeoutMs = 5000);

struct LipNotification {
    QString appName;
    uint id = 0;
    QString appIcon;
    QString summary;
    QString body;
    QStringList actions;
    QVariantMap hints;
    int timeout = 0;
};
Q_DECLARE_METATYPE(LipNotification)
QDBusArgument &operator<<(QDBusArgument &arg, const LipNotification &n);
const QDBusArgument &operator>>(const QDBusArgument &arg, LipNotification &n);

class FakeLipstick : public QObject, protected QDBusContext
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.Notifications")

public:
    // Thread-safe, from the test thread
    QList<LipNotification> notifyCalls() const;
    QList<uint> closeCalls() const;
    int getNotificationsCalls() const;
    QMap<uint, LipNotification> shown() const;
    void reset();
    // Notify with a replaces id answers 0 (lipstick lost it)
    void setReplaceGone(bool gone);
    // What lipstick shows, as GetNotifications lists it
    void setShown(const QList<LipNotification> &list);
    void emitClosed(uint id, uint reason);
    void emitAction(uint id, const QString &key);

public slots:
    uint Notify(const QString &appName, uint replacesId, const QString &appIcon, const QString &summary,
                const QString &body, const QStringList &actions, const QVariantMap &hints, int timeout);
    void CloseNotification(uint id);
    QList<LipNotification> GetNotifications(const QString &owner);

public:
    QDBusConnection *connection = nullptr;

private:
    mutable QMutex mMutex;
    QList<LipNotification> mNotifyCalls;
    QList<uint> mCloseCalls;
    QMap<uint, LipNotification> mShown;
    int mGetCalls = 0;
    uint mNextId = 100;
    bool mReplaceGone = false;
};

typedef QMap<QString, QVariantMap> FakeInterfaceList;
typedef QMap<QDBusObjectPath, FakeInterfaceList> FakeManagedObjects;

class FakeBluez : public QDBusVirtualObject
{
    Q_OBJECT
public:
    static constexpr const char *AdapterPath = "/org/bluez/hci0";
    static constexpr const char *DevicePath = "/org/bluez/hci0/dev_00_00_5E_00_53_01";

    // An adapter (powered or not) with GattManager1 and LEAdvertisingManager1
    void addAdapter(bool powered);
    void setPowered(bool powered, bool signal);
    void addDevice(bool connected, bool servicesResolved, const QString &alias);
    void setDevice(bool connected, bool servicesResolved, bool signal);
    void removeAll();
    void setActiveInstances(int n);
    void setFailGetManagedObjects(bool fail);
    void setFailRegisterAdvertisement(bool fail);

    // Signals as BlueZ sends them, from the fake's connection
    void emitPropertiesChanged(const QString &path, const QString &iface, const QVariantMap &changed,
                               const QStringList &invalidated = {});
    void emitInterfacesAdded(const QString &path);
    void emitInterfacesRemoved(const QString &path, const QStringList &interfaces);
    void removeObject(const QString &path);

    int calls(const QString &member) const;
    void resetCalls();

    QString introspect(const QString &path) const override;
    bool handleMessage(const QDBusMessage &message, const QDBusConnection &connection) override;

    QDBusConnection *connection = nullptr;

private:
    mutable QMutex mMutex;
    FakeManagedObjects mObjects;
    QMap<QString, int> mCalls;
    int mActiveInstances = 1;
    bool mFailGet = false;
    bool mFailRegisterAdvertisement = false;
};

// The thread both fakes live in, with their connections and bus names
class FakePeers : public QObject
{
    Q_OBJECT
public:
    FakePeers();
    ~FakePeers() override;

    FakeLipstick *lipstick() const { return mLipstick; }
    FakeBluez *bluez() const { return mBluez; }

    // Take / drop the bus names (lipstick or bluetoothd starting / exiting)
    bool startLipstick();
    void stopLipstick();
    bool startBluez();
    void stopBluez();

private:
    QThread mThread;
    FakeLipstick *mLipstick = nullptr;
    FakeBluez *mBluez = nullptr;
    QDBusConnection mLipstickBus;
    QDBusConnection mBluezBus;
};

#endif // FAKEPEERS_H
