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
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
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
// The newest messages of a conversation shown (version 4, <ms>)
static const int MaxMessages = 6;
// The app icons' size the phone renders (pixels)
static const int IconSize = 48;

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
QByteArray NotificationService::capabilities()
{
    return QByteArray("<capabilities><version>4</version>"
        "<actions>dismiss,reply,open,mute,answer,reject,silence,end,found</actions>"
        "<replies>1</replies>"
        // Version 3: smart replies (<sr>), the phone's own actions (<ax>, fed
        // back as ty=app), find my phone (feedback ty=findphone, <findphone>
        // from the phone)
        "<smartreplies>5</smartreplies><appactions>3</appactions><findphone>1</findphone>"
        // Version 4: conversations (<th>, <ms>), importance and alert flags
        // (<pr>, <oa>, <og>), categories (<ca>, vb=alarm), app icons (<ic>,
        // written to the icon characteristic; the cache token changes when the
        // watch lost them)
        "<threads>1</threads><messages>") + QByteArray::number(MaxMessages) + "</messages>"
        "<importance>1</importance><alarm>1</alarm>"
        "<icons>" + QByteArray::number(IconSize) + "</icons>"
        "<iconcache>" + iconCacheToken().toLatin1() + "</iconcache>"
        "</capabilities>";
}

// The newest messages of a conversation kept, and their longest text
static const int MaxMessageChars = 300;

// <ms><m><s>SENDER</s><t>TEXT</t></m>...</ms>: the newest MaxMessages
static QList<QPair<QString, QString>> readMessages(QXmlStreamReader &reader)
{
    QList<QPair<QString, QString>> messages;
    while(reader.readNextStartElement()) {
        if(reader.name() != QLatin1String("m")) {
            reader.skipCurrentElement();
            continue;
        }
        QString sender, text;
        while(reader.readNextStartElement()) {
            if(reader.name() == QLatin1String("s"))
                sender = reader.readElementText().simplified();
            else if(reader.name() == QLatin1String("t"))
                text = reader.readElementText().trimmed();
            else
                reader.skipCurrentElement();
        }
        if(text.length() > MaxMessageChars)
            text = text.left(MaxMessageChars - 1) + QChar(0x2026);
        if(!text.isEmpty())
            messages << qMakePair(sender, text);
    }
    while(messages.size() > MaxMessages)
        messages.removeFirst();
    return messages;
}

PhoneNotification PhoneNotification::parse(QXmlStreamReader &reader)
{
    PhoneNotification n;
    while(reader.readNextStartElement()) {
        const auto name = reader.name();
        if(name == QLatin1String("pn")) n.packageName = reader.readElementText();
        else if(name == QLatin1String("id")) n.id = reader.readElementText().toInt();
        else if(name == QLatin1String("an")) n.appName = reader.readElementText();
        else if(name == QLatin1String("ai")) n.appIcon = reader.readElementText();
        else if(name == QLatin1String("su")) n.summary = reader.readElementText();
        else if(name == QLatin1String("bo")) n.body = reader.readElementText();
        else if(name == QLatin1String("vb")) n.vibrate = reader.readElementText();
        else if(name == QLatin1String("ac"))
            n.phoneActions = reader.readElementText().split(',', Qt::SkipEmptyParts);
        else if(name == QLatin1String("rs"))
            n.resync = reader.readElementText().trimmed() == "1";
        else if(name == QLatin1String("ax"))
            n.appActions = readTextList(reader, "a", MaxAppActions, 40);
        else if(name == QLatin1String("sr"))
            n.smartReplies = readTextList(reader, "r", MaxSmartReplies, 120);
        else if(name == QLatin1String("th"))
            n.thread = reader.readElementText().trimmed().left(64);
        else if(name == QLatin1String("ms"))
            n.messages = readMessages(reader);
        else if(name == QLatin1String("pr"))
            n.importance = qBound(0, reader.readElementText().trimmed().toInt(), 5);
        else if(name == QLatin1String("oa"))
            n.onlyAlertOnce = reader.readElementText().trimmed() == "1";
        else if(name == QLatin1String("og"))
            n.ongoing = reader.readElementText().trimmed() == "1";
        else if(name == QLatin1String("ca"))
            n.category = reader.readElementText().trimmed().left(32);
        else if(name == QLatin1String("ic"))
            n.iconHash = reader.readElementText().trimmed();
        else reader.skipCurrentElement();
    }
    for(QString &a : n.phoneActions)
        a = a.trimmed();
    return n;
}

// A conversation: one line per message, with its sender when the messages
// are not all from the one the card is about (a group, or replies sent
// from the phone)
QString PhoneNotification::shownBody() const
{
    if(messages.isEmpty())
        return body;
    bool oneSender = true;
    for(const auto &m : messages) {
        if(!m.first.isEmpty() && m.first != summary)
            oneSender = false;
    }
    QStringList lines;
    for(const auto &m : messages)
        lines << ((oneSender || m.first.isEmpty()) ? m.second : m.first + QStringLiteral(": ") + m.second);
    return lines.join(QChar('\n'));
}

QString PhoneNotification::previewBody() const
{
    if(messages.isEmpty())
        return body;
    const auto &m = messages.last();
    return (m.first.isEmpty() || m.first == summary) ? m.second : m.first + QStringLiteral(": ") + m.second;
}

PostPlan PostPlan::decide(const PhoneNotification &n, bool exists, bool contentChanged, bool doNotDisturb)
{
    PostPlan plan;
    // A banner unless it was sent again after a reconnection, Android calls
    // it unimportant (min, low: silent like on Wear OS), or Do Not Disturb
    // is on (alarms and the find-my-watch alert still come through)
    const bool alarm = n.vibrate == QLatin1String("alarm");
    const bool findWatch = n.phoneActions.contains("found");
    const bool dndSilenced = doNotDisturb && !alarm && !findWatch;
    plan.preview = !n.resync && !(n.importance > 0 && n.importance <= 2) && !dndSilenced;
    plan.vibrate = plan.preview && n.vibrate != QLatin1String("none");

    if(!exists) {
        plan.kind = New;
    } else if(n.isCall() || !plan.preview || !contentChanged || n.onlyAlertOnce || n.ongoing) {
        // Calls keep their card (and their ringing), whatever changes; the
        // rest only alerts again for new content
        plan.kind = Update;
        if(!n.isCall()) {
            plan.preview = false;
            plan.vibrate = false;
        }
    } else {
        plan.kind = Repost;
    }
    return plan;
}

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
        // The phone's write woke the watch: stay out of autosleep until the
        // notification is with lipstick (chunks of one notification extend
        // it; the hold ends by itself 5 s after the last chunk)
        WakeHold::hold();

        mService->setMtu(optionMtu(options));
        mReader.addData(value);

        if(value.endsWith("</insert>")) {
            if(mReader.readNextStartElement() && mReader.name() == QLatin1String("insert")) {
                const PhoneNotification n = PhoneNotification::parse(mReader);
                mReader.clear();
                mService->post(n);
            } else
                mReader.clear();
        } else if(value.endsWith("</removed>")) {
            int id = 0;
            if(mReader.readNextStartElement() && mReader.name() == QLatin1String("removed")) {
                while(mReader.readNextStartElement()) {
                    if(mReader.name() == QLatin1String("id")) id = mReader.readElementText().toInt();
                    else mReader.skipCurrentElement();
                }
                mReader.clear();
                mService->removed(id);
            } else
                mReader.clear();
        } else if(value.endsWith("</findphone>")) {
            // Version 3: <findphone>stop</findphone>, the phone stopped
            // ringing (found there, or its time ran out)
            QString state;
            if(mReader.readNextStartElement() && mReader.name() == QLatin1String("findphone"))
                state = mReader.readElementText().trimmed();
            mReader.clear();
            if(state == "stop")
                mService->phoneStoppedRinging();
        }

        // Posted (or closed): a short grace for lipstick's feedback (ngfd)
        // and mce's display handling to take their own wakelocks; a card
        // closed a moment later (see removed()) keeps the watch up until then
        if(value.endsWith("</insert>"))
            WakeHold::release(WakeHold::GRACE_NOTIFICATION_MS);
        else if(value.endsWith("</removed>"))
            WakeHold::release(NotificationService::CloseDelayMs + WakeHold::GRACE_SHORT_MS);
    }

private:
    NotificationService *mService;
    QXmlStreamReader mReader;
};

/*
 * Version 4, phone to watch: an app's icon, sent once per package and icon
 * (its hash) before the notifications that use it, in as many writes as
 * needed:
 *   <icon><pn>PACKAGE</pn><h>HASH</h><d>BASE64 PNG</d></icon>
 * A small icon (an alpha mask: the launcher tints it like its own icons).
 */
class NotificationsIconChrc : public Characteristic
{
public:
    NotificationsIconChrc(QDBusConnection bus, unsigned int index, NotificationService *service)
      : Characteristic(bus, index, NOTI_ICON_UUID, {"encrypt-authenticated-write"}, service, service), mService(service) {}

public slots:
    void WriteValue(QByteArray value, QVariantMap options)
    {
        WakeHoldScope wake;
        mService->setMtu(optionMtu(options));
        mBuffer += value;
        if(mBuffer.size() > 64 * 1024) {
            qWarning() << "Notification icon without an end, dropped";
            mBuffer.clear();
            return;
        }
        if(!value.endsWith("</icon>"))
            return;
        QXmlStreamReader reader(mBuffer);
        mBuffer.clear();
        QString packageName, hash;
        QByteArray data;
        if(reader.readNextStartElement() && reader.name() == QLatin1String("icon")) {
            while(reader.readNextStartElement()) {
                if(reader.name() == QLatin1String("pn")) packageName = reader.readElementText().trimmed();
                else if(reader.name() == QLatin1String("h")) hash = reader.readElementText().trimmed();
                else if(reader.name() == QLatin1String("d")) data = reader.readElementText().toLatin1();
                else reader.skipCurrentElement();
            }
        }
        if(!NotificationService::storeIcon(packageName, hash, data))
            qWarning() << "Notification icon for" << packageName << "not stored";
    }

private:
    NotificationService *mService;
    QByteArray mBuffer;
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
        QByteArray value(NotificationService::capabilities());
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
    connect(service, &NotificationService::phoneConnectedChanged, this, &NotificationsDBusApi::PhoneConnectedChanged);
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
    addCharacteristic(new NotificationsIconChrc(bus, 4, this));

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
        // What was dismissed meanwhile, once the phone has set itself up
        else if(!mQueuedDismissals.isEmpty())
            QTimer::singleShot(1500, this, &NotificationService::flushQueuedDismissals);
        emit phoneConnectedChanged(notifying);
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
    QHash<uint, QString> threads;
    QHash<uint, uint> contents;
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
        threads.insert(lipstickId, hints.value(ThreadHint).toString());
        contents.insert(lipstickId, contentHash(summary, body));
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
        remember(it.value(), phoneActions.value(it.value()), callNotify.value(it.value()),
                 threads.value(it.value()), contents.value(it.value()));
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
    // A dismiss waits for the phone (at most an hour, the newest 64): it
    // would otherwise come back with the phone's resync after a reconnection
    if(type == QLatin1String("dismiss") && !mFeedback->notifying()) {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        mQueuedDismissals << qMakePair(phoneId, now);
        while(mQueuedDismissals.size() > 64)
            mQueuedDismissals.removeFirst();
        mDismissedAway.insert(phoneId, now);
        qInfo() << "Feedback to the phone: dismiss for" << phoneId << "waits for the phone";
        return;
    }
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
    // What went to the phone, without the text of a reply
    qInfo() << "Feedback to the phone:" << type << "for" << phoneId << (index >= 0 ? QString("index %1").arg(index) : QString())
            << (mFeedback->notifying() ? "" : "(no phone subscribed)");
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
                                   const QList<QVariant> &callNotify, const QString &thread,
                                   uint content)
{
    mPhoneActions.insert(notificationId, phoneActions);
    if(callNotify.isEmpty())
        mCallNotify.remove(notificationId);
    else
        mCallNotify.insert(notificationId, callNotify);
    const QString oldThread = mThreadOf.value(notificationId);
    if(!oldThread.isEmpty() && oldThread != thread && mThreads.value(oldThread) == notificationId)
        mThreads.remove(oldThread);
    if(thread.isEmpty()) {
        mThreadOf.remove(notificationId);
    } else {
        mThreadOf.insert(notificationId, thread);
        mThreads.insert(thread, notificationId);
    }
    mContent.insert(notificationId, content);
}

void NotificationService::forget(uint notificationId)
{
    mPhoneActions.remove(notificationId);
    mCallNotify.remove(notificationId);
    mTimestamps.remove(notificationId);
    mContent.remove(notificationId);
    mPendingClose.remove(notificationId);
    const QString thread = mThreadOf.take(notificationId);
    if(!thread.isEmpty() && mThreads.value(thread) == notificationId)
        mThreads.remove(thread);
}

QString NotificationService::callTimestamp(uint notificationId) const
{
    if(!notificationId || !mCallNotify.contains(notificationId))
        return QString();
    return mCallNotify.value(notificationId)[6].toMap().value("x-nemo-timestamp").toString();
}

uint NotificationService::contentHash(const QString &summary, const QString &body)
{
    return qHash(summary + QChar(0x1f) + body);
}

void NotificationService::post(const PhoneNotification &n)
{
    const int id = n.id;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for(auto it = mDismissedAway.begin(); it != mDismissedAway.end(); ) {
        if(now - it.value() > 3600 * 1000)
            it = mDismissedAway.erase(it);
        else
            ++it;
    }
    // Dismissed on the watch while the phone was away: its resync does not
    // bring it back (the queued dismiss removes it on the phone)
    if(n.resync && mDismissedAway.contains(id)) {
        qInfo() << "Phone notification" << id << "was dismissed on the watch, not shown again";
        return;
    }
    if(!n.resync)
        mDismissedAway.remove(id);

    // The card: by the phone's id, else (version 4) by its conversation,
    // which the phone posts under a new id for each new message
    uint replacesId = mKnownAndroidNotifs.value(id, 0);
    if(!replacesId && !n.thread.isEmpty()) {
        replacesId = mThreads.value(n.thread, 0);
        if(replacesId) {
            // The conversation's card follows the new id; the removal of
            // the old id that follows must not close it
            for(auto it = mKnownAndroidNotifs.begin(); it != mKnownAndroidNotifs.end(); ) {
                if(it.value() == replacesId)
                    it = mKnownAndroidNotifs.erase(it);
                else
                    ++it;
            }
            mKnownAndroidNotifs.insert(id, replacesId);
        }
    }
    // A card about to close (removed a moment ago) stays: re-posted
    mPendingClose.remove(replacesId);

    const QString shownBody = n.shownBody();
    const uint content = contentHash(n.summary, shownBody);
    const bool contentChanged = !replacesId || mContent.value(replacesId) != content;
    const PostPlan plan = PostPlan::decide(n, replacesId != 0, contentChanged, mDoNotDisturb);
    const bool isCall = n.isCall();

    QVariantMap hints;
    // Ours, by the phone's id: found again after a btsyncd restart or a
    // reboot (lipstick keeps its notifications and their hints)
    hints.insert(OwnerHint, Owner);
    hints.insert(PhoneIdHint, QString::number(id));
    if(!n.thread.isEmpty())
        hints.insert(ThreadHint, n.thread);
    if(!n.category.isEmpty())
        hints.insert(CategoryHint, n.category);

    if(plan.preview) {
        hints.insert("x-nemo-preview-body", n.previewBody());
        hints.insert("x-nemo-preview-summary", n.summary);
    }
    // No banner (lipstick shows none without preview summary and body), no
    // vibration: sent again after a reconnect, unimportant, Do Not Disturb,
    // or an update
    if(!plan.vibrate) {
        hints.insert("x-nemo-feedback", "notif_silent");
        hints.insert("x-nemo-feedback-suppressed", true);
    } else if(n.vibrate.isNull()) // for backwards compatibility
        hints.insert("x-nemo-feedback", "notif_strong");
    else if(n.vibrate == QLatin1String("normal"))
        hints.insert("x-nemo-feedback", "notif_normal");
    else if(n.vibrate == QLatin1String("ringtone"))
        hints.insert("x-nemo-feedback", "ringtone");
    else if(n.vibrate == QLatin1String("alarm"))
        // ngfd's alarm pattern, repeated until the alarm is acted on or gone
        hints.insert("x-nemo-feedback", "alarm");
    else
        hints.insert("x-nemo-feedback", "notif_strong");

    hints.insert("urgency", 3);

    // A ringing call keeps its arrival time through updates (e.g. silencing
    // it), and so does a card updated in place without news: lipstick would
    // stamp an update as new and move it to the top of the notifications
    if(isCall) {
        QString stamp = callTimestamp(replacesId);
        if(stamp.isEmpty())
            stamp = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
        hints.insert("x-nemo-timestamp", stamp);
    } else if(plan.kind == PostPlan::Update && (n.resync || !contentChanged || n.ongoing || n.onlyAlertOnce)) {
        const QString stamp = timestamp(replacesId);
        if(!stamp.isEmpty())
            hints.insert("x-nemo-timestamp", stamp);
    }

    // Freedesktop actions: identifier, label pairs. The launcher shows them
    // as buttons; lipstick reports a press through ActionInvoked.
    QStringList actions;
    for(const QString &key : n.phoneActions) {
        if(key.isEmpty() || key == "dismiss")
            continue;
        actions << key << actionLabel(key);
    }
    // The phone's own actions (Archive, Mark as read, Snooze...), by index;
    // the labels also go in a hint for the launcher
    for(int i = 0; i < n.appActions.size(); i++)
        actions << appActionKey(i) << n.appActions.at(i);
    if(!n.appActions.isEmpty())
        hints.insert("x-asteroid-app-actions", n.appActions.join('\n'));
    // Android's suggested replies, shown before the canned ones
    if(!n.smartReplies.isEmpty())
        hints.insert("x-asteroid-smart-replies", n.smartReplies.join('\n'));
    // The launcher looks for this to know the notification came from the
    // phone (and can be acted on there)
    if(!n.phoneActions.isEmpty() || !n.appActions.isEmpty())
        hints.insert("x-asteroid-phone-actions", n.phoneActions.isEmpty() ? QStringLiteral("dismiss") : n.phoneActions.join(','));

    // The app's icon when the phone sent it (version 4), else its icon name
    QString icon = n.iconHash.isEmpty() ? QString() : iconName(n.packageName, n.iconHash);
    if(icon.isEmpty())
        icon = n.appIcon;

    static QDBusInterface notifyApp(NOTIFICATIONS_SERVICE_NAME, NOTIFICATIONS_PATH_BASE, NOTIFICATIONS_MAIN_IFACE);
    if(plan.kind == PostPlan::Repost) {
        // New content to alert for: the old card goes (closed by us, not a
        // dismiss), a new one comes at the top
        qInfo() << "Phone notification" << id << ": new content for lipstick notification" << replacesId << ", posting it anew";
        mKnownAndroidNotifs.remove(id);
        forget(replacesId);
        mActioned.insert(replacesId);
        notifyApp.call(QDBus::NoBlock, "CloseNotification", replacesId);
        replacesId = 0;
    }

    QList<QVariant> argumentList;
    argumentList << n.appName;
    argumentList << replacesId;
    argumentList << icon;
    argumentList << n.summary;
    argumentList << shownBody;
    argumentList << actions;
    argumentList << hints;
    argumentList << (int) 0;    // timeout

    QDBusMessage reply = notifyApp.callWithArgumentList(QDBus::AutoDetect, "Notify", argumentList);
    if(reply.type() == QDBusMessage::ErrorMessage)
        fprintf(stderr, "NotificationService::post: D-Bus Error: %s\n", reply.errorMessage().toStdString().c_str());
    // lipstick answers 0 when the notification to replace is gone (closed
    // behind our back): post it as a new one
    if(replacesId && reply.type() != QDBusMessage::ErrorMessage
            && reply.arguments().size() > 0 && reply.arguments()[0].toUInt() == 0) {
        qInfo() << "Phone notification" << id << ": lipstick notification" << replacesId << "is gone, posting it again";
        mKnownAndroidNotifs.remove(id);
        forget(replacesId);
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
                mKnownAndroidNotifs.insert(id, lipstickId);
        }
    }
    if(lipstickId) {
        argumentList[1] = lipstickId;
        remember(lipstickId, n.phoneActions, isCall ? argumentList : QList<QVariant>(), n.thread, content);
        // lipstick stamps a notification without a timestamp hint with the
        // current time, also when it replaces one
        const QString stamped = hints.value("x-nemo-timestamp").toString();
        setTimestamp(lipstickId, stamped.isEmpty()
            ? QDateTime::currentDateTimeUtc().toString(Qt::ISODate) : stamped);
    }
}

void NotificationService::removed(int phoneId)
{
    const uint lipstickId = mKnownAndroidNotifs.value(phoneId, 0);
    if(!lipstickId)
        return;
    // Closing it here gives NotificationClosed with reason
    // CloseNotificationCalled: not reported back as a dismiss
    mKnownAndroidNotifs.remove(phoneId);
    if(mThreadOf.contains(lipstickId) && !mCallNotify.contains(lipstickId)) {
        // A conversation (version 4): apps re-post one by removing and
        // posting it again; its card stays if it comes back in a moment
        const int token = ++mCloseToken;
        mPendingClose.insert(lipstickId, token);
        QTimer::singleShot(CloseDelayMs, this, [this, lipstickId, token] {
            if(mPendingClose.value(lipstickId, -1) != token)
                return;
            mPendingClose.remove(lipstickId);
            // Re-posted under another phone id meanwhile: still in use
            for(auto it = mKnownAndroidNotifs.constBegin(); it != mKnownAndroidNotifs.constEnd(); ++it) {
                if(it.value() == lipstickId)
                    return;
            }
            closeNow(lipstickId);
        });
        return;
    }
    closeNow(lipstickId);
}

void NotificationService::closeNow(uint lipstickId)
{
    forget(lipstickId);
    static QDBusInterface notifyApp(NOTIFICATIONS_SERVICE_NAME, NOTIFICATIONS_PATH_BASE, NOTIFICATIONS_MAIN_IFACE);
    QDBusMessage reply = notifyApp.call("CloseNotification", lipstickId);
    if(reply.type() == QDBusMessage::ErrorMessage)
        fprintf(stderr, "NotificationService::removed: D-Bus Error: %s\n", reply.errorMessage().toStdString().c_str());
}

void NotificationService::flushQueuedDismissals()
{
    if(!mFeedback->notifying())
        return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const QList<QPair<int, qint64>> queued = mQueuedDismissals;
    mQueuedDismissals.clear();
    for(const auto &d : queued) {
        if(now - d.second <= 3600 * 1000)
            sendFeedback(d.first, "dismiss");
    }
}

static QString iconCacheDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + "/asteroid-btsyncd/icons";
}

QString NotificationService::iconCacheToken()
{
    const QString dir = iconCacheDir();
    static const QRegularExpression tokenRe("^[0-9a-f]{8,16}$");
    QFile token(dir + "/token");
    if(token.open(QIODevice::ReadOnly)) {
        const QString t = QString::fromLatin1(token.readAll().trimmed());
        if(tokenRe.match(t).hasMatch())
            return t;
    }
    // A new (empty) cache: a new token, so the phone sends its icons again
    QDir().mkpath(dir);
    for(const QFileInfo &f : QDir(dir).entryInfoList({"*.svg"}, QDir::Files))
        QFile::remove(f.absoluteFilePath());
    const QString t = QString::number(QRandomGenerator::global()->generate64(), 16).rightJustified(16, '0');
    QSaveFile out(dir + "/token");
    if(out.open(QIODevice::WriteOnly)) {
        out.write(t.toLatin1());
        out.commit();
    }
    return t;
}

QString NotificationService::iconPath(const QString &packageName, const QString &hash)
{
    static const QRegularExpression packageRe("^[A-Za-z0-9._]{1,128}$"), hashRe("^[0-9a-f]{1,16}$");
    if(!packageRe.match(packageName).hasMatch() || !hashRe.match(hash).hasMatch())
        return QString();
    return iconCacheDir() + "/" + packageName + "-" + hash + ".svg";
}

QString NotificationService::iconName(const QString &packageName, const QString &hash)
{
    const QString path = iconPath(packageName, hash);
    if(path.isEmpty() || !path.startsWith('/') || !QFile::exists(path))
        return QString();
    // The launcher's Icon shows /usr/share/icons/asteroid/NAME.svg: up from
    // there to the root (extra ".." stay at the root), then the cache
    return QStringLiteral("../../../../../../..") + path.left(path.length() - 4);
}

bool NotificationService::storeIcon(const QString &packageName, const QString &hash, const QByteArray &base64Png)
{
    const QString path = iconPath(packageName, hash);
    static const QRegularExpression base64Re("^[A-Za-z0-9+/]+={0,2}$");
    const QByteArray data = base64Png.trimmed();
    if(path.isEmpty() || data.isEmpty() || data.size() > 32 * 1024
            || !base64Re.match(QString::fromLatin1(data)).hasMatch())
        return false;
    if(!QByteArray::fromBase64(data).startsWith("\x89PNG"))
        return false;
    iconCacheToken();   // creates the cache
    // At most 300 icons are kept (an app may use several), the newest
    const QString dir = iconCacheDir();
    const QFileInfoList all = QDir(dir).entryInfoList({"*.svg"}, QDir::Files, QDir::Time);
    for(int i = 299; i < all.size(); i++)
        QFile::remove(all.at(i).absoluteFilePath());
    // An SVG around the PNG: the launcher's icons are SVGs it tints
    QSaveFile out(path);
    if(!out.open(QIODevice::WriteOnly))
        return false;
    out.write("<svg xmlns=\"http://www.w3.org/2000/svg\" xmlns:xlink=\"http://www.w3.org/1999/xlink\" "
              "width=\"48\" height=\"48\" viewBox=\"0 0 48 48\">"
              "<image x=\"0\" y=\"0\" width=\"48\" height=\"48\" xlink:href=\"data:image/png;base64,");
    out.write(data);
    out.write("\"/></svg>\n");
    if(!out.commit())
        return false;
    qInfo() << "Notification icon stored for" << packageName;
    return true;
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
