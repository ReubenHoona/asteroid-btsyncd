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
#include <QSet>
#include <QStringList>

#include "service.h"

class NotificationService;

/*
 * Watch to phone: what the user did with a notification, as XML in
 * notifications of at most MTU - 3 bytes; the phone joins them until the
 * closing tag:
 *   <action><id>ID</id><ty>TYPE</ty><tx>TEXT</tx></action>
 * TYPE is dismiss, or one of the actions the phone offered in <ac>; TEXT
 * only for reply.
 */
class NotificationsFeedbackChrc : public Characteristic
{
    Q_OBJECT
    Q_PROPERTY(QByteArray Value READ getValue NOTIFY valueChanged)

public:
    NotificationsFeedbackChrc(QDBusConnection bus, int index, Service *service);
    void send(const QByteArray &xml, int mtu);

signals:
    void valueChanged();

private slots:
    void emitPropertiesChanged();

private:
    QByteArray mValue;
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

private:
    NotificationService *mService;
};

class NotificationService : public Service
{
    Q_OBJECT
public:
    explicit NotificationService(int index, QDBusConnection bus, QObject *parent = 0);

    // Phone notification id -> lipstick notification id
    QHash<int, uint> mKnownAndroidNotifs;

    void setMtu(int mtu);
    bool reply(uint notificationId, const QString &text);
    bool silence(uint notificationId);

    // What the phone offered for a notification (by lipstick id), and for a
    // call the Notify arguments it was shown with (to update it)
    void remember(uint notificationId, const QStringList &phoneActions,
                  const QList<QVariant> &callNotify);
    void forget(uint notificationId);
    QString callTimestamp(uint notificationId) const;

private slots:
    void NotificationClosed(uint replacesId, uint reason);
    void ActionInvoked(uint replacesId, const QString &actionKey);

private:
    void sendFeedback(int phoneId, const QString &type, const QString &text = QString());

    NotificationsFeedbackChrc *mFeedback;
    // Lipstick ids whose close follows an action already reported to the
    // phone: lipstick removes a notification after any action, and that close
    // must not reach the phone as a dismiss.
    QSet<uint> mActioned;
    QHash<uint, QStringList> mPhoneActions;
    QHash<uint, QList<QVariant>> mCallNotify;
    int mMtu = 23;
};

#endif // NOTIFICATIONSERVICE_H
