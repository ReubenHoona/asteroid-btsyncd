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
#include "wakehold.h"

#include <MDConfItem>
#include <QDateTime>
#include <QDBusArgument>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusServiceWatcher>
#include <QDebug>
#include <QTimer>
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
    if(key == "silence") return QObject::tr("Silence");
    if(key == "end")    return QObject::tr("End call");
    if(key == "found")  return QObject::tr("Found it");
    return key;
}

// At most this many of the phone's own actions (<ax>) and smart replies (<sr>)
static const int MaxAppActions = 3;
static const int MaxSmartReplies = 5;

// The texts of the <item> children of the element the reader is on, trimmed,
// newlines folded, at most max of them, each at most maxLength characters
static QStringList readTextList(QXmlStreamReader &reader, const char *item, int max, int maxLength)
{
    QStringList list;
    while(reader.readNextStartElement()) {
        if(reader.name() == QLatin1String(item)) {
            QString text = reader.readElementText().simplified();
            if(text.length() > maxLength)
                text = text.left(maxLength - 1) + QChar(0x2026);
            if(!text.isEmpty() && list.size() < max)
                list << text;
        } else {
            reader.skipCurrentElement();
        }
    }
    return list;
}

// What this watch understands, for the capabilities characteristic
static const char CAPABILITIES[] =
    "<capabilities><version>3</version>"
    "<actions>dismiss,reply,open,mute,answer,reject,silence,end,found</actions>"
    "<replies>1</replies>"
    // Version 3: smart replies (<sr>), the phone's own actions (<ax>, fed
    // back as ty=app), find my phone (feedback ty=findphone, <findphone>
    // from the phone)
    "<smartreplies>5</smartreplies><appactions>3</appactions><findphone>1</findphone>"
    "</capabilities>";

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
        QStringList phoneActions, appActions, smartReplies;
        bool resync = false;

        // The phone's write woke the watch: stay out of autosleep until the
        // notification is with lipstick (chunks of one notification extend
        // it; the hold ends by itself 5 s after the last chunk)
        WakeHold::hold();

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
                    else if(mReader.name() == "rs")
                        resync = mReader.readElementText().trimmed() == "1";
                    else if(mReader.name() == "ax")
                        appActions = readTextList(mReader, "a", MaxAppActions, 40);
                    else if(mReader.name() == "sr")
                        smartReplies = readTextList(mReader, "r", MaxSmartReplies, 120);
                    else mReader.skipCurrentElement();
                }
                mReader.clear();

                replacesId = mService->mKnownAndroidNotifs.value(id, 0);

                QVariantMap hints;
                // Ours, by the phone's id: found again after a btsyncd restart
                // or a reboot (lipstick keeps its notifications and their hints)
                hints.insert(NotificationService::OwnerHint, NotificationService::Owner);
                hints.insert(NotificationService::PhoneIdHint, QString::number(id));

                if (resync) {
                    // <rs>1</rs>: the phone sends what it already had after a
                    // reconnect. No preview banner (lipstick shows none without
                    // preview summary and body), no vibration, and an update
                    // keeps its place in the list
                    hints.insert("x-nemo-feedback", "notif_silent");
                    hints.insert("x-nemo-feedback-suppressed", true);
                    const QString timestamp = mService->timestamp(replacesId);
                    if(!timestamp.isEmpty())
                        hints.insert("x-nemo-timestamp", timestamp);
                } else {
                    hints.insert("x-nemo-preview-body", body);
                    hints.insert("x-nemo-preview-summary", summary);
                }

                if (resync) {
                    // silent, above
                } else if (vibrate == nullptr) // for backwards compatibility
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

                // A ringing call keeps its arrival time through updates (e.g.
                // silencing it): lipstick would stamp an update as new and
                // move it to the top of the notifications
                const bool isCall = phoneActions.contains("answer") || phoneActions.contains("reject");
                if(isCall) {
                    QString timestamp = mService->callTimestamp(replacesId);
                    if(timestamp.isEmpty())
                        timestamp = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
                    hints.insert("x-nemo-timestamp", timestamp);
                }

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
                // The phone's own actions (Archive, Mark as read, Snooze...),
                // by index; the labels also go in a hint for the launcher
                for(int i = 0; i < appActions.size(); i++)
                    actions << NotificationService::appActionKey(i) << appActions.at(i);
                if(!appActions.isEmpty())
                    hints.insert("x-asteroid-app-actions", appActions.join('\n'));
                // Android's suggested replies, shown before the canned ones
                if(!smartReplies.isEmpty())
                    hints.insert("x-asteroid-smart-replies", smartReplies.join('\n'));
                // The launcher looks for this to know the notification came
                // from the phone (and can be acted on there)
                if(!phoneActions.isEmpty() || !appActions.isEmpty())
                    hints.insert("x-asteroid-phone-actions", phoneActions.isEmpty() ? QStringLiteral("dismiss") : phoneActions.join(','));

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
                // lipstick answers 0 when the notification to replace is gone
                // (closed behind our back): post it as a new one
                if(replacesId && reply.type() != QDBusMessage::ErrorMessage
                        && reply.arguments().size() > 0 && reply.arguments()[0].toUInt() == 0) {
                    qInfo() << "Phone notification" << id << ": lipstick notification" << replacesId << "is gone, posting it again";
                    mService->mKnownAndroidNotifs.remove(id);
                    mService->forget(replacesId);
                    replacesId = 0;
                    argumentList[1] = 0u;
                    if(!isCall)
                        hints.remove("x-nemo-timestamp");
                    argumentList[6] = hints;
                    reply = notifyApp.callWithArgumentList(QDBus::AutoDetect, "Notify", argumentList);
                }

                uint lipstickId = replacesId;
                if(!replacesId) {
                    if(reply.arguments().size() > 0) {
                        lipstickId = reply.arguments()[0].toUInt();
                        if(lipstickId)
                            mService->mKnownAndroidNotifs.insert(id, lipstickId);
                    }
                }
                if(lipstickId) {
                    argumentList[1] = lipstickId;
                    mService->remember(lipstickId, phoneActions,
                                       isCall ? argumentList : QList<QVariant>());
                    // lipstick stamps a notification without a timestamp hint
                    // with the current time, also when it replaces one
                    const QString stamped = hints.value("x-nemo-timestamp").toString();
                    mService->setTimestamp(lipstickId, stamped.isEmpty()
                        ? QDateTime::currentDateTimeUtc().toString(Qt::ISODate) : stamped);
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
                    mService->forget(replacesId);

                    QList<QVariant> argumentList;
                    argumentList << replacesId;

                    static QDBusInterface notifyApp(NOTIFICATIONS_SERVICE_NAME, NOTIFICATIONS_PATH_BASE, NOTIFICATIONS_MAIN_IFACE);
                    QDBusMessage reply = notifyApp.callWithArgumentList(QDBus::AutoDetect, "CloseNotification", argumentList);
                    if(reply.type() == QDBusMessage::ErrorMessage)
                        fprintf(stderr, "NotificationsUpdateChrc::writeValue: D-Bus Error: %s\n", reply.errorMessage().toStdString().c_str());
                }
            } else
                mReader.clear();
        } else if(value.endsWith("</findphone>")) {
            // Version 3: <findphone>stop</findphone>, the phone stopped
            // ringing (found there, or its time ran out)
            QString state;
            if(mReader.readNextStartElement() && mReader.name() == "findphone")
                state = mReader.readElementText().trimmed();
            mReader.clear();
            if(state == "stop")
                mService->phoneStoppedRinging();
        }

        // Posted (or closed): a short grace for lipstick's feedback (ngfd)
        // and mce's display handling to take their own wakelocks
        if(value.endsWith("</insert>"))
            WakeHold::release(WakeHold::GRACE_NOTIFICATION_MS);
        else if(value.endsWith("</removed>"))
            WakeHold::release(WakeHold::GRACE_SHORT_MS);
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

void NotificationsFeedbackChrc::StartNotify()
{
    mNotifying = true;
    emit notifyingChanged(true);
}

void NotificationsFeedbackChrc::StopNotify()
{
    mNotifying = false;
    emit notifyingChanged(false);
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
    connect(service, &NotificationService::findPhoneChanged, this, &NotificationsDBusApi::FindPhoneChanged);
}

bool NotificationsDBusApi::Reply(uint notificationId, const QString &text)
{
    return mService->reply(notificationId, text);
}

bool NotificationsDBusApi::Silence(uint notificationId)
{
    return mService->silence(notificationId);
}

bool NotificationsDBusApi::FindPhone(bool start)
{
    return mService->findPhone(start);
}

bool NotificationsDBusApi::FindingPhone()
{
    return mService->findingPhone();
}

bool NotificationsDBusApi::PhoneConnected()
{
    return mService->phoneConnected();
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

    // A phone that disconnects (or unsubscribes) is not ringing any more;
    // one that does not stop by itself is stopped after a while
    connect(mFeedback, &NotificationsFeedbackChrc::notifyingChanged, this, [this](bool notifying) {
        if(!notifying)
            setFindingPhone(false);
    });
    mFindPhoneTimeout = new QTimer(this);
    mFindPhoneTimeout->setSingleShot(true);
    mFindPhoneTimeout->setInterval(120 * 1000);
    connect(mFindPhoneTimeout, &QTimer::timeout, this, [this] { findPhone(false); });

    // The launcher's replies come in on the session bus
    if(!session.registerObject(BTSYNCD_NOTIF_PATH, new NotificationsDBusApi(this),
                               QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllSignals))
        fprintf(stderr, "NotificationService: cannot register %s on the session bus\n", BTSYNCD_NOTIF_PATH);
    if(!session.registerService(SERVICE_NAME))
        fprintf(stderr, "NotificationService: cannot own %s on the session bus\n", SERVICE_NAME);

    // The phone notifications lipstick still shows (it keeps them over a
    // reboot): now, and whenever lipstick's notification service comes back
    mLipstickWatcher = new QDBusServiceWatcher(NOTIFICATIONS_SERVICE_NAME, session,
                                               QDBusServiceWatcher::WatchForRegistration, this);
    connect(mLipstickWatcher, &QDBusServiceWatcher::serviceRegistered, this, [this] { restoreKnown(); });
    restoreKnown();
}

void NotificationService::restoreKnown()
{
    QDBusMessage call = QDBusMessage::createMethodCall(NOTIFICATIONS_SERVICE_NAME, NOTIFICATIONS_PATH_BASE,
                                                       NOTIFICATIONS_MAIN_IFACE, "GetNotifications");
    call << QString(Owner);
    // Blocking but short: a phone notification arriving before this is
    // done would otherwise be posted twice
    const QDBusMessage reply = QDBusConnection::sessionBus().call(call, QDBus::Block, 3000);
    if(reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty()) {
        qInfo() << "Notifications: no list from lipstick yet:" << reply.errorMessage();
        return;
    }

    // a(susssasa{sv}i): app name, id, icon, summary, body, actions, hints, timeout
    QHash<int, uint> found;
    QHash<uint, QStringList> phoneActions;
    QHash<uint, QList<QVariant>> callNotify;
    QHash<uint, QString> timestamps;
    QList<uint> duplicates;
    int listed = 0;
    const QDBusArgument arg = reply.arguments().at(0).value<QDBusArgument>();
    arg.beginArray();
    while(!arg.atEnd()) {
        QString appName, appIcon, summary, body;
        uint lipstickId = 0;
        QStringList actions;
        QVariantMap hints;
        int timeout = 0;
        arg.beginStructure();
        arg >> appName >> lipstickId >> appIcon >> summary >> body >> actions >> hints >> timeout;
        arg.endStructure();
        listed++;
        bool ok = false;
        const int phoneId = hints.value(PhoneIdHint).toString().toInt(&ok);
        if(!ok || !phoneId || !lipstickId)
            continue;
        if(found.contains(phoneId)) {
            // Posted twice (by an older btsyncd): keep the newer one
            const uint other = found.value(phoneId);
            duplicates << qMin(other, lipstickId);
            if(lipstickId < other)
                continue;
        }
        found.insert(phoneId, lipstickId);
        const QStringList keys = hints.value("x-asteroid-phone-actions").toString().split(',', Qt::SkipEmptyParts);
        phoneActions.insert(lipstickId, keys);
        if(keys.contains("answer") || keys.contains("reject"))
            callNotify.insert(lipstickId, QList<QVariant>() << appName << lipstickId << appIcon << summary
                                                            << body << actions << hints << timeout);
        timestamps.insert(lipstickId, hints.value("x-nemo-timestamp").toString());
    }
    arg.endArray();

    // lipstick is right about what exists: forget what it no longer has
    for(auto it = mKnownAndroidNotifs.begin(); it != mKnownAndroidNotifs.end(); ) {
        if(found.value(it.key(), 0) != it.value()) {
            forget(it.value());
            it = mKnownAndroidNotifs.erase(it);
        } else {
            ++it;
        }
    }
    for(auto it = found.constBegin(); it != found.constEnd(); ++it) {
        mKnownAndroidNotifs.insert(it.key(), it.value());
        remember(it.value(), phoneActions.value(it.value()), callNotify.value(it.value()));
        setTimestamp(it.value(), timestamps.value(it.value()));
    }
    static QDBusInterface notifyApp(NOTIFICATIONS_SERVICE_NAME, NOTIFICATIONS_PATH_BASE, NOTIFICATIONS_MAIN_IFACE);
    for(uint dup : duplicates) {
        if(!found.values().contains(dup))
            notifyApp.call(QDBus::NoBlock, "CloseNotification", dup);
    }
    qInfo() << "Notifications: lipstick has" << found.size() << "phone notifications (of" << listed
            << "of ours)" << (duplicates.isEmpty() ? "" : "and duplicates, closed:") << duplicates;
}

QString NotificationService::timestamp(uint notificationId) const
{
    return notificationId ? mTimestamps.value(notificationId) : QString();
}

void NotificationService::setTimestamp(uint notificationId, const QString &timestamp)
{
    if(notificationId && !timestamp.isEmpty())
        mTimestamps.insert(notificationId, timestamp);
}

void NotificationService::setMtu(int mtu)
{
    if(mtu > 0)
        mMtu = mtu;
}

void NotificationService::sendFeedback(int phoneId, const QString &type, const QString &text, int index)
{
    QByteArray xml;
    QXmlStreamWriter w(&xml);
    w.writeStartElement("action");
    w.writeTextElement("id", QString::number(phoneId));
    w.writeTextElement("ty", type);
    if(!text.isEmpty())
        w.writeTextElement("tx", text);
    if(index >= 0)
        w.writeTextElement("ix", QString::number(index));
    w.writeEndElement();
    mFeedback->send(xml, mMtu);
}

QString NotificationService::appActionKey(int index)
{
    return QStringLiteral("app-") + QString::number(index);
}

int NotificationService::appActionIndex(const QString &key)
{
    if(!key.startsWith(QLatin1String("app-")))
        return -1;
    bool ok = false;
    const int index = key.mid(4).toInt(&ok);
    return ok && index >= 0 && index < MaxAppActions ? index : -1;
}

bool NotificationService::phoneConnected() const
{
    return mFeedback->notifying();
}

bool NotificationService::findPhone(bool start)
{
    if(start && !phoneConnected()) {
        qInfo() << "Find phone: no phone connected";
        setFindingPhone(false);
        return false;
    }
    // A stop also goes out when the watch thinks the phone is quiet: the
    // phone may still ring from an earlier start
    if(phoneConnected())
        sendFeedback(0, "findphone", start ? QStringLiteral("start") : QStringLiteral("stop"));
    qInfo() << "Find phone:" << (start ? "start" : "stop");
    setFindingPhone(start);
    return true;
}

void NotificationService::phoneStoppedRinging()
{
    qInfo() << "Find phone: the phone stopped ringing";
    setFindingPhone(false);
}

void NotificationService::setFindingPhone(bool ringing)
{
    if(ringing)
        mFindPhoneTimeout->start();
    else
        mFindPhoneTimeout->stop();
    if(mFindingPhone == ringing)
        return;
    mFindingPhone = ringing;
    emit findPhoneChanged(ringing);
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
    forget(notificationId);
    static QDBusInterface notifyApp(NOTIFICATIONS_SERVICE_NAME, NOTIFICATIONS_PATH_BASE, NOTIFICATIONS_MAIN_IFACE);
    notifyApp.call(QDBus::NoBlock, "CloseNotification", notificationId);
    return true;
}

bool NotificationService::silence(uint notificationId)
{
    int id = mKnownAndroidNotifs.key(notificationId, 0);
    if(!id)
        return false;

    if(mPhoneActions.value(notificationId).contains("silence"))
        sendFeedback(id, "silence");

    // Update the notification to suppress its feedback: lipstick then stops
    // the ringtone vibration. An update shows no new preview.
    if(mCallNotify.contains(notificationId)) {
        QList<QVariant> args = mCallNotify.value(notificationId);
        QVariantMap hints = args[6].toMap();
        hints.insert("x-nemo-feedback-suppressed", true);
        args[6] = hints;
        mCallNotify.insert(notificationId, args);

        static QDBusInterface notifyApp(NOTIFICATIONS_SERVICE_NAME, NOTIFICATIONS_PATH_BASE, NOTIFICATIONS_MAIN_IFACE);
        notifyApp.callWithArgumentList(QDBus::NoBlock, "Notify", args);
    }
    return true;
}

void NotificationService::remember(uint notificationId, const QStringList &phoneActions,
                                   const QList<QVariant> &callNotify)
{
    mPhoneActions.insert(notificationId, phoneActions);
    if(callNotify.isEmpty())
        mCallNotify.remove(notificationId);
    else
        mCallNotify.insert(notificationId, callNotify);
}

void NotificationService::forget(uint notificationId)
{
    mPhoneActions.remove(notificationId);
    mCallNotify.remove(notificationId);
    mTimestamps.remove(notificationId);
}

QString NotificationService::callTimestamp(uint notificationId) const
{
    if(!notificationId || !mCallNotify.contains(notificationId))
        return QString();
    return mCallNotify.value(notificationId)[6].toMap().value("x-nemo-timestamp").toString();
}

void NotificationService::ActionInvoked(uint replacesId, const QString &actionKey)
{
    int id = mKnownAndroidNotifs.key(replacesId, 0);
    // A reply needs its text: it comes through Reply(), not as a bare action
    if(!id || actionKey == "reply" || actionKey == "default")
        return;
    const int appIndex = appActionIndex(actionKey);
    if(appIndex >= 0)
        sendFeedback(id, "app", QString(), appIndex);
    else
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
    forget(replacesId);
}
