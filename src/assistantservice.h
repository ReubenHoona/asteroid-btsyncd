/*
 * Copyright (C) 2026 Reuben Horner <reubarb.scons@gmail.com>
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

#ifndef ASSISTANTSERVICE_H
#define ASSISTANTSERVICE_H

#include <QByteArray>
#include <QObject>
#include <QHash>
#include <QSet>
#include <QStringList>
#include <QString>
#include <QTimer>

#include "characteristic.h"
#include "service.h"

class AssistantService;
class NotificationService;

/* Watch to phone: the questions (notifications of D001, MTU-sized pieces) */
class AssistantRequestsChrc : public Characteristic
{
    Q_OBJECT
    Q_PROPERTY(QByteArray Value READ getValue NOTIFY valueChanged)

public:
    AssistantRequestsChrc(QDBusConnection bus, int index, AssistantService *service);
    void send(const QByteArray &xml, int mtu);
    bool notifying() const { return mNotifying; }

public slots:
    void StartNotify() override;
    void StopNotify() override;

signals:
    void valueChanged();

private:
    void emitPropertiesChanged();
    QByteArray getValue() { return mValue; }
    QByteArray mValue;
    AssistantService *mService;
    bool mNotifying = false;
};

/*
 * Session bus API for asteroid-assistant: org.asteroidos.btsyncd.Assistant
 * on /org/asteroidos/btsyncd/assistant.
 *   Available() -> b       a phone that relays questions is connected
 *   Ask(u id, s request) -> b   request: the chat completions body (JSON)
 *   Cancel(u id) -> b
 * Signals Answer(u id, s text), Failed(u id, s code): timeout (no answer
 * from the phone in time), disconnected, or the phone's code (offline,
 * server, auth, no-url, bad-request).
 *
 * Actions on the phone (the launcher's assistant: directions, contacts, text
 * messages; see assistantprotocol.h):
 *   Actions() -> as        what the connected phone does now (nav, contacts,
 *                          sms); empty: no phone, or a Gadgetbridge without them
 *   Act(u id, s json) -> b json: {"a":"nav"|"contacts"|"sms", ...}; false when
 *                          the phone is away or does not do "a"
 *   CancelAct(u id) -> b
 * Signal ActResult(u id, s json): the phone's result, or {"ok":false,"er":
 * "timeout"|"disconnected"} from btsyncd. Every Act() that returned true gets
 * exactly one.
 */
class AssistantDBusApi : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.asteroidos.btsyncd.Assistant")

public:
    explicit AssistantDBusApi(AssistantService *service);

public slots:
    bool Available();
    bool Ask(uint id, const QString &request);
    bool Cancel(uint id);
    QStringList Actions();
    bool Act(uint id, const QString &json);
    bool CancelAct(uint id);

signals:
    void Answer(uint id, const QString &text);
    void Failed(uint id, const QString &code);
    void ActResult(uint id, const QString &json);

private:
    AssistantService *mService;
};

class AssistantService : public Service
{
    Q_OBJECT
public:
    // notifications: the phone's BlueZ link (NotificationService::phoneConnected):
    // BlueZ keeps a bonded phone's subscription when it goes, without StopNotify,
    // so the subscription alone does not say the phone is there
    AssistantService(int index, QDBusConnection bus, NotificationService *notifications = nullptr,
                     QObject *parent = nullptr);

    bool available() const;
    bool ask(uint id, const QByteArray &request);
    bool cancel(uint id);
    QStringList actions() const;
    bool act(uint id, const QByteArray &json);
    bool cancelAct(uint id);

    // From the characteristics
    void onReply(const QByteArray &xml, int mtu);
    void notifyChanged(bool on);

signals:
    void answered(uint id, const QString &text);
    void failed(uint id, const QString &code);
    void actResult(uint id, const QString &json);

private:
    void expire();
    // The phone is gone (unsubscribed, or its link dropped): what waits fails
    void phoneGone(const char *why);
    void endAct(uint id, const QString &json);

    NotificationService *mNotifications;
    AssistantRequestsChrc *mRequests;
    AssistantDBusApi *mApi;
    bool mPhoneReady = false;
    QStringList mPhoneActions;
    int mMtu = 0;
    QSet<uint> mWaiting;
    QTimer mTimeout;
    QHash<uint, QTimer *> mActs;   // actions waiting for the phone, each with its deadline
};

#endif // ASSISTANTSERVICE_H
