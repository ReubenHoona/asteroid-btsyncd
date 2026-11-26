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

#ifndef NOTIFICATIONSERVICE_H
#define NOTIFICATIONSERVICE_H

#include <QObject>
#include <QHash>
#include <QList>
#include <QPair>
#include <QSet>
#include <QStringList>
#include <QVariant>

#include "callcontrol.h"
#include "callobserver.h"
#include "service.h"

class BatteryStatus;
class NotificationService;
class QDBusServiceWatcher;
class QTimer;
class QXmlStreamReader;

/*
 * One <insert> from the phone. Version 4 adds the conversation (<th>,
 * <ms>), Android's importance and alert flags (<pr>, <oa>, <og>), its
 * category (<ca>) and the app icon sent before (<ic>).
 */
struct PhoneNotification
{
    int id = 0;
    QString packageName, appName, appIcon, summary, body;
    // null: no <vb> (an old phone), the strong pattern
    QString vibrate;
    QStringList phoneActions, appActions, smartReplies;
    // How many suggested replies came as HTML character references (&#x1F603;: some
    // notification assistants give their emoji so; decoded here)
    int smartReplyEntities = 0;
    bool resync = false;
    // Version 4
    QString thread;
    // The phone notification's identity (a hash of Android's notification key): the
    // same card whatever phone id it comes with
    QString key;
    // <cv>KEY</cv> of a message (CR-52): the key the messages API (the act convs) uses for the
    // conversation, "t<thread id>" or "n<hash>"; "" without or when it is not such a key. The card
    // gets it as the hint x-asteroid-conversation (CR-54)
    QString conversation;
    QList<QPair<QString, QString>> messages;   // sender, text; oldest first
    // Which of the messages are the user's own (<me>1</me> in an <m>, phones that know
    // the watch's <me> capability): the same length as messages
    QList<bool> ownMessages;
    // The picture of each message (<pc> in an <m>, phones that know the watch's <msgpictures>
    // capability), the same length as messages: id "" for a message without
    struct MessagePicture { QString id; int width = 0, height = 0; };
    QList<MessagePicture> messagePictures;
    // Version 5, per message like messages: when it was sent (epoch seconds, 0 unknown, <ts>) and
    // the sender's avatar (<av><i>ID</i><w>W</w><h>H</h></av>, a picture of the picture channel,
    // id "" without)
    QList<qint64> messageTimes;
    QList<MessagePicture> messageAvatars;
    // Every picture the card shows: its own (<pc>) and its messages', newest first
    QStringList pictureIds() const;
    // The newest message's picture id, "" if the messages have none
    QString newestMessagePicture() const;
    int importance = 0;                         // Android's 1 (min) .. 5 (max), 0 unknown
    bool onlyAlertOnce = false;
    bool ongoing = false;
    // Version 5: the sub text (<st>, one line), the reply's input forbids emoji (<ne>1</ne>),
    // a progress bar (<pg>), the phone's "when" in seconds (<wh>, only when the notification
    // shows it), the caller is a starred contact (<sd>1</sd> on a call)
    QString subText;
    bool noEmoji = false;
    struct Progress {
        bool present = false;
        bool indeterminate = false;
        int value = 0, max = 0;
        // "LENGTH:RRGGBB" (or "-" for the theme's colour) per segment, comma separated
        QString segments;
    } progress;
    qint64 when = 0;
    bool starred = false;
    QString category;
    QString iconHash;
    // Its picture (watches with <pictures> in their capabilities): the picture's id and the
    // size it comes at, <pc><i>ID</i><w>W</w><h>H</h></pc>
    QString pictureId;
    int pictureWidth = 0, pictureHeight = 0;

    // Reads the children of the <insert> element the reader is on
    static PhoneNotification parse(QXmlStreamReader &reader);
    bool isCall() const { return phoneActions.contains("answer") || phoneActions.contains("reject"); }
    // The body to show: the conversation's messages when there are some
    QString shownBody() const;
    // What the banner shows: the newest message
    QString previewBody() const;
    // The conversation for the launcher's full view (the x-asteroid-messages hint): a JSON
    // array of {"s": sender, "t": text, "me": true for the user's own, "p"/"w"/"h": its picture,
    // "ts": when (epoch seconds), "a": the sender's avatar file}, "" without messages
    QString messagesJson() const;
    // HTML character references (&#x1F603; &#128515;) in a text, as the characters they are
    static QString decodeCharacterReferences(const QString &text, int *found = nullptr);
    // Whether a text has an emoji (pictographs, dingbats, symbols)
    static bool hasEmoji(const QString &text);
};

/*
 * How btsyncd posts a phone notification:
 *   New      a card the watch does not have yet
 *   Update   replaces its card in place (lipstick neither shows a banner nor
 *            vibrates for a replacement)
 *   Repost   new content for a card the phone wants to alert for again (a new
 *            message in a conversation): the old card goes, a new one comes
 *            at the top, with banner and vibration
 * preview: with a banner; vibrate: with the phone's vibration pattern.
 * onCharger: the watch is on its charger or off the wrist and quiet there (as Wear OS's
 * SysUI, OffBodyAlertingMuter: alerting is muted while power is connected or the watch
 * is off body): no banner, no vibration, but for the find-my-watch alert. quiet (the
 * quietReason given) says why a notification that would have alerted does not (for
 * the log), else nullptr.
 * callAllowed: a call that Do Not Disturb or Bedtime lets through (a starred contact
 * or a repeat caller, as stock's zen policy).
 */
struct PostPlan
{
    enum Kind { New, Update, Repost };
    Kind kind = New;
    bool preview = false;
    bool vibrate = false;
    const char *quiet = nullptr;

    static PostPlan decide(const PhoneNotification &n, bool exists, bool contentChanged, bool doNotDisturb,
                           bool onCharger = false, bool callAllowed = false,
                           const char *quietReason = "on the charger");
    // The ngfd event (lipstick's x-nemo-feedback) of a notification that
    // vibrates, from the phone's <vb>. Stock's sounds: a message plays the message
    // alert, every other notification the system one (events notif_message and
    // notif_default; ngfd mutes their sound while "Mute ring & notifications" is on),
    // a call rings the ringtone (at the Ring volume) when that mute is off (muteRing false),
    // else it only vibrates (eos_phone_ringing), and the find-my-watch alert (the found
    // action) rings find_my_watch, loud whatever the volume and the mute.
    static QString feedbackEvent(const PhoneNotification &n, bool muteRing = true);
    // A message (Android's message category, or a conversation with messages)
    static bool isMessage(const PhoneNotification &n);
};

/*
 * Watch to phone: what the user did with a notification, as XML in
 * notifications of at most MTU - 3 bytes; the phone joins them until the
 * closing tag:
 *   <action><id>ID</id><ty>TYPE</ty><tx>TEXT</tx><ix>INDEX</ix></action>
 * TYPE is dismiss, or one of the actions the phone offered in <ac>; TEXT
 * only for reply. Protocol version 3 adds:
 *   ty=app, ix=N      the phone's own action N of the notification's <ax>
 *   ty=found          the find-my-watch alert was found
 *   ty=findphone, id=0, tx=start|stop   ring the phone / stop ringing it
 * and with notification pictures (<pictures> in the capabilities):
 *   ty=picture, tx=ID the watch has not got the picture the notification's <pc>
 *                     names: the phone writes it to the picture characteristic
 * and ty=unmute: the user turned the notifications of this notification's app
 * on again on the watch (ty=mute turned them off; phones that do not know
 * unmute ignore it)
 * With <ack>1</ack> in the capabilities the phone answers a reply, an app action and
 * "Open on phone" on the notification characteristic:
 *   <ack><id>ID</id><ty>reply|app|open</ty>[<ix>N</ix>]<ok>1|0</ok></ack>
 * (see NotificationService::actionAck); its status says <ak>1</ak> when it does
 * A dismiss while no phone is subscribed waits (version 4) and goes out
 * when a phone subscribes again.
 */
class NotificationsFeedbackChrc : public Characteristic
{
    Q_OBJECT
    Q_PROPERTY(QByteArray Value READ getValue NOTIFY valueChanged)

public:
    NotificationsFeedbackChrc(QDBusConnection bus, int index, Service *service);
    // Returns the number of notifications it took
    int send(const QByteArray &xml, int mtu);
    // The bytes one notification carries for an ATT MTU
    static int chunkSize(int mtu);
    // A phone has subscribed (BlueZ StartNotify): it gets what is sent
    bool notifying() const { return mNotifying; }

public slots:
    void StartNotify() override;
    void StopNotify() override;

signals:
    void valueChanged();
    void notifyingChanged(bool notifying);

private slots:
    void emitPropertiesChanged();

private:
    QByteArray mValue;
    bool mNotifying = false;
    QByteArray getValue() { return mValue; }
};

/*
 * Session bus API for the launcher: org.asteroidos.btsyncd.Notifications on
 * /org/asteroidos/btsyncd/notifications. Reply() sends a typed or canned
 * reply to the notification with the given (lipstick) id.
 */
class NotificationsDBusApi : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", BTSYNCD_NOTIF_IFACE)

public:
    explicit NotificationsDBusApi(NotificationService *service);

public slots:
    bool Reply(uint notificationId, const QString &text);
    /*
     * Silences a ringing call (by lipstick id): the watch stops vibrating,
     * and the phone silences its ringer when it offered "silence". The call
     * keeps ringing until answered, declined or ended.
     */
    bool Silence(uint notificationId);
    /*
     * Find my phone: start (or stop) ringing the phone. Returns false when
     * no phone is subscribed to the feedback (nothing to ring). The phone
     * stops by itself after a while, or when found there: FindPhoneChanged
     * tells (phones with protocol version 3 report the stop).
     */
    bool FindPhone(bool start);
    bool FindingPhone();
    // A phone is connected and subscribed to the feedback
    bool PhoneConnected();
    /*
     * A notification's picture (the file its x-asteroid-picture hint names): 1 here,
     * 0 on its way from the phone, 2 not to be had (the phone cannot send it, the
     * phone is away, no card shows it). PictureChanged says when that changes.
     */
    int PictureState(const QString &path);
    /*
     * The watch's notifications of this notification's app (by lipstick id) on or off, as
     * Wear OS's long press menu ("Notifications": the app, "Get notifications from APP
     * on your watch"): the phone stops (mute) or starts again (unmute) sending them.
     * The card stays. False, and nothing sent, when the card is not the phone's or the
     * phone does not offer it (no "mute" among its actions), or no phone is connected.
     */
    bool SetAppNotifications(uint notificationId, bool enabled);
    // The watch is on its charger and phone notifications come quietly (see PostPlan)
    bool QuietOnCharger();
    /*
     * The phone answers what the watch sends for a reply, an app action and "Open on
     * phone" (ActionResult follows each, within ActionAckMs): false for a phone that does
     * not, which gets "ok" at once, as before.
     */
    bool ActionAcks();

signals:
    /*
     * The result of a reply, app action or "Open on phone" the watch sent for this
     * notification (lipstick id): kind "reply", "app" or "open", the app action's index (else
     * -1), result "ok" (the phone ran it; at once for a phone that does not answer),
     * "failed" (the phone could not, or no phone is connected) or "timeout" (no answer within
     * five seconds).
     */
    void ActionResult(uint notificationId, const QString &kind, int index, const QString &result);
    void FindPhoneChanged(bool ringing);
    // The phone (dis)connected: subscribed to the feedback or not any more
    void PhoneConnectedChanged(bool connected);
    // A notification picture arrived (available), or will not come (not available)
    void PictureChanged(const QString &path, bool available);

private:
    NotificationService *mService;
};

class NotificationService : public Service
{
    Q_OBJECT
public:
    explicit NotificationService(int index, QDBusConnection bus, QObject *parent = 0);

    // Hints on every phone notification btsyncd posts: lipstick's owner
    // (GetNotifications lists by it) and the phone's id
    static constexpr const char *Owner = "asteroid-btsyncd";
    static constexpr const char *OwnerHint = "x-nemo-owner";
    static constexpr const char *PhoneIdHint = "x-asteroid-phone-id";
    // Version 4: the conversation's key, and the Android category
    static constexpr const char *ThreadHint = "x-asteroid-thread";
    // The conversation's key in the messages API (CR-54), from <cv>: the launcher opens the
    // Messages app on that conversation
    static constexpr const char *ConversationHint = "x-asteroid-conversation";
    static constexpr const char *KeyHint = "x-asteroid-phone-key";
    static constexpr const char *CategoryHint = "x-asteroid-category";
    // The conversation's messages for the full view (PhoneNotification::messagesJson)
    static constexpr const char *MessagesHint = "x-asteroid-messages";
    // Version 5: the sub text (a line under the title), no emoji in the reply input, the
    // progress bar (value, maximum, indeterminate, segments "LENGTH:RRGGBB,..." with "-"
    // for the theme's colour), the find-my-watch alert (full screen "Playing sound"), a call
    // that Do Not Disturb or Bedtime lets through ("starred" or "repeat"), and lipstick's
    // resident (the card stays after an action: the phone updates or removes it)
    static constexpr const char *SubTextHint = "x-asteroid-subtext";
    static constexpr const char *NoEmojiHint = "x-asteroid-no-emoji";
    static constexpr const char *ProgressHint = "x-asteroid-progress";
    static constexpr const char *ProgressMaxHint = "x-asteroid-progress-max";
    static constexpr const char *ProgressIndeterminateHint = "x-asteroid-progress-indeterminate";
    static constexpr const char *ProgressSegmentsHint = "x-asteroid-progress-segments";
    static constexpr const char *FindWatchHint = "x-asteroid-find-watch";
    static constexpr const char *CallExceptionHint = "x-asteroid-call-exception";
    static constexpr const char *ResidentHint = "resident";
    // A card the phone removed closes this much later (see removed())
    static const int CloseDelayMs = 1500;
    // The phone's first update of a card within this long after a reply
    // from the watch is the reply itself: no alert for it
    static const int ReplyEchoMs = 60000;

    // Phone notification id -> lipstick notification id
    QHash<int, uint> mKnownAndroidNotifs;

    // Rebuilds mKnownAndroidNotifs (and the actions, call and timestamp
    // memory) from the notifications lipstick shows for us, so the phone's
    // updates and removals still find them after a btsyncd restart or a
    // reboot. Closes duplicates of one phone notification.
    void restoreKnown();

    // When lipstick shows a notification as posted (ISO UTC), "" if unknown
    QString timestamp(uint notificationId) const;
    void setTimestamp(uint notificationId, const QString &timestamp);

    void setMtu(int mtu);

    // A notification from the phone, and its removal
    void post(const PhoneNotification &n);
    void removed(int phoneId);
    // After a resync, every notification the phone has (phone id, key hash): the cards of
    // phone notifications in neither list were removed while the phone was away and go
    // (not calls, find my watch, or cards posted in the last syncGraceMs)
    void sync(const QSet<int> &phoneIds, const QSet<QString> &keys);
    void setSyncGraceMs(int ms) { mSyncGraceMs = ms; }
    static const int SyncGraceMs = 60 * 1000;

    // Do Not Disturb (the watch's, synced with the phone's by the phone
    // status service): phone notifications come without banner or vibration,
    // but alarms and the find-my-watch alert
    void setDoNotDisturb(bool on) { mDoNotDisturb = on; }
    bool doNotDisturb() const { return mDoNotDisturb; }
    // Bedtime or Theater mode (/org/asteroidos/modes/bedtime, theater, the
    // launcher's modes engine): phone notifications as quiet as with DND,
    // as stock (alarms and the find-my-watch alert still come through)
    void setModeQuiet(bool on) { mModeQuiet = on; }
    bool modeQuiet() const { return mModeQuiet; }
    // Which of the modes is on (Bedtime, Theater), for the calls they let through: Do Not
    // Disturb and Bedtime ring for starred contacts and repeat callers (stock's zen policy
    // Policy(120,2,0) and createZenPolicyFor bedtime), Theater for none. A quiet mode that
    // is neither counts as Bedtime.
    void setBedtimeMode(bool on) { mBedtimeMode = on; }
    void setTheaterMode(bool on) { mTheaterMode = on; }
    // A second call from the same caller within this long is a repeat caller (Android's
    // config_zen_repeat_callers_threshold)
    static const int RepeatCallerMs = 15 * 60 * 1000;
    // The reason a ringing call is let through while the watch is quiet ("starred",
    // "repeat"), nullptr when it is not; also remembers the caller (in memory only). newCard:
    // the phone's call card is a new one, whose own state starts afresh.
    const char *callException(const PhoneNotification &n, bool newCard);

    // "Mute ring & notifications" (stock's ringer mode VIBRATE, the default): a call only
    // vibrates, notifications play no sound (ngfd mutes it); off: calls ring and
    // notifications play the message or system sound
    static constexpr const char *MuteRingKey = "/org/asteroidos/sound/mute-ring-notifications";
    static bool muteRingAndNotifications();

    /*
     * The phone answers a reply, an app action and "Open on phone" (<ak>1</ak> in its
     * status, set by the phone status service), and says whether it could do it on the
     * notification characteristic (actionAck). Each of those feedbacks starts a timer of
     * ActionAckMs (no periodic timer); ActionResult is emitted with the answer, or
     * "timeout". A phone that does not answer gets "ok" at once, and no phone at all
     * "failed". The flag is gone when the phone is.
     */
    static const int ActionAckMs = 5000;
    bool phoneAnswers() const { return mPhoneAnswers; }
    void setPhoneAnswers(bool on) { mPhoneAnswers = on; }
    void setActionAckMs(int ms) { mActionAckMs = ms; }
    // What the phone wrote: <ack><id>PHONE ID</id><ty>TYPE</ty><ix>N</ix><ok>1|0</ok></ack>
    void actionAck(int phoneId, const QString &type, int index, bool ok);

    // The protocol version 4 app icon cache (~/.cache/asteroid-btsyncd/icons):
    // its token changes when it starts empty, so the phone sends its icons again
    static QString iconCacheToken();
    static QString iconPath(const QString &packageName, const QString &hash);
    // The icon name for the launcher (an SVG it finds through its icon
    // directory), "" when the watch does not have that icon
    static QString iconName(const QString &packageName, const QString &hash);
    static bool storeIcon(const QString &packageName, const QString &hash, const QByteArray &base64Png);
    static QByteArray capabilities();

    /*
     * Notification pictures (capabilities <pictures>, see NotificationsPictureChrc): a
     * picture is a JPEG of at most PictureSide pixels a side and MaxPictureBytes,
     * kept in ~/.cache/asteroid-btsyncd/pictures/ID.jpg while a card shows it. The
     * card's hints: PictureHint, the file (there or not yet), PictureSizeHint "WxH".
     */
    static constexpr const char *PictureHint = "x-asteroid-picture";
    static constexpr const char *PictureSizeHint = "x-asteroid-picture-size";
    static const int PictureSide = 384;
    static const int MaxPictureBytes = 24 * 1024;
    // The cache: at most this many pictures and bytes (the oldest go first, those of
    // no card before those of a card)
    static const int MaxPictures = 128;
    static const int MaxPictureCacheBytes = 4 * 1024 * 1024;
    // A picture asked for is not asked for again within this long (unless the phone
    // came back meanwhile); one that has not come after PictureTimeoutMs is given up
    static const int PictureRetryMs = 60 * 1000;
    static const int PictureTimeoutMs = 120 * 1000;
    // The files of the cards that went are deleted this much later (a card posted
    // anew with the same picture keeps it)
    static const int PicturePruneDelayMs = 3000;
    enum PictureState { PicturePending = 0, PictureReady = 1, PictureUnavailable = 2 };
    static QString pictureDir();
    static bool isPictureId(const QString &id);
    // "" for an id that is not one
    static QString picturePath(const QString &id);
    // Checks a picture from the phone (size, hash, a whole JPEG) and writes it to the cache
    static bool storePicture(const QString &id, const QString &hash, int size, const QByteArray &base64Jpeg);
    // What the phone wrote to the picture characteristic: the picture (hasData), or that it
    // cannot send it; elapsedMs from its first write
    void pictureArrived(int phoneId, const QString &id, const QString &hash, int size,
                        const QByteArray &base64Jpeg, bool hasData, qint64 elapsedMs);
    // (a picture of a card's older message, not asked for yet, is asked for now: on demand)
    int pictureState(const QString &path);
    void setPicturePruneDelayMs(int ms) { mPicturePruneDelayMs = ms; }
    void setPictureTimeoutMs(int ms) { mPictureTimeoutMs = ms; }
    void setPictureCacheLimits(int pictures, int bytes) { mMaxPictures = pictures; mMaxPictureCacheBytes = bytes; }

    bool reply(uint notificationId, const QString &text);
    bool silence(uint notificationId);
    bool setAppNotifications(uint notificationId, bool enabled);

    /*
     * Quiet on the charger and off the wrist, as Wear OS (SysUI OffBodyAlertingMuter):
     * while mce says the charger is on, or the watch is off the wrist, phone
     * notifications come without banner or vibration (the find-my-watch alert still
     * rings). Settings > Vibration > "Silence when off wrist" is the dconf key
     * SilenceOffWristKey (default true; contract row CR-10). The retired
     * QuietOnChargerKey of image 44 is read only while SilenceOffWristKey is unset.
     * Off the wrist counts only while the launcher's mirror of the sensor hub says
     * it knows (OffBodyAvailableKey true) and OffBodyKey is true (CR-44).
     * The charger's state comes from mce (BatteryStatus), or setOnCharger (the tests).
     */
    static constexpr const char *SilenceOffWristKey = "/org/asteroidos/notifications/silence-off-wrist";
    static constexpr const char *QuietOnChargerKey = "/org/asteroidos/notifications/quiet-on-charger";
    static constexpr const char *OffBodyKey = "/org/asteroidos/sensors/off-body";
    static constexpr const char *OffBodyAvailableKey = "/org/asteroidos/sensors/off-body-available";
    // The switch: SilenceOffWristKey, else the retired QuietOnChargerKey, else true
    static bool silenceSwitch();
    bool onCharger() const;
    // The charger part only (the QuietOnCharger call of the D-Bus API)
    bool quietOnCharger() const;
    // The watch is known to be off the wrist and the switch is on
    bool quietOffWrist() const;

    /*
     * Settings > Modes > Customize (CR-30): /org/asteroidos/modes/MODE/starred and
     * /repeat, MODE do-not-disturb, bedtime or theater; the defaults are stock's
     * (Do Not Disturb and Bedtime allow both, Theater none).
     */
    static constexpr const char *ModeKeyBase = "/org/asteroidos/modes/";
    static bool modeAllows(const QString &mode, const QString &what);

    /*
     * Phone apps in Settings > Notifications > App notifications (CR-20):
     * BlockedPackagesKey (list of Android package names) is what the user switched
     * off: the inserts of those packages are dropped (and their card goes);
     * KnownAppsKey (list of "PACKAGE|NAME", at most MaxKnownApps) is what btsyncd
     * has seen, written when a package is new or its name changed.
     */
    static constexpr const char *BlockedPackagesKey = "/org/asteroidos/notifications/blocked-packages";
    static constexpr const char *KnownAppsKey = "/org/asteroidos/notifications/known-apps";
    static const int MaxKnownApps = 200;
    static bool isBlockedPackage(const QString &package);
    // true if the list was written
    static bool learnApp(const QString &package, const QString &name);
    void setOnCharger(bool on) { mChargerOverride = on ? 1 : 0; }

    // Find my phone (see NotificationsDBusApi::FindPhone)
    bool findPhone(bool start);
    bool findingPhone() const { return mFindingPhone; }
    // The phone is there: connected (BlueZ) and subscribed to the feedback
    bool phoneConnected() const;
    // The BlueZ link (BlueZManager): BlueZ keeps a bonded phone's subscriptions
    // over a disconnection, without StopNotify (and without StartNotify when it
    // is back), so the subscription alone does not say the phone is gone. Until
    // this is called (the tests), the subscription decides.
    void setLinkConnected(bool connected);
    // The watch's hands-free link: it runs the call cards' answer, reject and end for the
    // calls it knows (on the watch, with the call audio); "answer-phone" answers on the
    // phone, the audio stays there. Without it, or for a call it does not know, the action
    // goes to the phone as before.
    void setCallControl(CallControl *calls) { mCallControl = calls; }
    // Who keeps the call log (CallLog): told about the phone's call cards, from the frames only
    void setCallObserver(CallObserver *observer) { mCallObserver = observer; }
    // After the link is back: the phone has set itself up again by then (service
    // discovery, its subscriptions): queued dismissals are sent
    static const int PhoneSetupMs = 5000;
    /*
     * Calls after a (re)connection. The phone posts a call's card (ringing:
     * answer/reject; in progress: end) and removes it only when the call's
     * state changes, and its notification list (sync) never names calls. A
     * removal the watch missed (down, or out of range, when the call ended)
     * would leave the card, and the launcher's in-call view, up for hours.
     * So every (re)connection closes the call cards the phone has not posted
     * again within CallGraceMs of it: a phone that re-posts a live call keeps
     * its card, as does every card posted since the connection. (The
     * launcher ignores restored call cards older than a few minutes as well.)
     * The grace covers the phone's setup and its resync, which the journal
     * shows about 12 s after the subscription.
     */
    static const int CallGraceMs = 15 * 1000;
    void setCallGraceMs(int ms) { mCallGraceMs = ms; }
    // A ringing call or a call in progress, by what the phone offers for it
    static bool isCallCard(const QStringList &phoneActions);
    // The phone stopped ringing (found there, or its time ran out)
    void phoneStoppedRinging();

    // Freedesktop action key for the phone's own action N, and back (-1 if
    // the key is not one)
    static QString appActionKey(int index);
    static int appActionIndex(const QString &key);

signals:
    void findPhoneChanged(bool ringing);
    void phoneConnectedChanged(bool connected);
    void pictureChanged(const QString &path, bool available);
    void actionResult(uint notificationId, const QString &kind, int index, const QString &result);

public:
    // What the phone offered for a notification (by lipstick id), and for a
    // call the Notify arguments it was shown with (to update it)
    void remember(uint notificationId, const QStringList &phoneActions,
                  const QList<QVariant> &callNotify, const QString &thread = QString(),
                  uint content = 0, const QString &key = QString());
    void forget(uint notificationId);
    QString callTimestamp(uint notificationId) const;

private slots:
    void NotificationClosed(uint replacesId, uint reason);
    void ActionInvoked(uint replacesId, const QString &actionKey);

private:
    void sendFeedback(int phoneId, const QString &type, const QString &text = QString(), int index = -1);
    // A reply, app action or "Open on phone" went out (sent: to a phone that is there): its result
    // comes from the phone's answer or the timer
    void trackAction(int phoneId, const QString &kind, int index, bool sent);
    void reportAction(uint lipstickId, const QString &kind, int index, const char *result);
    void failPendingActions();
    // A card's action was reported to the phone: lipstick may close the card for it (a card
    // that is not resident), which is no dismiss
    void markActioned(uint lipstickId);
    bool wasActioned(uint lipstickId);
    void setFindingPhone(bool ringing);
    void closeNow(uint lipstickId);
    void flushQueuedDismissals();
    // CallGraceMs after the connection numbered `connection`: the call cards
    // the phone has not posted since go
    void closeStaleCalls(int connection);
    // phoneConnected() changed: signal, and what waited for the phone after flushDelayMs
    void updatePhoneConnected(int flushDelayMs);
    static uint contentHash(const QString &summary, const QString &body);
    // The picture a card shows (lipstick id), asked for from the phone when it is not here
    // The pictures a card shows (lipstick id), asked for from the phone when they are not
    // here: the card's own and its newest message's at once, the others when shown
    // (PictureState, on demand)
    void setPictures(uint lipstickId, int phoneId, const QStringList &ids, const QStringList &askNow);
    void requestPicture(int phoneId, const QString &id);
    bool pictureShown(const QString &id) const;
    void pictureUnavailable(const QString &id, const char *why);
    // Deletes (PicturePruneDelayMs later) the pictures no card shows, and keeps the
    // cache within its limits
    void schedulePicturePrune();
    void prunePictures();
    // The card the phone notification n belongs to goes (by id, conversation or key): the card's
    // lipstick id, 0 if it had none
    uint closeCardOf(const PhoneNotification &n);

    NotificationsFeedbackChrc *mFeedback;
    // mce's charger state (quiet on the charger); -1: mce's, else setOnCharger's
    BatteryStatus *mBattery = nullptr;
    int mChargerOverride = -1;
    QString mChargerLogged;
    // Lipstick ids whose close follows an action already reported to the
    // phone (and when): lipstick removes a notification that is not resident
    // after any action, and that close must not reach the phone as a dismiss. A
    // resident card stays, so only a close right after the action is its.
    QHash<uint, qint64> mActioned;
    static const int ActionedMs = 3000;
    struct PendingAction { int phoneId; QString kind; int index; uint lipstickId; int token; };
    QList<PendingAction> mPending;
    int mPendingToken = 0;
    int mActionAckMs = ActionAckMs;
    bool mPhoneAnswers = false;
    bool mBedtimeMode = false, mTheaterMode = false;
    struct CallVerdict { bool starred = false; bool repeat = false; qint64 at = 0; };
    QHash<int, CallVerdict> mCallVerdicts;
    // The callers that rang (a hash of their text, never the text) and when
    QHash<QString, qint64> mCallersSeen;
    QHash<uint, QStringList> mPhoneActions;
    CallControl *mCallControl = nullptr;
    CallObserver *mCallObserver = nullptr;
    QHash<uint, QList<QVariant>> mCallNotify;
    QHash<uint, QString> mTimestamps;
    QDBusServiceWatcher *mLipstickWatcher = nullptr;
    QTimer *mFindPhoneTimeout = nullptr;
    bool mFindingPhone = false;
    bool mLinkKnown = false;
    bool mLink = false;
    bool mPhoneConnected = false;
    int mMtu = 23;
    bool mDoNotDisturb = false;
    bool mModeQuiet = false;

    // Version 4: the card of each conversation (thread key -> lipstick id,
    // and back), and what each card shows (to alert only for new content)
    QHash<QString, uint> mThreads;
    QHash<uint, QString> mThreadOf;
    QHash<uint, uint> mContent;
    // The card of each phone notification by its key (and back), and when each card
    // was last posted (a card newer than the phone's list is not dropped by it)
    QHash<QString, uint> mKeys;
    QHash<uint, QString> mKeyOf;
    QHash<uint, qint64> mPostedAt;
    int mSyncGraceMs = SyncGraceMs;
    // lipstick id -> when the watch replied to it
    QHash<uint, qint64> mRepliedAt;
    // Cards the phone removed, closed a moment later: a conversation the app
    // re-posts (remove, then insert) keeps its card
    QHash<uint, int> mPendingClose;
    int mCloseToken = 0;
    // Dismissed while no phone was subscribed: (phone id, when), sent when a
    // phone subscribes; the phone's resync must not bring them back
    QList<QPair<int, qint64>> mQueuedDismissals;
    QHash<int, qint64> mDismissedAway;
    // Call cards after a (re)connection (CallGraceMs): when the phone
    // connected, and which connection the grace timer belongs to
    qint64 mConnectedAt = 0;
    int mConnection = 0;
    int mCallGraceMs = CallGraceMs;

    // Notification pictures: the picture of each card (lipstick id -> picture id); when
    // each was asked for, how often one came broken, the ones the phone cannot send
    QHash<uint, QStringList> mPictureOf;
    QHash<QString, qint64> mPictureRequested;
    QHash<QString, int> mPictureFailures;
    QSet<QString> mPicturesUnavailable;
    // lipstick's list was read: the cache's other files are no card's
    bool mPicturesKnown = false;
    QTimer *mPicturePrune = nullptr;
    int mPicturePruneDelayMs = PicturePruneDelayMs;
    int mPictureTimeoutMs = PictureTimeoutMs;
    int mMaxPictures = MaxPictures;
    int mMaxPictureCacheBytes = MaxPictureCacheBytes;
};

#endif // NOTIFICATIONSERVICE_H
