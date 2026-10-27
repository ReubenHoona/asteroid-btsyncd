#include "fakepeers.h"

#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusVariant>
#include <QMutexLocker>
#include <QDBusPendingCall>
#include <QTest>

bool initTestBus()
{
    const QByteArray session = qgetenv("DBUS_SESSION_BUS_ADDRESS");
    if (session.isEmpty())
        return false;
    // btsyncd's GATT objects and BlueZ calls go to the "system bus": the
    // private test bus, never the host's
    qputenv("DBUS_SYSTEM_BUS_ADDRESS", session);
    return true;
}

bool haveTestBus()
{
    return !qgetenv("DBUS_SESSION_BUS_ADDRESS").isEmpty() && QDBusConnection::sessionBus().isConnected();
}

QDBusMessage callAsPeer(const QDBusMessage &call, int timeoutMs)
{
    QDBusConnection peer = QDBusConnection::connectToBus(QDBusConnection::SessionBus, QStringLiteral("fake-peer"));
    QDBusPendingCall pending = peer.asyncCall(call, timeoutMs);
    (void)QTest::qWaitFor([&pending] { return pending.isFinished(); }, timeoutMs + 1000);
    return pending.reply();
}

QDBusArgument &operator<<(QDBusArgument &arg, const LipNotification &n)
{
    arg.beginStructure();
    arg << n.appName << n.id << n.appIcon << n.summary << n.body << n.actions << n.hints << n.timeout;
    arg.endStructure();
    return arg;
}

const QDBusArgument &operator>>(const QDBusArgument &arg, LipNotification &n)
{
    arg.beginStructure();
    arg >> n.appName >> n.id >> n.appIcon >> n.summary >> n.body >> n.actions >> n.hints >> n.timeout;
    arg.endStructure();
    return arg;
}

/* FakeLipstick */

QList<LipNotification> FakeLipstick::notifyCalls() const
{
    QMutexLocker lock(&mMutex);
    return mNotifyCalls;
}

QList<uint> FakeLipstick::closeCalls() const
{
    QMutexLocker lock(&mMutex);
    return mCloseCalls;
}

int FakeLipstick::getNotificationsCalls() const
{
    QMutexLocker lock(&mMutex);
    return mGetCalls;
}

QMap<uint, LipNotification> FakeLipstick::shown() const
{
    QMutexLocker lock(&mMutex);
    return mShown;
}

void FakeLipstick::reset()
{
    QMutexLocker lock(&mMutex);
    mNotifyCalls.clear();
    mCloseCalls.clear();
    mShown.clear();
    mGetCalls = 0;
    mReplaceGone = false;
}

void FakeLipstick::setReplaceGone(bool gone)
{
    QMutexLocker lock(&mMutex);
    mReplaceGone = gone;
}

void FakeLipstick::setShown(const QList<LipNotification> &list)
{
    QMutexLocker lock(&mMutex);
    mShown.clear();
    for (const LipNotification &n : list) {
        mShown.insert(n.id, n);
        mNextId = qMax(mNextId, n.id + 1);
    }
}

void FakeLipstick::emitClosed(uint id, uint reason)
{
    QDBusMessage signal = QDBusMessage::createSignal("/org/freedesktop/Notifications",
                                                     "org.freedesktop.Notifications", "NotificationClosed");
    signal << id << reason;
    connection->send(signal);
}

void FakeLipstick::emitAction(uint id, const QString &key)
{
    QDBusMessage signal = QDBusMessage::createSignal("/org/freedesktop/Notifications",
                                                     "org.freedesktop.Notifications", "ActionInvoked");
    signal << id << key;
    connection->send(signal);
}

uint FakeLipstick::Notify(const QString &appName, uint replacesId, const QString &appIcon, const QString &summary,
                          const QString &body, const QStringList &actions, const QVariantMap &hints, int timeout)
{
    QMutexLocker lock(&mMutex);
    LipNotification n{appName, replacesId, appIcon, summary, body, actions, hints, timeout};
    mNotifyCalls << n;
    if (replacesId) {
        if (mReplaceGone || !mShown.contains(replacesId))
            return 0;
    } else {
        n.id = mNextId++;
    }
    mShown.insert(n.id, n);
    return n.id;
}

void FakeLipstick::CloseNotification(uint id)
{
    QMutexLocker lock(&mMutex);
    mCloseCalls << id;
    mShown.remove(id);
}

QList<LipNotification> FakeLipstick::GetNotifications(const QString &owner)
{
    QMutexLocker lock(&mMutex);
    mGetCalls++;
    QList<LipNotification> out;
    for (const LipNotification &n : std::as_const(mShown)) {
        if (n.hints.value("x-nemo-owner").toString() == owner)
            out << n;
    }
    return out;
}

/* FakeBluez */

static const char ADAPTER_IFACE_[] = "org.bluez.Adapter1";
static const char GATT_MANAGER_[] = "org.bluez.GattManager1";
static const char ADV_MANAGER_[] = "org.bluez.LEAdvertisingManager1";
static const char DEVICE_IFACE_[] = "org.bluez.Device1";
static const char PROPS_IFACE_[] = "org.freedesktop.DBus.Properties";
static const char OM_IFACE_[] = "org.freedesktop.DBus.ObjectManager";

void FakeBluez::addAdapter(bool powered)
{
    QMutexLocker lock(&mMutex);
    FakeInterfaceList ifaces;
    ifaces.insert(ADAPTER_IFACE_, QVariantMap{{"Powered", powered}, {"Address", "00:00:5E:00:53:00"}});
    ifaces.insert(GATT_MANAGER_, QVariantMap());
    ifaces.insert(ADV_MANAGER_, QVariantMap{{"ActiveInstances", QVariant::fromValue(uchar(mActiveInstances))}});
    mObjects.insert(QDBusObjectPath(AdapterPath), ifaces);
}

void FakeBluez::setPowered(bool powered, bool signal)
{
    {
        QMutexLocker lock(&mMutex);
        mObjects[QDBusObjectPath(AdapterPath)][ADAPTER_IFACE_].insert("Powered", powered);
    }
    if (signal)
        emitPropertiesChanged(AdapterPath, ADAPTER_IFACE_, {{"Powered", powered}});
}

void FakeBluez::addDevice(bool connected, bool servicesResolved, const QString &alias)
{
    QMutexLocker lock(&mMutex);
    FakeInterfaceList ifaces;
    ifaces.insert(DEVICE_IFACE_, QVariantMap{{"Connected", connected}, {"ServicesResolved", servicesResolved},
                                             {"Alias", alias}, {"Address", "00:00:5E:00:53:01"}});
    mObjects.insert(QDBusObjectPath(DevicePath), ifaces);
}

void FakeBluez::setDevice(bool connected, bool servicesResolved, bool signal)
{
    {
        QMutexLocker lock(&mMutex);
        QVariantMap &props = mObjects[QDBusObjectPath(DevicePath)][DEVICE_IFACE_];
        props.insert("Connected", connected);
        props.insert("ServicesResolved", servicesResolved);
    }
    if (signal)
        emitPropertiesChanged(DevicePath, DEVICE_IFACE_, {{"Connected", connected}, {"ServicesResolved", servicesResolved}});
}

void FakeBluez::removeAll()
{
    QMutexLocker lock(&mMutex);
    mObjects.clear();
}

void FakeBluez::setActiveInstances(int n)
{
    QMutexLocker lock(&mMutex);
    mActiveInstances = n;
}

void FakeBluez::setFailGetManagedObjects(bool fail)
{
    QMutexLocker lock(&mMutex);
    mFailGet = fail;
}

void FakeBluez::setFailRegisterAdvertisement(bool fail)
{
    QMutexLocker lock(&mMutex);
    mFailRegisterAdvertisement = fail;
}

void FakeBluez::emitPropertiesChanged(const QString &path, const QString &iface, const QVariantMap &changed,
                                      const QStringList &invalidated)
{
    QDBusMessage signal = QDBusMessage::createSignal(path, PROPS_IFACE_, "PropertiesChanged");
    signal << iface << changed << invalidated;
    connection->send(signal);
}

void FakeBluez::emitInterfacesAdded(const QString &path)
{
    FakeInterfaceList ifaces;
    {
        QMutexLocker lock(&mMutex);
        ifaces = mObjects.value(QDBusObjectPath(path));
    }
    QDBusMessage signal = QDBusMessage::createSignal("/", OM_IFACE_, "InterfacesAdded");
    signal << QVariant::fromValue(QDBusObjectPath(path)) << QVariant::fromValue(ifaces);
    connection->send(signal);
}

void FakeBluez::emitInterfacesRemoved(const QString &path, const QStringList &interfaces)
{
    QDBusMessage signal = QDBusMessage::createSignal("/", OM_IFACE_, "InterfacesRemoved");
    signal << QVariant::fromValue(QDBusObjectPath(path)) << interfaces;
    connection->send(signal);
}

void FakeBluez::removeObject(const QString &path)
{
    QMutexLocker lock(&mMutex);
    mObjects.remove(QDBusObjectPath(path));
}

int FakeBluez::calls(const QString &member) const
{
    QMutexLocker lock(&mMutex);
    return mCalls.value(member);
}

void FakeBluez::resetCalls()
{
    QMutexLocker lock(&mMutex);
    mCalls.clear();
}

QString FakeBluez::introspect(const QString &path) const
{
    if (path == QLatin1String("/")) {
        return QStringLiteral(
            "<interface name=\"org.freedesktop.DBus.ObjectManager\">"
            "<method name=\"GetManagedObjects\"><arg name=\"objects\" type=\"a{oa{sa{sv}}}\" direction=\"out\"/></method>"
            "<signal name=\"InterfacesAdded\"><arg name=\"object\" type=\"o\"/><arg name=\"interfaces\" type=\"a{sa{sv}}\"/></signal>"
            "<signal name=\"InterfacesRemoved\"><arg name=\"object\" type=\"o\"/><arg name=\"interfaces\" type=\"as\"/></signal>"
            "</interface>");
    }
    if (path == QLatin1String(AdapterPath)) {
        return QStringLiteral(
            "<interface name=\"org.bluez.GattManager1\">"
            "<method name=\"RegisterApplication\"><arg name=\"application\" type=\"o\" direction=\"in\"/>"
            "<arg name=\"options\" type=\"a{sv}\" direction=\"in\"/></method>"
            "<method name=\"UnregisterApplication\"><arg name=\"application\" type=\"o\" direction=\"in\"/></method>"
            "</interface>"
            "<interface name=\"org.bluez.LEAdvertisingManager1\">"
            "<method name=\"RegisterAdvertisement\"><arg name=\"advertisement\" type=\"o\" direction=\"in\"/>"
            "<arg name=\"options\" type=\"a{sv}\" direction=\"in\"/></method>"
            "<method name=\"UnregisterAdvertisement\"><arg name=\"service\" type=\"o\" direction=\"in\"/></method>"
            "<property name=\"ActiveInstances\" type=\"y\" access=\"read\"/>"
            "</interface>"
            "<interface name=\"org.bluez.Adapter1\"><property name=\"Powered\" type=\"b\" access=\"readwrite\"/></interface>");
    }
    return QString();
}

bool FakeBluez::handleMessage(const QDBusMessage &message, const QDBusConnection &bus)
{
    const QString path = message.path();
    const QString iface = message.interface();
    const QString member = message.member();

    if (iface == QLatin1String("org.freedesktop.DBus.Introspectable") && member == QLatin1String("Introspect")) {
        const QString xml = QStringLiteral("<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\" "
                                           "\"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n<node>")
                + introspect(path)
                + QStringLiteral("<interface name=\"org.freedesktop.DBus.Introspectable\"><method name=\"Introspect\">"
                                 "<arg name=\"xml_data\" type=\"s\" direction=\"out\"/></method></interface></node>");
        bus.send(message.createReply(xml));
        return true;
    }

    QMutexLocker lock(&mMutex);
    mCalls[member]++;
    if (path == QLatin1String("/") && member == QLatin1String("GetManagedObjects")) {
        if (mFailGet)
            bus.send(message.createErrorReply("org.bluez.Error.Failed", "fake failure"));
        else
            bus.send(message.createReply(QVariant::fromValue(mObjects)));
        return true;
    }
    if (path == QLatin1String(AdapterPath) && mObjects.contains(QDBusObjectPath(AdapterPath))) {
        if (member == QLatin1String("RegisterApplication") || member == QLatin1String("UnregisterAdvertisement")) {
            bus.send(message.createReply());
            return true;
        }
        if (member == QLatin1String("RegisterAdvertisement")) {
            if (mFailRegisterAdvertisement)
                bus.send(message.createErrorReply("org.bluez.Error.Failed", "fake failure"));
            else
                bus.send(message.createReply());
            return true;
        }
        if (iface == QLatin1String(PROPS_IFACE_) && member == QLatin1String("Get")) {
            const QList<QVariant> args = message.arguments();
            if (args.size() == 2 && args[1].toString() == QLatin1String("ActiveInstances")) {
                bus.send(message.createReply(QVariant::fromValue(QDBusVariant(QVariant::fromValue(uchar(mActiveInstances))))));
                return true;
            }
        }
    }
    bus.send(message.createErrorReply("org.freedesktop.DBus.Error.UnknownMethod", "not in the fake: " + member));
    return true;
}

/* FakePeers */

FakePeers::FakePeers()
    : mLipstickBus(QDBusConnection::connectToBus(QDBusConnection::SessionBus, QStringLiteral("fake-lipstick")))
    , mBluezBus(QDBusConnection::connectToBus(QDBusConnection::SessionBus, QStringLiteral("fake-bluez")))
{
    qDBusRegisterMetaType<LipNotification>();
    qDBusRegisterMetaType<QList<LipNotification>>();
    qDBusRegisterMetaType<FakeInterfaceList>();
    qDBusRegisterMetaType<FakeManagedObjects>();

    mThread.setObjectName(QStringLiteral("fake-peers"));
    mThread.start();

    mLipstick = new FakeLipstick;
    mLipstick->connection = &mLipstickBus;
    // Registered before the move: the registration parents helpers to the
    // object, and the move takes them along
    mLipstickBus.registerObject(QStringLiteral("/org/freedesktop/Notifications"), mLipstick,
                                QDBusConnection::ExportAllSlots);
    mLipstick->moveToThread(&mThread);

    mBluez = new FakeBluez;
    mBluez->connection = &mBluezBus;
    mBluezBus.registerVirtualObject(QStringLiteral("/"), mBluez, QDBusConnection::SubPath);
    mBluez->moveToThread(&mThread);
}

FakePeers::~FakePeers()
{
    stopLipstick();
    stopBluez();
    mLipstickBus.unregisterObject(QStringLiteral("/org/freedesktop/Notifications"));
    mBluezBus.unregisterObject(QStringLiteral("/"));
    mThread.quit();
    mThread.wait();
    delete mLipstick;
    delete mBluez;
    QDBusConnection::disconnectFromBus(QStringLiteral("fake-lipstick"));
    QDBusConnection::disconnectFromBus(QStringLiteral("fake-bluez"));
}

bool FakePeers::startLipstick()
{
    return mLipstickBus.registerService(QStringLiteral("org.freedesktop.Notifications"));
}

void FakePeers::stopLipstick()
{
    mLipstickBus.unregisterService(QStringLiteral("org.freedesktop.Notifications"));
}

bool FakePeers::startBluez()
{
    return mBluezBus.registerService(QStringLiteral("org.bluez"));
}

void FakePeers::stopBluez()
{
    mBluezBus.unregisterService(QStringLiteral("org.bluez"));
}
