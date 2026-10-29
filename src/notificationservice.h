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

#include "service.h"

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
    bool resync = false;
    // Version 4
    QString thread;
    // The phone notification's identity (a hash of Android's notification key): the
    // same card whatever phone id it comes with
    QString key;
    QList<QPair<QString, QString>> messages;   // sender, text; oldest first
    int importance = 0;                         // Android's 1 (min) .. 5 (max), 0 unknown
    bool onlyAlertOnce = false;
    bool ongoing = false;
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
 */
struct PostPlan
{
    enum Kind { New, Update, Repost };
    Kind kind = New;
    bool preview = false;
    bool vibrate = false;

    static PostPlan decide(const PhoneNotification &n, bool exists, bool contentChanged, bool doNotDisturb);
    // The ngfd event (lipstick's x-nemo-feedback) of a notification that
    // vibrates, from the phone's <vb>
    static QString feedbackEvent(const PhoneNotification &n);
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

signals:
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
    static constexpr const char *KeyHint = "x-asteroid-phone-key";
    static constexpr const char *CategoryHint = "x-asteroid-category";
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
    int pictureState(const QString &path) const;
    void setPicturePruneDelayMs(int ms) { mPicturePruneDelayMs = ms; }
    void setPictureTimeoutMs(int ms) { mPictureTimeoutMs = ms; }
    void setPictureCacheLimits(int pictures, int bytes) { mMaxPictures = pictures; mMaxPictureCacheBytes = bytes; }

    bool reply(uint notificationId, const QString &text);
    bool silence(uint notificationId);

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
    // After the link is back: the phone has set itself up again by then (service
    // discovery, its subscriptions): queued dismissals are sent
    static const int PhoneSetupMs = 5000;
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
    void setFindingPhone(bool ringing);
    void closeNow(uint lipstickId);
    void flushQueuedDismissals();
    // phoneConnected() changed: signal, and what waited for the phone after flushDelayMs
    void updatePhoneConnected(int flushDelayMs);
    static uint contentHash(const QString &summary, const QString &body);
    // The picture a card shows (lipstick id), asked for from the phone when it is not here
    void setPicture(uint lipstickId, int phoneId, const QString &id);
    void requestPicture(int phoneId, const QString &id);
    bool pictureShown(const QString &id) const;
    void pictureUnavailable(const QString &id, const char *why);
    // Deletes (PicturePruneDelayMs later) the pictures no card shows, and keeps the
    // cache within its limits
    void schedulePicturePrune();
    void prunePictures();

    NotificationsFeedbackChrc *mFeedback;
    // Lipstick ids whose close follows an action already reported to the
    // phone: lipstick removes a notification after any action, and that close
    // must not reach the phone as a dismiss.
    QSet<uint> mActioned;
    QHash<uint, QStringList> mPhoneActions;
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

    // Notification pictures: the picture of each card (lipstick id -> picture id); when
    // each was asked for, how often one came broken, the ones the phone cannot send
    QHash<uint, QString> mPictureOf;
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
