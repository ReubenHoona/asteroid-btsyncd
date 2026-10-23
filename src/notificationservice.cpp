/*
 * Copyright (C) 2016 - Florent Revest <revestflo@gmail.com>
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

#include "notificationservice.h"
#include "characteristic.h"
#include "common.h"

#include <MDConfItem>
#include <QDBusInterface>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>

// Actions the phone may offer in <ac>, with the button text the watch shows.
// dismiss needs no button: it is the watch's own dismiss.
static QString actionLabel(const QString &key)
{
    if(key == "reply")  return QObject::tr("Reply");
    if(key == "open")   return QObject::tr("Open on phone");
    if(key == "mute")   return QObject::tr("Mute app");
    if(key == "answer") return QObject::tr("Answer");
    if(key == "reject") return QObject::tr("Decline");
    return key;
}

// What this watch understands, for the capabilities characteristic
static const char CAPABILITIES[] =
    "<capabilities><version>1</version>"
    "<actions>dismiss,reply,open,mute,answer,reject</actions>"
    "<replies>1</replies></capabilities>";

static int optionMtu(const QVariantMap &options)
{
    return options.contains("mtu") ? options.value("mtu").toInt() : 0;
}

class NotificationsUpdateChrc : public Characteristic
{
public:
    NotificationsUpdateChrc(QDBusConnection bus, unsigned int index, NotificationService *service)
      : Characteristic(bus, index, NOTI_UPD_UUID, {"encrypt-authenticated-write"}, service, service), mService(service) {}

public slots:
    void WriteValue(QByteArray value, QVariantMap options)
    {
        int id = 0;
        uint replacesId;
        QString packageName, appName, appIcon, summary, body, vibrate = nullptr;
        QStringList phoneActions;

        mService->setMtu(optionMtu(options));
        mReader.addData(value);

        if(value.endsWith("</insert>")) {
            if(mReader.readNextStartElement() && mReader.name() == "insert") {
                while(mReader.readNextStartElement()) {
                    if(mReader.name() == "pn") packageName = mReader.readElementText();
                    else if(mReader.name() == "id") id = mReader.readElementText().toInt();
                    else if(mReader.name() == "an") appName = mReader.readElementText();
                    else if(mReader.name() == "ai") appIcon = mReader.readElementText();
                    else if(mReader.name() == "su") summary = mReader.readElementText();
                    else if(mReader.name() == "bo") body = mReader.readElementText();
                    else if(mReader.name() == "vb") vibrate = mReader.readElementText();
                    else if(mReader.name() == "ac")
                        phoneActions = mReader.readElementText().split(',', Qt::SkipEmptyParts);
                    else mReader.skipCurrentElement();
                }
                mReader.clear();

                replacesId = mService->mKnownAndroidNotifs.value(id, 0);

                QVariantMap hints;
                hints.insert("x-nemo-preview-body", body);
                hints.insert("x-nemo-preview-summary", summary);

                if (vibrate == nullptr) // for backwards compatibility
                    hints.insert("x-nemo-feedback", "notif_strong");
                else if (vibrate.compare("none") == 0)
                    hints.insert("x-nemo-feedback", "notif_silent");
                else if (vibrate.compare("normal") == 0)
                    hints.insert("x-nemo-feedback", "notif_normal");
                else if (vibrate.compare("strong") == 0)
                    hints.insert("x-nemo-feedback", "notif_strong");
                else if (vibrate.compare("ringtone") == 0)
                    hints.insert("x-nemo-feedback", "ringtone");
                else
                    hints.insert("x-nemo-feedback", "notif_strong");

                hints.insert("urgency", 3);

                // Freedesktop actions: identifier, label pairs. The launcher
                // shows them as buttons; lipstick reports a press through
                // ActionInvoked.
                QStringList actions;
                for(const QString &a : phoneActions) {
                    QString key = a.trimmed();
                    if(key.isEmpty() || key == "dismiss")
                        continue;
                    actions << key << actionLabel(key);
                }
                // The launcher looks for this to know the notification came
                // from the phone (and can be acted on there)
                if(!phoneActions.isEmpty())
                    hints.insert("x-asteroid-phone-actions", phoneActions.join(','));

                QList<QVariant> argumentList;
                argumentList << appName;
                argumentList << replacesId;
                argumentList << appIcon;
                argumentList << summary;
                argumentList << body;
                argumentList << actions;
                argumentList << hints;
                argumentList << (int) 0;    // timeout

                static QDBusInterface notifyApp(NOTIFICATIONS_SERVICE_NAME, NOTIFICATIONS_PATH_BASE, NOTIFICATIONS_MAIN_IFACE);
                QDBusMessage reply = notifyApp.callWithArgumentList(QDBus::AutoDetect, "Notify", argumentList);
                if(reply.type() == QDBusMessage::ErrorMessage) {
                    fprintf(stderr, "NotificationsUpdateChrc::writeValue: D-Bus Error: %s\n", reply.errorMessage().toStdString().c_str());
                }

                if(!replacesId) {
                    if(reply.arguments().size() > 0)
                        mService->mKnownAndroidNotifs.insert(id, reply.arguments()[0].toUInt());
                }
            } else
                mReader.clear();
        } else if(value.endsWith("</removed>")) {
            if(mReader.readNextStartElement() && mReader.name() == "removed") {
                while(mReader.readNextStartElement())
                    if(mReader.name() == "id") id = mReader.readElementText().toInt();
                mReader.clear();

                replacesId = mService->mKnownAndroidNotifs.value(id, 0);
                if(replacesId) {
                    // Closing it here gives NotificationClosed with reason
                    // CloseNotificationCalled: not reported back as a dismiss
                    mService->mKnownAndroidNotifs.remove(id);

                    QList<QVariant> argumentList;
                    argumentList << replacesId;

                    static QDBusInterface notifyApp(NOTIFICATIONS_SERVICE_NAME, NOTIFICATIONS_PATH_BASE, NOTIFICATIONS_MAIN_IFACE);
                    QDBusMessage reply = notifyApp.callWithArgumentList(QDBus::AutoDetect, "CloseNotification", argumentList);
                    if(reply.type() == QDBusMessage::ErrorMessage)
                        fprintf(stderr, "NotificationsUpdateChrc::writeValue: D-Bus Error: %s\n", reply.errorMessage().toStdString().c_str());
                }
            } else
                mReader.clear();
        }
    }

private:
    NotificationService *mService;
    QXmlStreamReader mReader;
};

/*
 * Phone to watch: the canned replies the watch offers, as
 * <replies><r>TEXT</r>...</replies>, in as many writes as needed. Stored in
 * dconf for the launcher.
 */
class NotificationsRepliesChrc : public Characteristic
{
public:
    NotificationsRepliesChrc(QDBusConnection bus, unsigned int index, NotificationService *service)
      : Characteristic(bus, index, NOTI_RPL_UUID, {"encrypt-authenticated-write"}, service, service), mService(service) {}

public slots:
    void WriteValue(QByteArray value, QVariantMap options)
    {
        mService->setMtu(optionMtu(options));
        mReader.addData(value);
        if(!value.endsWith("</replies>"))
            return;

        QStringList replies;
        if(mReader.readNextStartElement() && mReader.name() == "replies") {
            while(mReader.readNextStartElement()) {
                if(mReader.name() == "r") {
                    QString r = mReader.readElementText().trimmed();
                    if(!r.isEmpty())
                        replies << r;
                } else
                    mReader.skipCurrentElement();
            }
            // None given: back to the launcher's defaults
            MDConfItem item("/org/asteroidos/notifications/canned-replies");
            if(replies.isEmpty())
                item.unset();
            else
                item.set(replies);
        }
        mReader.clear();
    }

private:
    NotificationService *mService;
    QXmlStreamReader mReader;
};

/*
 * Phone reads what the watch supports, so it only sends the new elements and
 * expects feedback from watches that have them.
 */
class NotificationsCapabilitiesChrc : public Characteristic
{
public:
    NotificationsCapabilitiesChrc(QDBusConnection bus, unsigned int index, Service *service)
      : Characteristic(bus, index, NOTI_CAP_UUID, {"encrypt-authenticated-read"}, service, service) {}

public slots:
    QByteArray ReadValue(QVariantMap options)
    {
        QByteArray value(CAPABILITIES);
        int offset = options.value("offset", 0).toInt();
        return offset > 0 ? value.mid(offset) : value;
    }
};

NotificationsFeedbackChrc::NotificationsFeedbackChrc(QDBusConnection bus, int index, Service *service)
    : Characteristic(bus, index, NOTI_FDB_UUID, {"encrypt-authenticated-notify"}, service)
{
    connect(this, SIGNAL(valueChanged()), this, SLOT(emitPropertiesChanged()));
}

void NotificationsFeedbackChrc::send(const QByteArray &xml, int mtu)
{
    // One notification carries at most MTU - 3 bytes; the phone joins the
    // pieces until </action>
    int chunk = qBound(20, mtu - 3, 509);
    for(int pos = 0; pos < xml.size(); pos += chunk) {
        mValue = xml.mid(pos, chunk);
        emit valueChanged();
    }
}

void NotificationsFeedbackChrc::emitPropertiesChanged()
{
    QDBusConnection connection = QDBusConnection::systemBus();
    QDBusMessage message = QDBusMessage::createSignal(getPath().path(),
                                                      "org.freedesktop.DBus.Properties",
                                                      "PropertiesChanged");

    QVariantMap changedProperties;
    changedProperties[QStringLiteral("Value")] = QVariant(mValue);

    QList<QVariant> arguments;
    arguments << QVariant(GATT_CHRC_IFACE) << QVariant(changedProperties) << QVariant(QStringList());
    message.setArguments(arguments);

    if (!connection.send(message))
        fprintf(stderr, "NotificationsFeedbackChrc: failed to send the PropertiesChanged signal\n");
}

NotificationsDBusApi::NotificationsDBusApi(NotificationService *service)
    : QObject(service), mService(service)
{
}

bool NotificationsDBusApi::Reply(uint notificationId, const QString &text)
{
    return mService->reply(notificationId, text);
}

NotificationService::NotificationService(int index, QDBusConnection bus, QObject *parent) : Service(bus, index, NOTIF_UUID, parent)
{
    mFeedback = new NotificationsFeedbackChrc(bus, 1, this);
    addCharacteristic(new NotificationsUpdateChrc(bus, 0, this));
    addCharacteristic(mFeedback);
    addCharacteristic(new NotificationsRepliesChrc(bus, 2, this));
    addCharacteristic(new NotificationsCapabilitiesChrc(bus, 3, this));

    QDBusConnection session = QDBusConnection::sessionBus();
    session.connect(NOTIFICATIONS_SERVICE_NAME, NOTIFICATIONS_PATH_BASE,
        NOTIFICATIONS_MAIN_IFACE, "NotificationClosed", this, SLOT(NotificationClosed(uint, uint)));
    session.connect(NOTIFICATIONS_SERVICE_NAME, NOTIFICATIONS_PATH_BASE,
        NOTIFICATIONS_MAIN_IFACE, "ActionInvoked", this, SLOT(ActionInvoked(uint, QString)));

    // The launcher's replies come in on the session bus
    if(!session.registerObject(BTSYNCD_NOTIF_PATH, new NotificationsDBusApi(this), QDBusConnection::ExportAllSlots))
        fprintf(stderr, "NotificationService: cannot register %s on the session bus\n", BTSYNCD_NOTIF_PATH);
    if(!session.registerService(SERVICE_NAME))
        fprintf(stderr, "NotificationService: cannot own %s on the session bus\n", SERVICE_NAME);
}

void NotificationService::setMtu(int mtu)
{
    if(mtu > 0)
        mMtu = mtu;
}

void NotificationService::sendFeedback(int phoneId, const QString &type, const QString &text)
{
    QByteArray xml;
    QXmlStreamWriter w(&xml);
    w.writeStartElement("action");
    w.writeTextElement("id", QString::number(phoneId));
    w.writeTextElement("ty", type);
    if(!text.isEmpty())
        w.writeTextElement("tx", text);
    w.writeEndElement();
    mFeedback->send(xml, mMtu);
}

bool NotificationService::reply(uint notificationId, const QString &text)
{
    int id = mKnownAndroidNotifs.key(notificationId, 0);
    if(!id || text.trimmed().isEmpty())
        return false;

    sendFeedback(id, "reply", text.trimmed());

    // Done with it on the watch; the phone updates or removes its own
    mActioned.insert(notificationId);
    mKnownAndroidNotifs.remove(id);
    static QDBusInterface notifyApp(NOTIFICATIONS_SERVICE_NAME, NOTIFICATIONS_PATH_BASE, NOTIFICATIONS_MAIN_IFACE);
    notifyApp.call(QDBus::NoBlock, "CloseNotification", notificationId);
    return true;
}

void NotificationService::ActionInvoked(uint replacesId, const QString &actionKey)
{
    int id = mKnownAndroidNotifs.key(replacesId, 0);
    // A reply needs its text: it comes through Reply(), not as a bare action
    if(!id || actionKey == "reply" || actionKey == "default")
        return;
    sendFeedback(id, actionKey);
    mActioned.insert(replacesId);
}

void NotificationService::NotificationClosed(uint replacesId, uint reason)
{
    const uint dismissedByUser = 2;  // freedesktop / lipstick close reason
    int id = mKnownAndroidNotifs.key(replacesId, 0);
    if(id) {
        if(reason == dismissedByUser && !mActioned.contains(replacesId))
            sendFeedback(id, "dismiss");
        mKnownAndroidNotifs.remove(id);
    }
    mActioned.remove(replacesId);
}
