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

#include "phonestatusservice.h"
#include "common.h"
#include "notificationservice.h"
#include "wakehold.h"

#include <MDConfItem>
#include <QDateTime>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDebug>
#include <QTimer>
#include <QXmlStreamReader>

class PhoneStatusChrc : public Characteristic
{
public:
    PhoneStatusChrc(QDBusConnection bus, unsigned int index, PhoneStatusService *service)
      : Characteristic(bus, index, PHONE_STATUS_UUID, {"encrypt-authenticated-write"}, service, service), mService(service) {}

public slots:
    void WriteValue(QByteArray value, QVariantMap options)
    {
        Q_UNUSED(options);
        // Woken by the phone: hold until the status is applied
        WakeHoldScope wake;
        mBuffer += value;
        if(mBuffer.size() > 4096) {
            qWarning() << "Phone status without an end, dropped";
            mBuffer.clear();
            return;
        }
        if(!value.endsWith("</ps>"))
            return;
        const QByteArray xml = mBuffer;
        mBuffer.clear();
        mService->applyStatus(xml);
    }

private:
    PhoneStatusService *mService;
    QByteArray mBuffer;
};

class PhoneCapabilitiesChrc : public Characteristic
{
public:
    PhoneCapabilitiesChrc(QDBusConnection bus, unsigned int index, Service *service)
      : Characteristic(bus, index, PHONE_CAP_UUID, {"encrypt-authenticated-read"}, service, service) {}

public slots:
    QByteArray ReadValue(QVariantMap options)
    {
        const QByteArray value = PhoneStatusService::capabilities();
        const int offset = options.value("offset", 0).toInt();
        return offset > 0 ? value.mid(offset) : value;
    }
};

PhoneRequestsChrc::PhoneRequestsChrc(QDBusConnection bus, int index, Service *service)
    : Characteristic(bus, index, PHONE_REQ_UUID, {"encrypt-authenticated-notify"}, service)
{
    connect(this, SIGNAL(valueChanged()), this, SLOT(emitPropertiesChanged()));
}

void PhoneRequestsChrc::send(const QByteArray &xml)
{
    // Short enough for one notification at the smallest MTU... nearly: the
    // phone joins the pieces until </req>
    for(int pos = 0; pos < xml.size(); pos += 20) {
        mValue = xml.mid(pos, 20);
        emit valueChanged();
    }
}

void PhoneRequestsChrc::StartNotify()
{
    mNotifying = true;
    emit notifyingChanged(true);
}

void PhoneRequestsChrc::StopNotify()
{
    mNotifying = false;
    emit notifyingChanged(false);
}

void PhoneRequestsChrc::emitPropertiesChanged()
{
    QDBusMessage message = QDBusMessage::createSignal(getPath().path(), "org.freedesktop.DBus.Properties",
                                                      "PropertiesChanged");
    QVariantMap changedProperties;
    changedProperties[QStringLiteral("Value")] = QVariant(mValue);
    message << QVariant(GATT_CHRC_IFACE) << QVariant(changedProperties) << QVariant(QStringList());
    if(!QDBusConnection::systemBus().send(message))
        fprintf(stderr, "PhoneRequestsChrc: failed to send the PropertiesChanged signal\n");
}

PhoneDBusApi::PhoneDBusApi(PhoneStatusService *service) : QObject(service), mService(service)
{
    connect(service, &PhoneStatusService::connectedChanged, this, &PhoneDBusApi::ConnectedChanged);
    connect(service, &PhoneStatusService::batteryChanged, this, &PhoneDBusApi::BatteryChanged);
    connect(service, &PhoneStatusService::doNotDisturbChanged, this, &PhoneDBusApi::DoNotDisturbChanged);
}

bool PhoneDBusApi::Connected() { return mService->connected(); }
int PhoneDBusApi::BatteryLevel() { return mService->batteryLevel(); }
bool PhoneDBusApi::Charging() { return mService->charging(); }
bool PhoneDBusApi::DoNotDisturb() { return mService->doNotDisturb(); }
bool PhoneDBusApi::SetDoNotDisturb(bool on) { return mService->setDoNotDisturb(on); }

QByteArray PhoneStatusService::capabilities()
{
    return QByteArrayLiteral("<capabilities><version>1</version><battery>1</battery><dnd>1</dnd></capabilities>");
}

PhoneStatusService::PhoneStatusService(int index, QDBusConnection bus, NotificationService *notifications, QObject *parent)
    : Service(bus, index, PHONE_UUID, parent), mNotifications(notifications)
{
    addCharacteristic(new PhoneStatusChrc(bus, 0, this));
    mRequests = new PhoneRequestsChrc(bus, 1, this);
    addCharacteristic(mRequests);
    addCharacteristic(new PhoneCapabilitiesChrc(bus, 2, this));

    // A phone that listens again gets what the user chose meanwhile
    connect(mRequests, &PhoneRequestsChrc::notifyingChanged, this, [this](bool notifying) {
        if(notifying && mDndPending)
            sendDndRequest();
    });

    mDndConf = new MDConfItem(DndKey, this);
    mDnd = mDndConf->value(false).toBool();
    mNotifications->setDoNotDisturb(mDnd);
    connect(mDndConf, &MDConfItem::valueChanged, this, &PhoneStatusService::dndConfChanged);

    mNoticeTimer = new QTimer(this);
    mNoticeTimer->setSingleShot(true);
    mNoticeTimer->setInterval(DisconnectNoticeMs);
    connect(mNoticeTimer, &QTimer::timeout, this, &PhoneStatusService::showDisconnectNotice);
    connect(mNotifications, &NotificationService::phoneConnectedChanged, this, &PhoneStatusService::phoneConnectedChanged);
    mWasConnected = mNotifications->phoneConnected();
    closeStaleNotices();

    QDBusConnection session = QDBusConnection::sessionBus();
    if(!session.registerObject(BTSYNCD_PHONE_PATH, new PhoneDBusApi(this),
                               QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllSignals))
        fprintf(stderr, "PhoneStatusService: cannot register %s on the session bus\n", BTSYNCD_PHONE_PATH);
}

void PhoneStatusService::setDisconnectNoticeMs(int ms)
{
    mNoticeTimer->setInterval(ms);
}

bool PhoneStatusService::connected() const
{
    return mNotifications->phoneConnected();
}

void PhoneStatusService::applyStatus(const QByteArray &xml)
{
    QXmlStreamReader reader(xml);
    if(!reader.readNextStartElement() || reader.name() != QLatin1String("ps"))
        return;
    int level = mBatteryLevel;
    bool charging = mCharging;
    QString dnd;
    while(reader.readNextStartElement()) {
        const auto name = reader.name();
        const QString text = reader.readElementText().trimmed();
        bool ok = false;
        if(name == QLatin1String("v")) {
            const int v = text.toInt(&ok);
            if(ok)
                mPhoneVersion = v;
        } else if(name == QLatin1String("bl")) {
            const int l = text.toInt(&ok);
            if(ok)
                level = qBound(-1, l, 100);
        } else if(name == QLatin1String("bc")) {
            charging = text == "1";
        } else if(name == QLatin1String("dnd")) {
            dnd = text;
        }
    }
    if(level != mBatteryLevel || charging != mCharging) {
        mBatteryLevel = level;
        mCharging = charging;
        qInfo() << "Phone battery:" << level << "%" << (charging ? "charging" : "");
        emit batteryChanged(level, charging);
    }
    if(!dnd.isEmpty()) {
        const bool on = dnd != QLatin1String("off");
        if(mDndPending) {
            // The watch's choice is on its way to the phone (or waits for
            // it): the phone's earlier state does not undo it
            if(on == mDnd || QDateTime::currentMSecsSinceEpoch() - mDndPendingSince > DndPendingMs)
                mDndPending = false;
            else
                return;
        }
        if(on != mDnd)
            qInfo() << "Do Not Disturb from the phone:" << dnd;
        setDnd(on, true);
    }
}

bool PhoneStatusService::setDoNotDisturb(bool on)
{
    setDnd(on, false);
    return mRequests->notifying();
}

void PhoneStatusService::dndConfChanged()
{
    // The launcher's quick panel (or an earlier set of ours, then a no-op)
    setDnd(mDndConf->value(false).toBool(), false);
}

void PhoneStatusService::setDnd(bool on, bool fromPhone)
{
    if(on == mDnd)
        return;
    mDnd = on;
    mNotifications->setDoNotDisturb(on);
    if(mDndConf->value(false).toBool() != on)
        mDndConf->set(on);
    emit doNotDisturbChanged(on);
    if(fromPhone)
        return;
    qInfo() << "Do Not Disturb on the watch:" << (on ? "on" : "off");
    mDndPending = true;
    mDndPendingSince = QDateTime::currentMSecsSinceEpoch();
    sendDndRequest();
}

bool PhoneStatusService::sendDndRequest()
{
    if(!mRequests->notifying())
        return false;
    mDndPendingSince = QDateTime::currentMSecsSinceEpoch();
    mRequests->send(mDnd ? QByteArrayLiteral("<req><dnd>on</dnd></req>") : QByteArrayLiteral("<req><dnd>off</dnd></req>"));
    return true;
}

void PhoneStatusService::phoneConnectedChanged(bool connected)
{
    emit connectedChanged(connected);
    if(connected) {
        mWasConnected = true;
        mNoticeTimer->stop();
        closeDisconnectNotice();
    } else if(mWasConnected) {
        // Gone for a while (not a quick reconnection): a quiet card
        mNoticeTimer->start();
    }
}

void PhoneStatusService::showDisconnectNotice()
{
    if(connected() || mNoticeId)
        return;
    QVariantMap hints;
    hints.insert(NotificationService::OwnerHint, NotificationService::Owner);
    hints.insert("x-asteroid-phone-notice", true);
    // Quiet: no banner, no vibration
    hints.insert("x-nemo-feedback", "notif_silent");
    hints.insert("x-nemo-feedback-suppressed", true);
    hints.insert("urgency", 1);
    QDBusMessage call = QDBusMessage::createMethodCall(NOTIFICATIONS_SERVICE_NAME, NOTIFICATIONS_PATH_BASE,
                                                       NOTIFICATIONS_MAIN_IFACE, "Notify");
    call << QObject::tr("Phone") << 0u << QStringLiteral("ios-phone-portrait")
         << QObject::tr("Phone disconnected")
         << QObject::tr("Notifications from the phone arrive again when it is back in range.")
         << QStringList() << hints << 0;
    const QDBusMessage reply = QDBusConnection::sessionBus().call(call, QDBus::Block, 3000);
    if(reply.type() == QDBusMessage::ReplyMessage && !reply.arguments().isEmpty())
        mNoticeId = reply.arguments().at(0).toUInt();
    qInfo() << "Phone disconnected for" << DisconnectNoticeMs / 1000 << "s: notice" << mNoticeId;
}

void PhoneStatusService::closeDisconnectNotice()
{
    if(!mNoticeId)
        return;
    QDBusMessage call = QDBusMessage::createMethodCall(NOTIFICATIONS_SERVICE_NAME, NOTIFICATIONS_PATH_BASE,
                                                       NOTIFICATIONS_MAIN_IFACE, "CloseNotification");
    call << mNoticeId;
    QDBusConnection::sessionBus().call(call, QDBus::NoBlock);
    mNoticeId = 0;
}

// A notice left by an earlier btsyncd (restarted, or the watch rebooted)
void PhoneStatusService::closeStaleNotices()
{
    QDBusMessage call = QDBusMessage::createMethodCall(NOTIFICATIONS_SERVICE_NAME, NOTIFICATIONS_PATH_BASE,
                                                       NOTIFICATIONS_MAIN_IFACE, "GetNotifications");
    call << QString(NotificationService::Owner);
    const QDBusMessage reply = QDBusConnection::sessionBus().call(call, QDBus::Block, 3000);
    if(reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty())
        return;
    QList<uint> stale;
    const QDBusArgument arg = reply.arguments().at(0).value<QDBusArgument>();
    arg.beginArray();
    while(!arg.atEnd()) {
        QString appName, appIcon, summary, body;
        uint id = 0;
        QStringList actions;
        QVariantMap hints;
        int timeout = 0;
        arg.beginStructure();
        arg >> appName >> id >> appIcon >> summary >> body >> actions >> hints >> timeout;
        arg.endStructure();
        if(hints.value("x-asteroid-phone-notice").toBool())
            stale << id;
    }
    arg.endArray();
    for(uint id : stale) {
        QDBusMessage close = QDBusMessage::createMethodCall(NOTIFICATIONS_SERVICE_NAME, NOTIFICATIONS_PATH_BASE,
                                                            NOTIFICATIONS_MAIN_IFACE, "CloseNotification");
        close << id;
        QDBusConnection::sessionBus().call(close, QDBus::NoBlock);
    }
}
