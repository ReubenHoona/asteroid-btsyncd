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
#include "batterystatus.h"
#include "characteristic.h"
#include "common.h"
#include "messagejoiner.h"
#include "wakehold.h"

#include <MDConfItem>
#include <QCryptographicHash>
#include <QDateTime>
#include <QElapsedTimer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
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
#include <cctype>
#include <utility>

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
        // Each insert's notification key (<ky>), and the phone's whole list after a
        // resync (<sync>): a notification keeps its card, a removed one goes
        "<sync>1</sync>"
        // Notification pictures: the longest side and the largest JPEG taken (<pc> in an
        // insert, feedback ty=picture, the picture characteristic)
        "<pictures>" + QByteArray::number(PictureSide) + "</pictures>"
        "<picturebytes>" + QByteArray::number(MaxPictureBytes) + "</picturebytes>"
        // The user's own messages of a conversation marked (<me>1</me> in an <m>): the
        // full view shows them as Wear OS does, on the other side
        "<me>1</me>"
        // Each message's own picture (<pc> in an <m>), kept while a message shows it
        "<msgpictures>1</msgpictures>"
        "</capabilities>";
}

// The newest messages of a conversation kept, and their longest text
static const int MaxMessageChars = 300;

// <pc><i>ID</i><w>W</w><h>H</h></pc>: a picture, and the size it comes at
static void readPictureRef(QXmlStreamReader &reader, QString &id, int &width, int &height)
{
    while(reader.readNextStartElement()) {
        if(reader.name() == QLatin1String("i"))
            id = reader.readElementText().trimmed();
        else if(reader.name() == QLatin1String("w"))
            width = qBound(0, reader.readElementText().trimmed().toInt(), 4096);
        else if(reader.name() == QLatin1String("h"))
            height = qBound(0, reader.readElementText().trimmed().toInt(), 4096);
        else
            reader.skipCurrentElement();
    }
    if(!NotificationService::isPictureId(id))
        id.clear();
}

// <ms><m><s>SENDER</s><t>TEXT</t><me>1</me><pc>..</pc></m>...</ms>: the newest
// MaxMessages, which are the user's own (<me>1</me>) and their pictures (a message
// with a picture may have no text)
static void readMessages(QXmlStreamReader &reader, PhoneNotification &n)
{
    QList<QPair<QString, QString>> messages;
    QList<bool> own;
    QList<PhoneNotification::MessagePicture> pictures;
    while(reader.readNextStartElement()) {
        if(reader.name() != QLatin1String("m")) {
            reader.skipCurrentElement();
            continue;
        }
        QString sender, text;
        bool me = false;
        PhoneNotification::MessagePicture picture;
        while(reader.readNextStartElement()) {
            if(reader.name() == QLatin1String("s"))
                sender = reader.readElementText().simplified();
            else if(reader.name() == QLatin1String("t"))
                text = reader.readElementText().trimmed();
            else if(reader.name() == QLatin1String("me"))
                me = reader.readElementText().trimmed() == QLatin1String("1");
            else if(reader.name() == QLatin1String("pc"))
                readPictureRef(reader, picture.id, picture.width, picture.height);
            else
                reader.skipCurrentElement();
        }
        if(text.length() > MaxMessageChars)
            text = text.left(MaxMessageChars - 1) + QChar(0x2026);
        if(!text.isEmpty() || !picture.id.isEmpty()) {
            messages << qMakePair(sender, text);
            own << me;
            pictures << picture;
        }
    }
    while(messages.size() > MaxMessages) {
        messages.removeFirst();
        own.removeFirst();
        pictures.removeFirst();
    }
    n.messages = messages;
    n.ownMessages = own;
    n.messagePictures = pictures;
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
        else if(name == QLatin1String("sr")) {
            // Suggested replies: some notification assistants give their emoji as HTML
            // character references (&#x1F603;), which would show as that text
            n.smartReplies = readTextList(reader, "r", MaxSmartReplies, 120);
            for(QString &r : n.smartReplies) {
                int found = 0;
                r = PhoneNotification::decodeCharacterReferences(r, &found);
                if(found)
                    n.smartReplyEntities++;
            }
        }
        else if(name == QLatin1String("th"))
            n.thread = reader.readElementText().trimmed().left(64);
        else if(name == QLatin1String("ky"))
            n.key = reader.readElementText().trimmed().left(64);
        else if(name == QLatin1String("ms"))
            readMessages(reader, n);
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
        else if(name == QLatin1String("pc"))
            // <pc><i>ID</i><w>W</w><h>H</h></pc>: the picture, and its size
            readPictureRef(reader, n.pictureId, n.pictureWidth, n.pictureHeight);
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

QString PhoneNotification::messagesJson() const
{
    if(messages.isEmpty())
        return QString();
    QJsonArray list;
    for(int i = 0; i < messages.size(); i++) {
        QJsonObject m;
        m.insert(QStringLiteral("s"), messages.at(i).first);
        m.insert(QStringLiteral("t"), messages.at(i).second);
        if(ownMessages.value(i, false))
            m.insert(QStringLiteral("me"), true);
        const MessagePicture picture = messagePictures.value(i);
        if(!picture.id.isEmpty()) {
            m.insert(QStringLiteral("p"), NotificationService::picturePath(picture.id));
            if(picture.width > 0 && picture.height > 0) {
                m.insert(QStringLiteral("w"), picture.width);
                m.insert(QStringLiteral("h"), picture.height);
            }
        }
        list.append(m);
    }
    return QString::fromUtf8(QJsonDocument(list).toJson(QJsonDocument::Compact));
}

QStringList PhoneNotification::pictureIds() const
{
    QStringList ids;
    if(!pictureId.isEmpty())
        ids << pictureId;
    for(int i = messagePictures.size() - 1; i >= 0; i--) {
        const QString &id = messagePictures.at(i).id;
        if(!id.isEmpty() && !ids.contains(id))
            ids << id;
    }
    return ids;
}

QString PhoneNotification::newestMessagePicture() const
{
    for(int i = messagePictures.size() - 1; i >= 0; i--) {
        if(!messagePictures.at(i).id.isEmpty())
            return messagePictures.at(i).id;
    }
    return QString();
}

QString PhoneNotification::decodeCharacterReferences(const QString &text, int *found)
{
    static const QRegularExpression ref(QStringLiteral("&#(?:[xX]([0-9A-Fa-f]{1,6})|([0-9]{1,7}));"));
    if(found)
        *found = 0;
    if(!text.contains(QLatin1String("&#")))
        return text;
    QString out;
    int last = 0;
    auto it = ref.globalMatch(text);
    while(it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        bool ok = false;
        const uint cp = m.capturedView(1).isEmpty() ? m.captured(2).toUInt(&ok, 10) : m.captured(1).toUInt(&ok, 16);
        // a character (not a surrogate, not a control character but tab and newline)
        if(!ok || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF) || (cp < 0x20 && cp != 9 && cp != 10))
            continue;
        out += text.mid(last, m.capturedStart() - last);
        const char32_t c = cp;
        out += QString::fromUcs4(&c, 1);
        last = m.capturedEnd();
        if(found)
            (*found)++;
    }
    out += text.mid(last);
    return out;
}

bool PhoneNotification::hasEmoji(const QString &text)
{
    for(const uint cp : text.toUcs4()) {
        if(cp >= 0x1F000 || (cp >= 0x2600 && cp <= 0x27BF) || (cp >= 0x2B00 && cp <= 0x2BFF) || cp == 0x2764)
            return true;
    }
    return false;
}

/*
 * As Wear OS, a call ringing on the phone and an alarm going off on the phone
 * only vibrate on the watch: the phone plays their sound. eos_phone_ringing
 * and eos_phone_alarm are ngfd's ringtone and alarm vibration patterns
 * without sound; ngfd's own ringtone and alarm events, which ring through
 * the watch speaker, stay for the watch's alarms and timers and for the
 * find-my-watch alert (it rings the watch itself, as on Wear OS).
 */
QString PostPlan::feedbackEvent(const PhoneNotification &n)
{
    if(n.vibrate.isNull()) // for backwards compatibility
        return QStringLiteral("notif_strong");
    if(n.vibrate == QLatin1String("normal"))
        return QStringLiteral("notif_normal");
    if(n.vibrate == QLatin1String("ringtone"))
        return n.phoneActions.contains("found") ? QStringLiteral("ringtone") : QStringLiteral("eos_phone_ringing");
    if(n.vibrate == QLatin1String("alarm"))
        // the alarm pattern, repeated until the alarm is acted on or gone
        return QStringLiteral("eos_phone_alarm");
    return QStringLiteral("notif_strong");
}

PostPlan PostPlan::decide(const PhoneNotification &n, bool exists, bool contentChanged, bool doNotDisturb,
                          bool onCharger)
{
    PostPlan plan;
    // A banner unless it was sent again after a reconnection, Android calls
    // it unimportant (min, low: silent like on Wear OS), or Do Not Disturb
    // is on (alarms and the find-my-watch alert still come through)
    const bool alarm = n.vibrate == QLatin1String("alarm");
    const bool findWatch = n.phoneActions.contains("found");
    const bool dndSilenced = doNotDisturb && !alarm && !findWatch;
    // On the charger nothing alerts (Wear OS mutes alerting while power is
    // connected, whatever the notification), but the find-my-watch alert: the
    // user asked the watch to ring
    const bool chargerSilenced = onCharger && !findWatch;
    const bool wouldAlert = !n.resync && !(n.importance > 0 && n.importance <= 2) && !dndSilenced;
    plan.preview = wouldAlert && !chargerSilenced;
    plan.vibrate = plan.preview && n.vibrate != QLatin1String("none");
    if(wouldAlert && chargerSilenced)
        plan.quiet = "on the charger";

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

static int optionOffset(const QVariantMap &options)
{
    return options.value("offset", 0).toInt();
}

// What an insert says, for the log: no text, the title as its length
static QString describe(const PhoneNotification &n, int bytes)
{
    QStringList flags;
    if(n.resync) flags << "resync";
    if(!n.thread.isEmpty()) flags << QString("thread, %1 message(s)%2").arg(n.messages.size())
            .arg(n.ownMessages.contains(true) ? QString(" (%1 own)").arg(n.ownMessages.count(true)) : QString());
    if(n.importance) flags << QString("importance %1").arg(n.importance);
    if(n.onlyAlertOnce) flags << "alert once";
    if(n.ongoing) flags << "ongoing";
    if(!n.category.isEmpty()) flags << "category " + n.category;
    if(!n.iconHash.isEmpty()) flags << "icon";
    if(!n.pictureId.isEmpty()) flags << "picture";
    if(!n.newestMessagePicture().isEmpty()) {
        int pictures = 0;
        for(const auto &p : n.messagePictures)
            pictures += p.id.isEmpty() ? 0 : 1;
        flags << QString("%1 message picture(s)").arg(pictures);
    }
    if(!n.smartReplies.isEmpty()) {
        int emoji = 0;
        for(const QString &r : n.smartReplies)
            emoji += PhoneNotification::hasEmoji(r) ? 1 : 0;
        flags << QString("smart replies %1 (%2 with emoji, %3 with entity text)")
                 .arg(n.smartReplies.size()).arg(emoji).arg(n.smartReplyEntities);
    }
    if(!n.phoneActions.isEmpty()) flags << "actions " + n.phoneActions.join(',');
    if(!n.key.isEmpty()) flags << "key";
    return QString("Phone notification %1: insert (%2 bytes) from %3, title %4 chars, body %5 chars, vibration %6%7")
        .arg(n.id).arg(bytes).arg(n.packageName.isEmpty() ? QStringLiteral("?") : n.packageName)
        .arg(n.summary.size()).arg(n.body.size())
        .arg(n.vibrate.isNull() ? QStringLiteral("default") : n.vibrate)
        .arg(flags.isEmpty() ? QString() : ", " + flags.join(", "));
}

class NotificationsUpdateChrc : public Characteristic
{
public:
    NotificationsUpdateChrc(QDBusConnection bus, unsigned int index, NotificationService *service)
      : Characteristic(bus, index, NOTI_UPD_UUID, {"encrypt-authenticated-write"}, service, service), mService(service),
        mJoiner("Notifications", {"insert", "removed", "findphone", "sync"}, MaxUpdateBytes)
    {
        // A write cut by the disconnection never ends: the next connection starts afresh
        QObject::connect(service, &NotificationService::phoneConnectedChanged, this, [this](bool connected) {
            if(!connected)
                mJoiner.drop("the phone is gone");
        });
    }

    static const int MaxUpdateBytes = 16384;

public slots:
    void WriteValue(QByteArray value, QVariantMap options)
    {
        // The phone's write woke the watch: stay out of autosleep until the
        // notification is with lipstick (chunks of one notification extend
        // it; the hold ends by itself 5 s after the last chunk)
        WakeHold::hold();

        mService->setMtu(optionMtu(options));
        bool posted = false, removed = false;
        for(const auto &message : mJoiner.add(value, optionOffset(options))) {
            QXmlStreamReader reader(message.second);
            if(!reader.readNextStartElement()) {
                qWarning() << "Notifications: a message of" << message.second.size() << "bytes could not be read:" << reader.errorString();
                continue;
            }
            if(message.first == "insert") {
                const PhoneNotification n = PhoneNotification::parse(reader);
                if(reader.hasError()) {
                    qWarning() << "Phone notification" << n.id << ": an insert of" << message.second.size()
                               << "bytes could not be read:" << reader.errorString();
                    continue;
                }
                qInfo().noquote() << describe(n, message.second.size());
                mService->post(n);
                posted = true;
            } else if(message.first == "removed") {
                int id = 0;
                while(reader.readNextStartElement()) {
                    if(reader.name() == QLatin1String("id")) id = reader.readElementText().toInt();
                    else reader.skipCurrentElement();
                }
                qInfo() << "Phone notification" << id << ": removed on the phone";
                mService->removed(id);
                removed = true;
            } else if(message.first == "sync") {
                // <sync><n><id>ID</id><ky>KEY</ky></n>...</sync>: the phone's notifications
                QSet<int> ids;
                QSet<QString> keys;
                while(reader.readNextStartElement()) {
                    if(reader.name() != QLatin1String("n")) {
                        reader.skipCurrentElement();
                        continue;
                    }
                    while(reader.readNextStartElement()) {
                        if(reader.name() == QLatin1String("id")) ids.insert(reader.readElementText().toInt());
                        else if(reader.name() == QLatin1String("ky")) keys.insert(reader.readElementText().trimmed());
                        else reader.skipCurrentElement();
                    }
                }
                if(reader.hasError()) {
                    qWarning() << "Notifications: the phone's list could not be read:" << reader.errorString();
                    continue;
                }
                mService->sync(ids, keys);
            } else if(message.first == "findphone") {
                // Version 3: <findphone>stop</findphone>, the phone stopped
                // ringing (found there, or its time ran out)
                const QString state = reader.readElementText().trimmed();
                qInfo() << "Find phone from the phone:" << state;
                if(state == "stop")
                    mService->phoneStoppedRinging();
            } else {
                qWarning() << "Notifications: unknown message" << message.first << "dropped";
            }
        }

        // Posted (or closed): a short grace for lipstick's feedback (ngfd)
        // and mce's display handling to take their own wakelocks; a card
        // closed a moment later (see removed()) keeps the watch up until then
        if(removed)
            WakeHold::release(NotificationService::CloseDelayMs + WakeHold::GRACE_SHORT_MS);
        else if(posted)
            WakeHold::release(WakeHold::GRACE_NOTIFICATION_MS);
    }

private:
    NotificationService *mService;
    MessageJoiner mJoiner;
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
      : Characteristic(bus, index, NOTI_ICON_UUID, {"encrypt-authenticated-write"}, service, service), mService(service),
        mJoiner("Notification icon", {"icon"}, 64 * 1024)
    {
        QObject::connect(service, &NotificationService::phoneConnectedChanged, this, [this](bool connected) {
            if(!connected)
                mJoiner.drop("the phone is gone");
        });
    }

public slots:
    void WriteValue(QByteArray value, QVariantMap options)
    {
        WakeHoldScope wake;
        mService->setMtu(optionMtu(options));
        for(const auto &message : mJoiner.add(value, optionOffset(options))) {
            if(message.first != "icon") {
                qWarning() << "Notification icon: unknown message" << message.first << "dropped";
                continue;
            }
            QXmlStreamReader reader(message.second);
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
    }

private:
    NotificationService *mService;
    MessageJoiner mJoiner;
};

/*
 * Notification pictures (capabilities <pictures>SIDE</pictures><picturebytes>MAX</picturebytes>),
 * as Wear OS shows a message's picture in its notification:
 *   1. the phone's <insert> names its picture and the size it comes at,
 *      <pc><i>ID</i><w>W</w><h>H</h></pc> (ID: 16 hex digits, a hash of the picture);
 *      the card gets the hints x-asteroid-picture (the file, ~/.cache/asteroid-btsyncd/
 *      pictures/ID.jpg, there or not yet) and x-asteroid-picture-size ("WxH")
 *   2. the watch asks for a picture it does not have: feedback ty=picture, tx=ID
 *      (once a minute at most, again when the phone is back)
 *   3. the phone writes it here, in as many writes as needed:
 *        <pic><id>PHONE ID</id><pc>ID</pc><h>HASH</h><sz>BYTES</sz><d>BASE64 JPEG</d></pic>
 *      HASH: the first 16 hex digits of the JPEG's SHA-256; without <d>: the phone cannot
 *      send it (gone, unreadable, larger than MAX at any quality)
 *   4. the launcher shows it once there (PictureChanged on the session bus API)
 * A picture is kept while a card shows it (also over a reboot: lipstick keeps its cards);
 * the cache keeps at most MaxPictures and MaxPictureCacheBytes.
 */
class NotificationsPictureChrc : public Characteristic
{
public:
    NotificationsPictureChrc(QDBusConnection bus, unsigned int index, NotificationService *service)
      : Characteristic(bus, index, NOTI_PIC_UUID, {"encrypt-authenticated-write"}, service, service), mService(service),
        mJoiner("Notification picture", {"pic"}, 48 * 1024)
    {
        // A picture cut by the disconnection never ends: the watch asks again when the
        // phone is back
        QObject::connect(service, &NotificationService::phoneConnectedChanged, this, [this](bool connected) {
            if(!connected)
                mJoiner.drop("the phone is gone");
        });
    }

public slots:
    void WriteValue(QByteArray value, QVariantMap options)
    {
        // The writes of a picture follow each other: autosleep waits for its end
        WakeHold::hold();
        mService->setMtu(optionMtu(options));
        if(mJoiner.idle())
            mStarted.start();
        bool done = false;
        for(const auto &message : mJoiner.add(value, optionOffset(options))) {
            done = true;
            if(message.first != "pic") {
                qWarning() << "Notification picture: unknown message" << message.first << "dropped";
                continue;
            }
            QXmlStreamReader reader(message.second);
            int phoneId = 0, size = -1;
            QString id, hash;
            QByteArray data;
            bool hasData = false;
            if(reader.readNextStartElement() && reader.name() == QLatin1String("pic")) {
                while(reader.readNextStartElement()) {
                    if(reader.name() == QLatin1String("id")) phoneId = reader.readElementText().toInt();
                    else if(reader.name() == QLatin1String("pc")) id = reader.readElementText().trimmed();
                    else if(reader.name() == QLatin1String("h")) hash = reader.readElementText().trimmed();
                    else if(reader.name() == QLatin1String("sz")) size = reader.readElementText().trimmed().toInt();
                    else if(reader.name() == QLatin1String("d")) {
                        data = reader.readElementText().toLatin1();
                        hasData = true;
                    }
                    else reader.skipCurrentElement();
                }
            }
            if(reader.hasError()) {
                qWarning() << "Notification picture: a message of" << message.second.size() << "bytes could not be read:" << reader.errorString();
                continue;
            }
            mService->pictureArrived(phoneId, id, hash, size, data, hasData, mStarted.isValid() ? mStarted.elapsed() : 0);
        }
        if(done && mJoiner.idle())
            WakeHold::release(WakeHold::GRACE_SHORT_MS);
    }

private:
    NotificationService *mService;
    MessageJoiner mJoiner;
    QElapsedTimer mStarted;
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
      : Characteristic(bus, index, NOTI_RPL_UUID, {"encrypt-authenticated-write"}, service, service), mService(service),
        mJoiner("Canned replies", {"replies"}, 16384)
    {
        QObject::connect(service, &NotificationService::phoneConnectedChanged, this, [this](bool connected) {
            if(!connected)
                mJoiner.drop("the phone is gone");
        });
    }

public slots:
    void WriteValue(QByteArray value, QVariantMap options)
    {
        mService->setMtu(optionMtu(options));
        for(const auto &message : mJoiner.add(value, optionOffset(options))) {
            QXmlStreamReader reader(message.second);
            if(message.first != "replies" || !reader.readNextStartElement())
                continue;
            QStringList replies;
            while(reader.readNextStartElement()) {
                if(reader.name() == QLatin1String("r")) {
                    QString r = reader.readElementText().trimmed();
                    if(!r.isEmpty())
                        replies << r;
                } else
                    reader.skipCurrentElement();
            }
            qInfo() << "Canned replies from the phone:" << replies.size();
            // None given: back to the launcher's defaults
            MDConfItem item("/org/asteroidos/notifications/canned-replies");
            if(replies.isEmpty())
                item.unset();
            else
                item.set(replies);
        }
    }

private:
    NotificationService *mService;
    MessageJoiner mJoiner;
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

int NotificationsFeedbackChrc::chunkSize(int mtu)
{
    return qBound(20, mtu - 3, 509);
}

int NotificationsFeedbackChrc::send(const QByteArray &xml, int mtu)
{
    // One notification carries at most MTU - 3 bytes; the phone joins the
    // pieces until </action>
    const int chunk = chunkSize(mtu);
    int pieces = 0;
    for(int pos = 0; pos < xml.size(); pos += chunk) {
        mValue = xml.mid(pos, chunk);
        emit valueChanged();
        pieces++;
    }
    return pieces;
}

void NotificationsFeedbackChrc::StartNotify()
{
    qInfo() << "Feedback: the phone subscribed";
    mNotifying = true;
    emit notifyingChanged(true);
}

void NotificationsFeedbackChrc::StopNotify()
{
    qInfo() << "Feedback: the phone unsubscribed";
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
    connect(service, &NotificationService::pictureChanged, this, &NotificationsDBusApi::PictureChanged);
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

int NotificationsDBusApi::PictureState(const QString &path)
{
    return mService->pictureState(path);
}

bool NotificationsDBusApi::SetAppNotifications(uint notificationId, bool enabled)
{
    return mService->setAppNotifications(notificationId, enabled);
}

bool NotificationsDBusApi::QuietOnCharger()
{
    return mService->quietOnCharger();
}

NotificationService::NotificationService(int index, QDBusConnection bus, QObject *parent) : Service(bus, index, NOTIF_UUID, parent)
{
    mFeedback = new NotificationsFeedbackChrc(bus, 1, this);
    addCharacteristic(new NotificationsUpdateChrc(bus, 0, this));
    addCharacteristic(mFeedback);
    addCharacteristic(new NotificationsRepliesChrc(bus, 2, this));
    addCharacteristic(new NotificationsCapabilitiesChrc(bus, 3, this));
    addCharacteristic(new NotificationsIconChrc(bus, 4, this));
    addCharacteristic(new NotificationsPictureChrc(bus, 5, this));
    mPicturePrune = new QTimer(this);
    mPicturePrune->setSingleShot(true);
    connect(mPicturePrune, &QTimer::timeout, this, &NotificationService::prunePictures);

    // Quiet on the charger: mce's charger state (it signals only changes)
    mBattery = new BatteryStatus(this);
    connect(mBattery, &BatteryStatus::stateChanged, this, [this] {
        const QString state = mBattery->chargerState();
        if(state == mChargerLogged)
            return;
        mChargerLogged = state;
        qInfo().noquote() << QString("Charger %1: phone notifications %2").arg(state)
                .arg(quietOnCharger() ? "come quietly" : "alert as usual");
    });

    QDBusConnection session = QDBusConnection::sessionBus();
    session.connect(NOTIFICATIONS_SERVICE_NAME, NOTIFICATIONS_PATH_BASE,
        NOTIFICATIONS_MAIN_IFACE, "NotificationClosed", this, SLOT(NotificationClosed(uint, uint)));
    session.connect(NOTIFICATIONS_SERVICE_NAME, NOTIFICATIONS_PATH_BASE,
        NOTIFICATIONS_MAIN_IFACE, "ActionInvoked", this, SLOT(ActionInvoked(uint, QString)));

    // A phone that disconnects (or unsubscribes) is not ringing any more;
    // one that does not stop by itself is stopped after a while
    connect(mFeedback, &NotificationsFeedbackChrc::notifyingChanged, this, [this](bool) {
        updatePhoneConnected(1500);
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
    QHash<uint, QString> keyHashes;
    QHash<uint, uint> contents;
    QHash<uint, QStringList> pictures;
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
        keyHashes.insert(lipstickId, hints.value(KeyHint).toString());
        contents.insert(lipstickId, contentHash(summary, body));
        // its pictures: its own and its messages'
        QStringList cardPictures;
        QStringList paths(hints.value(PictureHint).toString());
        const QJsonArray messageList = QJsonDocument::fromJson(hints.value(MessagesHint).toString().toUtf8()).array();
        for(const QJsonValue &m : messageList)
            paths << m.toObject().value(QStringLiteral("p")).toString();
        for(const QString &p : std::as_const(paths)) {
            const QString picture = QFileInfo(p).completeBaseName();
            if(isPictureId(picture) && p == picturePath(picture) && !cardPictures.contains(picture))
                cardPictures << picture;
        }
        if(!cardPictures.isEmpty())
            pictures.insert(lipstickId, cardPictures);
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
                 threads.value(it.value()), contents.value(it.value()), keyHashes.value(it.value()));
        setTimestamp(it.value(), timestamps.value(it.value()));
    }
    // The pictures of the cards lipstick has; the cache's other files go
    mPictureOf.clear();
    for(auto it = found.constBegin(); it != found.constEnd(); ++it) {
        if(pictures.contains(it.value()))
            mPictureOf.insert(it.value(), pictures.value(it.value()));
    }
    mPicturesKnown = true;
    schedulePicturePrune();
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
    if(type == QLatin1String("dismiss") && !phoneConnected()) {
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
    // What went to the phone: the length of a reply, not its text (dconf
    // /org/asteroidos/btsyncd/log-feedback true, or BTSYNCD_LOG_FEEDBACK=1:
    // the whole XML, text included, for debugging)
    const bool connected = phoneConnected();
    const int pieces = mFeedback->send(xml, mMtu);
    QString line = QStringLiteral("Feedback to the phone: %1 for %2").arg(type).arg(phoneId);
    if(index >= 0)
        line += QStringLiteral(" index %1").arg(index);
    else if(!text.isEmpty())
        line += QStringLiteral(" (text of %1 characters)").arg(text.size());
    line += QStringLiteral(", %1 bytes in %2 notification(s) of up to %3 (ATT MTU %4)")
            .arg(xml.size()).arg(pieces).arg(NotificationsFeedbackChrc::chunkSize(mMtu)).arg(mMtu);
    if(!connected)
        line += QStringLiteral(" (no phone connected)");
    qInfo().noquote() << line;
    if(qEnvironmentVariableIntValue("BTSYNCD_LOG_FEEDBACK") == 1
            || MDConfItem(QStringLiteral("/org/asteroidos/btsyncd/log-feedback")).value(false).toBool())
        qInfo().noquote() << "Feedback XML:" << QString::fromUtf8(xml);
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
    return mFeedback->notifying() && (!mLinkKnown || mLink);
}

void NotificationService::setLinkConnected(bool connected)
{
    if(mLinkKnown && connected == mLink)
        return;
    mLinkKnown = true;
    mLink = connected;
    qInfo() << "Phone link:" << (connected ? "connected" : "disconnected")
            << (mFeedback->notifying() ? "" : "(not subscribed)");
    updatePhoneConnected(PhoneSetupMs);
}

void NotificationService::updatePhoneConnected(int flushDelayMs)
{
    const bool connected = phoneConnected();
    if(connected == mPhoneConnected)
        return;
    mPhoneConnected = connected;
    // A phone that is gone is not ringing any more, and sends no pictures: those still
    // missing are asked for again when it is back (its resync names them)
    if(!connected) {
        setFindingPhone(false);
        const QStringList waiting = mPictureRequested.keys();
        mPictureRequested.clear();
        for(const QString &id : waiting) {
            if(!QFile::exists(picturePath(id)) && pictureShown(id))
                emit pictureChanged(picturePath(id), false);
        }
    }
    // What was dismissed meanwhile, once the phone has set itself up
    else if(!mQueuedDismissals.isEmpty())
        QTimer::singleShot(flushDelayMs, this, &NotificationService::flushQueuedDismissals);
    emit phoneConnectedChanged(connected);
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

    // Wear OS keeps the conversation on the watch (the launcher says
    // "Sent"): the phone's update, with the reply in it, replaces the card in
    // place without alerting, or the phone removes it
    mRepliedAt.insert(notificationId, QDateTime::currentMSecsSinceEpoch());
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

bool NotificationService::setAppNotifications(uint notificationId, bool enabled)
{
    const int id = mKnownAndroidNotifs.key(notificationId, 0);
    if(!id || !mPhoneActions.value(notificationId).contains("mute")) {
        qInfo() << "App notifications" << (enabled ? "on" : "off") << "for" << notificationId
                << ": not a phone notification that offers it";
        return false;
    }
    if(!phoneConnected()) {
        qInfo() << "App notifications" << (enabled ? "on" : "off") << "for" << id << ": no phone connected";
        return false;
    }
    sendFeedback(id, enabled ? QStringLiteral("unmute") : QStringLiteral("mute"));
    return true;
}

bool NotificationService::onCharger() const
{
    if(mChargerOverride >= 0)
        return mChargerOverride == 1;
    return mBattery && mBattery->chargerState() == QLatin1String("on");
}

bool NotificationService::quietOnCharger() const
{
    return onCharger() && MDConfItem(QuietOnChargerKey).value(true).toBool();
}

void NotificationService::remember(uint notificationId, const QStringList &phoneActions,
                                   const QList<QVariant> &callNotify, const QString &thread,
                                   uint content, const QString &key)
{
    const QString oldKey = mKeyOf.value(notificationId);
    if(!oldKey.isEmpty() && oldKey != key && mKeys.value(oldKey) == notificationId)
        mKeys.remove(oldKey);
    if(key.isEmpty()) {
        mKeyOf.remove(notificationId);
    } else {
        mKeyOf.insert(notificationId, key);
        mKeys.insert(key, notificationId);
    }
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
    mRepliedAt.remove(notificationId);
    const QString thread = mThreadOf.take(notificationId);
    if(!thread.isEmpty() && mThreads.value(thread) == notificationId)
        mThreads.remove(thread);
    const QString key = mKeyOf.take(notificationId);
    if(!key.isEmpty() && mKeys.value(key) == notificationId)
        mKeys.remove(key);
    mPostedAt.remove(notificationId);
    if(mPictureOf.remove(notificationId))
        schedulePicturePrune();
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
    // The same phone notification under another phone id (a resync after the phone forgot
    // the id the watch knows it by): its card, updated in place
    if(!replacesId && !n.key.isEmpty()) {
        replacesId = mKeys.value(n.key, 0);
        if(replacesId) {
            qInfo().noquote() << QString("Phone notification %1: the card of the same phone notification is %2").arg(id).arg(replacesId);
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
    PostPlan plan = PostPlan::decide(n, replacesId != 0, contentChanged, mDoNotDisturb || mModeQuiet, quietOnCharger());
    // The update that brings the watch's own reply: in place, quietly
    if(replacesId && mRepliedAt.contains(replacesId)) {
        const qint64 repliedAt = mRepliedAt.take(replacesId);
        if(plan.kind == PostPlan::Repost && QDateTime::currentMSecsSinceEpoch() - repliedAt < ReplyEchoMs) {
            plan.kind = PostPlan::Update;
            plan.preview = false;
            plan.vibrate = false;
        }
    }
    const bool isCall = n.isCall();

    QVariantMap hints;
    // Ours, by the phone's id: found again after a btsyncd restart or a
    // reboot (lipstick keeps its notifications and their hints)
    hints.insert(OwnerHint, Owner);
    hints.insert(PhoneIdHint, QString::number(id));
    if(!n.thread.isEmpty())
        hints.insert(ThreadHint, n.thread);
    if(!n.key.isEmpty())
        hints.insert(KeyHint, n.key);
    if(!n.category.isEmpty())
        hints.insert(CategoryHint, n.category);
    // The conversation, message by message, for the full view (the body has it as lines)
    if(!n.messages.isEmpty())
        hints.insert(MessagesHint, n.messagesJson());
    // Its picture: the file, there or on its way (the launcher shows a placeholder of
    // that size until it is)
    if(!n.pictureId.isEmpty()) {
        hints.insert(PictureHint, picturePath(n.pictureId));
        if(n.pictureWidth > 0 && n.pictureHeight > 0)
            hints.insert(PictureSizeHint, QStringLiteral("%1x%2").arg(n.pictureWidth).arg(n.pictureHeight));
    }

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
    } else
        hints.insert("x-nemo-feedback", PostPlan::feedbackEvent(n));

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
    qInfo().noquote() << QString("Phone notification %1: %2 (lipstick %3, %4, %5)").arg(id)
            .arg(plan.kind == PostPlan::New ? "new card" : plan.kind == PostPlan::Update ? "card updated" : "card posted anew")
            .arg(lipstickId).arg(plan.preview ? "banner" : "no banner")
            .arg(plan.vibrate ? QStringLiteral("vibration")
                              : plan.quiet ? QStringLiteral("quiet: ") + QLatin1String(plan.quiet) : QStringLiteral("quiet"));
    if(!lipstickId)
        qWarning() << "Phone notification" << id << ": lipstick did not take it"
                   << (reply.type() == QDBusMessage::ErrorMessage ? reply.errorMessage() : QString());
    if(lipstickId) {
        argumentList[1] = lipstickId;
        remember(lipstickId, n.phoneActions, isCall ? argumentList : QList<QVariant>(), n.thread, content, n.key);
        QStringList askNow;
        if(!n.pictureId.isEmpty())
            askNow << n.pictureId;
        const QString newest = n.newestMessagePicture();
        if(!newest.isEmpty() && !askNow.contains(newest))
            askNow << newest;
        setPictures(lipstickId, id, n.pictureIds(), askNow);
        mPostedAt.insert(lipstickId, QDateTime::currentMSecsSinceEpoch());
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

void NotificationService::sync(const QSet<int> &phoneIds, const QSet<QString> &keys)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QList<int> stale;
    for(auto it = mKnownAndroidNotifs.constBegin(); it != mKnownAndroidNotifs.constEnd(); ++it) {
        const uint lipstickId = it.value();
        if(phoneIds.contains(it.key()))
            continue;
        const QString key = mKeyOf.value(lipstickId);
        if(!key.isEmpty() && keys.contains(key))
            continue;       // the same notification under another phone id: its next insert matches
        // Not the phone's notification list: a call, find my watch
        if(mCallNotify.contains(lipstickId) || mPhoneActions.value(lipstickId).contains("found"))
            continue;
        // Posted while the list was on its way
        if(now - mPostedAt.value(lipstickId, 0) < mSyncGraceMs)
            continue;
        stale << it.key();
    }
    qInfo().noquote() << QString("Phone notifications: the phone has %1, the watch %2 cards, %3 removed on the phone while it was away")
            .arg(phoneIds.size()).arg(mKnownAndroidNotifs.size()).arg(stale.size());
    for(const int phoneId : stale) {
        qInfo().noquote() << QString("Phone notification %1: not on the phone any more, closed").arg(phoneId);
        removed(phoneId);
    }
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
    if(!phoneConnected())
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

QString NotificationService::pictureDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + "/asteroid-btsyncd/pictures";
}

bool NotificationService::isPictureId(const QString &id)
{
    static const QRegularExpression idRe("^[0-9a-f]{16}$");
    return idRe.match(id).hasMatch();
}

QString NotificationService::picturePath(const QString &id)
{
    return isPictureId(id) ? pictureDir() + "/" + id + ".jpg" : QString();
}

bool NotificationService::storePicture(const QString &id, const QString &hash, int size, const QByteArray &base64Jpeg)
{
    static const QRegularExpression hashRe("^[0-9a-f]{16}$"), base64Re("^[A-Za-z0-9+/]+={0,2}$");
    const QByteArray data = base64Jpeg.trimmed();
    const QString path = picturePath(id);
    if(path.isEmpty() || !hashRe.match(hash).hasMatch() || size <= 0 || size > MaxPictureBytes
            || data.isEmpty() || data.size() > (MaxPictureBytes + 2) / 3 * 4
            || !base64Re.match(QString::fromLatin1(data)).hasMatch()) {
        qWarning().noquote() << "Notification picture" << id << ": not a picture message";
        return false;
    }
    const QByteArray jpeg = QByteArray::fromBase64(data);
    if(jpeg.size() != size) {
        qWarning().noquote() << "Notification picture" << id << ":" << jpeg.size() << "bytes, the phone said" << size;
        return false;
    }
    if(QString::fromLatin1(QCryptographicHash::hash(jpeg, QCryptographicHash::Sha256).toHex().left(16)) != hash) {
        qWarning().noquote() << "Notification picture" << id << ": its hash does not match";
        return false;
    }
    // A whole JPEG: its start and end markers
    if(!jpeg.startsWith("\xff\xd8\xff") || !jpeg.endsWith("\xff\xd9")) {
        qWarning().noquote() << "Notification picture" << id << ": not a whole JPEG";
        return false;
    }
    QDir().mkpath(pictureDir());
    QSaveFile out(path);
    if(!out.open(QIODevice::WriteOnly) || out.write(jpeg) != jpeg.size() || !out.commit()) {
        qWarning().noquote() << "Notification picture" << id << ": cannot be written to" << path;
        return false;
    }
    return true;
}

bool NotificationService::pictureShown(const QString &id) const
{
    for(auto it = mPictureOf.constBegin(); it != mPictureOf.constEnd(); ++it) {
        if(it.value().contains(id))
            return true;
    }
    return false;
}

void NotificationService::setPictures(uint lipstickId, int phoneId, const QStringList &ids, const QStringList &askNow)
{
    const QStringList old = mPictureOf.value(lipstickId);
    if(ids.isEmpty())
        mPictureOf.remove(lipstickId);
    else
        mPictureOf.insert(lipstickId, ids);
    for(const QString &id : old) {
        if(!ids.contains(id)) {
            schedulePicturePrune();
            break;
        }
    }
    // the card's own picture and the newest message's now; an older message's when the
    // launcher shows it (pictureState)
    for(const QString &id : askNow) {
        if(!QFile::exists(picturePath(id)))
            requestPicture(phoneId, id);
    }
}

void NotificationService::requestPicture(int phoneId, const QString &id)
{
    if(!phoneConnected()) {
        qInfo().noquote() << QString("Notification picture %1 of %2: waits for the phone").arg(id).arg(phoneId);
        return;
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if(mPictureRequested.contains(id) && now - mPictureRequested.value(id) < PictureRetryMs)
        return;
    mPictureRequested.insert(id, now);
    mPicturesUnavailable.remove(id);
    qInfo().noquote() << QString("Notification picture %1 of %2: asked for").arg(id).arg(phoneId);
    sendFeedback(phoneId, "picture", id);
    // No answer (the phone's Gadgetbridge without pictures, a lost write): given up
    QTimer::singleShot(mPictureTimeoutMs, this, [this, id, now] {
        if(mPictureRequested.value(id, -1) != now || QFile::exists(picturePath(id)))
            return;
        pictureUnavailable(id, "no answer from the phone");
    });
}

void NotificationService::pictureUnavailable(const QString &id, const char *why)
{
    qInfo().noquote() << QString("Notification picture %1: not shown, %2").arg(id).arg(why);
    mPicturesUnavailable.insert(id);
    if(pictureShown(id))
        emit pictureChanged(picturePath(id), false);
}

void NotificationService::pictureArrived(int phoneId, const QString &id, const QString &hash, int size,
                                         const QByteArray &base64Jpeg, bool hasData, qint64 elapsedMs)
{
    if(!isPictureId(id)) {
        qWarning() << "Notification picture for" << phoneId << "without an id, dropped";
        return;
    }
    const qint64 requestedAt = mPictureRequested.value(id, 0);
    if(!hasData) {
        mPictureRequested.remove(id);
        pictureUnavailable(id, "the phone cannot send it");
        return;
    }
    // The card went meanwhile: nothing to show it in
    if(!pictureShown(id)) {
        qInfo().noquote() << QString("Notification picture %1 of %2: no card shows it any more, dropped").arg(id).arg(phoneId);
        return;
    }
    if(!storePicture(id, hash, size, base64Jpeg)) {
        // Broken on its way: asked for once more, then given up
        mPictureRequested.remove(id);
        const int failures = mPictureFailures.value(id, 0) + 1;
        mPictureFailures.insert(id, failures);
        if(failures < 2)
            requestPicture(phoneId, id);
        else
            pictureUnavailable(id, "it came broken twice");
        return;
    }
    mPictureRequested.remove(id);
    mPictureFailures.remove(id);
    mPicturesUnavailable.remove(id);
    qInfo().noquote() << QString("Notification picture %1 of %2: %3 bytes stored, %4 ms of writes%5")
            .arg(id).arg(phoneId).arg(size).arg(elapsedMs)
            .arg(requestedAt ? QString(", %1 ms after it was asked for").arg(QDateTime::currentMSecsSinceEpoch() - requestedAt) : QString());
    emit pictureChanged(picturePath(id), true);
    schedulePicturePrune();
}

int NotificationService::pictureState(const QString &path)
{
    const QString id = QFileInfo(path).completeBaseName();
    if(!isPictureId(id) || picturePath(id) != path)
        return PictureUnavailable;
    if(QFile::exists(path))
        return PictureReady;
    if(!pictureShown(id) || mPicturesUnavailable.contains(id) || !phoneConnected())
        return PictureUnavailable;
    // a picture no one asked for yet (an older message's, a cache that lost it): asked for
    // now, from the phone notification of a card that shows it
    if(!mPictureRequested.contains(id)) {
        for(auto it = mPictureOf.constBegin(); it != mPictureOf.constEnd(); ++it) {
            const int phoneId = it.value().contains(id) ? mKnownAndroidNotifs.key(it.key(), 0) : 0;
            if(phoneId) {
                qInfo().noquote() << QString("Notification picture %1 of %2: shown, so asked for").arg(id).arg(phoneId);
                requestPicture(phoneId, id);
                break;
            }
        }
    }
    return PicturePending;
}

void NotificationService::schedulePicturePrune()
{
    if(mPicturePrune)
        mPicturePrune->start(mPicturePruneDelayMs);
}

void NotificationService::prunePictures()
{
    QSet<QString> shown;
    for(const QStringList &ids : std::as_const(mPictureOf)) {
        for(const QString &id : ids)
            shown.insert(id);
    }
    // What no card shows is forgotten
    for(auto it = mPictureRequested.begin(); it != mPictureRequested.end(); ) {
        if(!shown.contains(it.key())) it = mPictureRequested.erase(it); else ++it;
    }
    for(auto it = mPictureFailures.begin(); it != mPictureFailures.end(); ) {
        if(!shown.contains(it.key())) it = mPictureFailures.erase(it); else ++it;
    }
    for(auto it = mPicturesUnavailable.begin(); it != mPicturesUnavailable.end(); ) {
        if(!shown.contains(*it)) it = mPicturesUnavailable.erase(it); else ++it;
    }
    // Newest first: the files no card shows go (once lipstick's list is known), then the
    // oldest beyond the limits
    const QFileInfoList files = QDir(pictureDir()).entryInfoList(QDir::Files, QDir::Time);
    QFileInfoList kept;
    int removed = 0;
    for(const QFileInfo &f : files) {
        const QString id = f.completeBaseName();
        const bool picture = f.suffix() == QLatin1String("jpg") && isPictureId(id);
        if(mPicturesKnown && (!picture || !shown.contains(id))) {
            QFile::remove(f.absoluteFilePath());
            removed++;
        } else if(picture) {
            kept << f;
        }
    }
    qint64 bytes = 0;
    int left = 0;
    for(int i = 0; i < kept.size(); i++) {
        bytes += kept.at(i).size();
        if(i < mMaxPictures && bytes <= mMaxPictureCacheBytes) {
            left++;
            continue;
        }
        const QString id = kept.at(i).completeBaseName();
        QFile::remove(kept.at(i).absoluteFilePath());
        removed++;
        if(shown.contains(id)) {
            mPicturesUnavailable.insert(id);
            emit pictureChanged(picturePath(id), false);
        }
    }
    if(removed)
        qInfo() << "Notification pictures:" << removed << "deleted," << left << "kept";
}
