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

#include <functional>

#include "assistantprotocol.h"
#include "characteristic.h"
#include "contactnames.h"
#include "phonerelay.h"
#include "service.h"

class AssistantReplyChrc;
class AssistantService;
class MDConfItem;
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
 * exactly one. Ids from InternalIdBase up are btsyncd's own (the Messages
 * API, messageservice.h): Act() refuses them and ActResult never carries them.
 *
 * Signal PhoneEvent(s kind, s json): something the phone reports on its own
 * (assistantprotocol.h): kind nav (the next turn of a Google Maps
 * navigation, or {"st":"end"}) or cam (the camera remote's state).
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
    void PhoneEvent(const QString &kind, const QString &json);

private slots:
    void onActResult(uint id, const QString &json);

private:
    AssistantService *mService;
};

class AssistantService : public Service, public PhoneRelay
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

    // PhoneRelay: btsyncd's own acts with a callback (CallAudio's cvol, the weather refresh)
    bool relayHas(const QString &act) const override { return actions().contains(act); }
    bool relayAct(const QByteArray &json, const std::function<void(const QString &)> &done) override;

    // Action ids from here up belong to btsyncd's own users (MessageService)
    static const uint InternalIdBase = 0x40000000u;
    // The ids of relayAct() are in a band of their own, above the Messages API's
    static const uint RelayIdBase = InternalIdBase + 0x20000000u;

    // From the characteristics
    void onReply(const QByteArray &xml, int mtu);
    void notifyChanged(bool on);

signals:
    void answered(uint id, const QString &text);
    void failed(uint id, const QString &code);
    void actResult(uint id, const QString &json);
    void phoneEvent(const QString &kind, const QString &json);

private:
    void expire();
    // The phone is gone (unsubscribed, or its link dropped): what waits fails
    void phoneGone(const char *why);
    void endAct(uint id, const QString &json);
    // The watch's Weather app asked for a fresh forecast (dconf weather/refresh-request, CR-40):
    // the phone is asked over the act wxrefresh, while one is connected that does it
    void onWeatherRefreshRequest();
    // A part of the phone's contact names, or "off" (contactnames.h)
    void onNames(const AssistantProtocol::Reply &r);

    NotificationService *mNotifications;
    AssistantRequestsChrc *mRequests;
    AssistantReplyChrc *mReply;
    AssistantDBusApi *mApi;
    bool mPhoneReady = false;
    QStringList mPhoneActions;
    int mMtu = 0;
    QSet<uint> mWaiting;
    QTimer mTimeout;
    QHash<uint, QTimer *> mActs;   // actions waiting for the phone, each with its deadline
    ContactNames::Assembler mNames;
    QHash<uint, std::function<void(const QString &)>> mDone;
    uint mRelayCounter = 0;
    MDConfItem *mWeatherRefresh = nullptr;
    bool mWeatherRefreshing = false;
};

#endif // ASSISTANTSERVICE_H
