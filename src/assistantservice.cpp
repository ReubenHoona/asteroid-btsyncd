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

#include "assistantservice.h"
#include "assistantprotocol.h"
#include "common.h"
#include "notificationservice.h"
#include "wakehold.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDebug>

// Requests are a system prompt and a question, about 1 kB; replies a
// sentence or two
static const int MaxRequestSize = 8192;
static const int MaxReplySize = 8192;
// The phone posts with its own timeout (asteroid-assistant waits 12 s);
// questions nobody answered are forgotten after this
static const int ForgetMs = 30000;
// When the phone's writes carry no mtu option (BlueZ < 5.62): the smallest
static const int AssumedMtu = 23;
// An action's result: directions and contacts come in a second or two; a
// text message once the phone's network took it (Gadgetbridge waits up to 20 s)
static const int ActResultMs = 15000;
static const int SmsResultMs = 40000;

static int optionMtu(const QVariantMap &options)
{
    return options.contains("mtu") ? options.value("mtu").toInt() : 0;
}

/* Phone to watch: <asr>...</asr> in as many writes as needed */
class AssistantReplyChrc : public Characteristic
{
public:
    AssistantReplyChrc(QDBusConnection bus, unsigned int index, AssistantService *service)
      : Characteristic(bus, index, ASSISTANT_REPLY_UUID, {"encrypt-authenticated-write"}, service, service), mService(service) {}

public slots:
    void WriteValue(QByteArray value, QVariantMap options) override
    {
        // the watch may be asleep when the answer comes: hold while it is handled
        WakeHoldScope wake;
        mBuffer += value;
        for (;;) {
            // an answer or status ends with </asr>, an action's result with </acr>
            const int a = mBuffer.indexOf("</asr>");
            const int c = mBuffer.indexOf("</acr>");
            const int end = a < 0 ? c : c < 0 ? a : qMin(a, c);
            if (end < 0) {
                if (mBuffer.size() > MaxReplySize) {
                    qWarning() << "Assistant reply without an end, dropped";
                    mBuffer.clear();
                }
                return;
            }
            const QByteArray xml = mBuffer.left(end + 6);
            mBuffer = mBuffer.mid(end + 6);
            mService->onReply(xml, optionMtu(options));
        }
    }

private:
    AssistantService *mService;
    QByteArray mBuffer;
};

class AssistantCapabilitiesChrc : public Characteristic
{
public:
    AssistantCapabilitiesChrc(QDBusConnection bus, unsigned int index, Service *service)
      : Characteristic(bus, index, ASSISTANT_CAP_UUID, {"encrypt-authenticated-read"}, service, service) {}

public slots:
    QByteArray ReadValue(QVariantMap options) override
    {
        const QByteArray value = AssistantProtocol::capabilitiesXml();
        const int offset = options.value("offset", 0).toInt();
        return offset > 0 ? value.mid(offset) : value;
    }
};

AssistantRequestsChrc::AssistantRequestsChrc(QDBusConnection bus, int index, AssistantService *service)
    : Characteristic(bus, index, ASSISTANT_REQUEST_UUID, {"encrypt-authenticated-notify"}, service, service), mService(service)
{
}

void AssistantRequestsChrc::send(const QByteArray &xml, int mtu)
{
    // one notification carries MTU - 3 bytes; the phone joins them until </ask>
    const int chunk = qBound(20, (mtu > 0 ? mtu : AssumedMtu) - 3, 509);
    for (int pos = 0; pos < xml.size(); pos += chunk) {
        mValue = xml.mid(pos, chunk);
        emit valueChanged();
        emitPropertiesChanged();
    }
}

void AssistantRequestsChrc::StartNotify()
{
    mNotifying = true;
    mService->notifyChanged(true);
}

void AssistantRequestsChrc::StopNotify()
{
    mNotifying = false;
    mService->notifyChanged(false);
}

void AssistantRequestsChrc::emitPropertiesChanged()
{
    QDBusMessage message = QDBusMessage::createSignal(getPath().path(), "org.freedesktop.DBus.Properties",
                                                      "PropertiesChanged");
    QVariantMap changedProperties;
    changedProperties[QStringLiteral("Value")] = QVariant(mValue);
    message << QVariant(GATT_CHRC_IFACE) << QVariant(changedProperties) << QVariant(QStringList());
    if (!QDBusConnection::systemBus().send(message))
        fprintf(stderr, "AssistantRequestsChrc: failed to send the PropertiesChanged signal\n");
}

AssistantDBusApi::AssistantDBusApi(AssistantService *service) : QObject(service), mService(service)
{
    connect(service, &AssistantService::answered, this, &AssistantDBusApi::Answer);
    connect(service, &AssistantService::failed, this, &AssistantDBusApi::Failed);
    connect(service, &AssistantService::actResult, this, &AssistantDBusApi::ActResult);
}

bool AssistantDBusApi::Available() { return mService->available(); }
bool AssistantDBusApi::Ask(uint id, const QString &request) { return mService->ask(id, request.toUtf8()); }
bool AssistantDBusApi::Cancel(uint id) { return mService->cancel(id); }
QStringList AssistantDBusApi::Actions() { return mService->actions(); }
bool AssistantDBusApi::Act(uint id, const QString &json) { return mService->act(id, json.toUtf8()); }
bool AssistantDBusApi::CancelAct(uint id) { return mService->cancelAct(id); }

AssistantService::AssistantService(int index, QDBusConnection bus, NotificationService *notifications, QObject *parent)
    : Service(bus, index, ASSISTANT_UUID, parent), mNotifications(notifications)
{
    mRequests = new AssistantRequestsChrc(bus, 0, this);
    addCharacteristic(mRequests);
    addCharacteristic(new AssistantReplyChrc(bus, 1, this));
    addCharacteristic(new AssistantCapabilitiesChrc(bus, 2, this));

    mTimeout.setSingleShot(true);
    connect(&mTimeout, &QTimer::timeout, this, &AssistantService::expire);

    // A bonded phone out of range keeps its subscription: without this the
    // relay stayed "available", asteroid-assistant sent its questions into the
    // void and failed after its own timeout instead of asking the server
    if (mNotifications) {
        connect(mNotifications, &NotificationService::phoneConnectedChanged, this, [this](bool connected) {
            if (!connected)
                phoneGone("link");
        });
    }

    mApi = new AssistantDBusApi(this);
    QDBusConnection session = QDBusConnection::sessionBus();
    if (!session.registerObject(QStringLiteral(BTSYNCD_ASSISTANT_PATH), mApi,
                                QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllSignals))
        fprintf(stderr, "AssistantService: cannot register %s on the session bus\n", BTSYNCD_ASSISTANT_PATH);
}

bool AssistantService::available() const
{
    return mRequests->notifying() && mPhoneReady && (!mNotifications || mNotifications->phoneConnected());
}

bool AssistantService::ask(uint id, const QByteArray &request)
{
    if (!available() || id == 0 || request.isEmpty() || request.size() > MaxRequestSize)
        return false;
    mWaiting.insert(id);
    mRequests->send(AssistantProtocol::askXml(id, request), mMtu);
    mTimeout.start(ForgetMs);
    qInfo() << "Assistant: question" << id << "to the phone," << request.size() << "bytes";
    return true;
}

bool AssistantService::cancel(uint id)
{
    if (!mWaiting.remove(id))
        return false;
    if (mRequests->notifying())
        mRequests->send(AssistantProtocol::cancelXml(id), mMtu);
    return true;
}

QStringList AssistantService::actions() const
{
    // as available(), without the answer server: actions work without one
    if (!mRequests->notifying() || (mNotifications && !mNotifications->phoneConnected()))
        return QStringList();
    return mPhoneActions;
}

bool AssistantService::act(uint id, const QByteArray &json)
{
    if (id == 0 || json.isEmpty() || json.size() > MaxRequestSize || mActs.contains(id))
        return false;
    const QString a = QJsonDocument::fromJson(json).object().value(QStringLiteral("a")).toString();
    if (a.isEmpty() || !actions().contains(a))
        return false;
    // the phone answers each in a few seconds (a text message: once the network took it)
    QTimer *t = new QTimer(this);
    t->setSingleShot(true);
    connect(t, &QTimer::timeout, this, [this, id] {
        qInfo() << "Assistant: action" << id << "got no result from the phone";
        if (mRequests->notifying())
            mRequests->send(AssistantProtocol::actCancelXml(id), mMtu);
        endAct(id, QStringLiteral("{\"ok\":false,\"er\":\"timeout\"}"));
    });
    t->start(a == QLatin1String("sms") ? SmsResultMs : ActResultMs);
    mActs.insert(id, t);
    mRequests->send(AssistantProtocol::actXml(id, json), mMtu);
    // the action's kind only: no place, name or message in the log
    qInfo() << "Assistant: action" << id << a << "to the phone," << json.size() << "bytes";
    return true;
}

bool AssistantService::cancelAct(uint id)
{
    QTimer *t = mActs.take(id);
    if (!t)
        return false;
    t->deleteLater();
    if (mRequests->notifying())
        mRequests->send(AssistantProtocol::actCancelXml(id), mMtu);
    return true;
}

void AssistantService::endAct(uint id, const QString &json)
{
    QTimer *t = mActs.take(id);
    if (!t)
        return;
    t->stop();
    t->deleteLater();
    emit actResult(id, json);
}

void AssistantService::onReply(const QByteArray &xml, int mtu)
{
    if (mtu > 0)
        mMtu = mtu;
    const AssistantProtocol::Reply r = AssistantProtocol::parseReply(xml);
    if (!r.valid) {
        qWarning() << "Assistant: bad reply from the phone";
        return;
    }
    if (r.act) {
        if (!mActs.contains(r.id)) {
            qDebug() << "Assistant: action result" << r.id << "nobody waits for";
            return;
        }
        qInfo() << "Assistant: action" << r.id << "result from the phone," << r.json.size() << "bytes";
        endAct(r.id, r.json);
        return;
    }
    if (!r.state.isEmpty()) {
        mPhoneReady = r.state == QLatin1String("ready");
        // an older Gadgetbridge says nothing about actions: none
        mPhoneActions = r.hasActions ? r.actions : QStringList();
        qInfo() << "Assistant: the phone's relay is" << r.state << r.error << "MTU" << mMtu
                << "actions" << mPhoneActions.join(QLatin1Char(','));
        return;
    }
    if (!mWaiting.remove(r.id)) {
        qDebug() << "Assistant: answer" << r.id << "nobody waits for";
        return;
    }
    if (!r.error.isEmpty()) {
        qInfo() << "Assistant: the phone could not answer" << r.id << ":" << r.error;
        emit failed(r.id, r.error);
    } else {
        qInfo() << "Assistant: answer" << r.id << "from the phone," << r.text.size() << "characters";
        emit answered(r.id, r.text);
    }
}

void AssistantService::expire()
{
    const QSet<uint> waiting = mWaiting;
    mWaiting.clear();
    for (uint id : waiting)
        emit failed(id, QStringLiteral("timeout"));
}

void AssistantService::notifyChanged(bool on)
{
    qInfo() << "Assistant: relay notifications" << (on ? "on" : "off");
    if (on)
        return;
    phoneGone("unsubscribed");
}

void AssistantService::phoneGone(const char *why)
{
    // it says again whether it relays when it is back (Gadgetbridge writes
    // its state on every connection), and with the MTU of that connection
    const bool wasReady = mPhoneReady;
    mPhoneReady = false;
    mPhoneActions.clear();
    mMtu = 0;
    mTimeout.stop();
    const QSet<uint> waiting = mWaiting;
    mWaiting.clear();
    const QList<uint> acts = mActs.keys();
    if (wasReady || !waiting.isEmpty() || !acts.isEmpty())
        qInfo() << "Assistant: the phone is gone (" << why << "):" << waiting.size() << "question(s) and"
                << acts.size() << "action(s) failed";
    for (uint id : waiting)
        emit failed(id, QStringLiteral("disconnected"));
    for (uint id : acts)
        endAct(id, QStringLiteral("{\"ok\":false,\"er\":\"disconnected\"}"));
}
