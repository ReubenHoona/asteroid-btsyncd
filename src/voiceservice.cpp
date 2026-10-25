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

#include "voiceservice.h"
#include "voicesource.h"
#include "notificationservice.h"
#include "characteristic.h"
#include "common.h"

#include <MDConfItem>
#include <QDBusMessage>
#include <QDebug>

// Frames per audio notification at most: 2 x 20 ms keeps the added latency
// at 20 ms and the rate at 25 notifications a second
static const int MaxFramesPerNotification = 2;
// A session listens at most this long
static const int MaxListenMs = 30000;
// After the end of speech, the phone has this long for the final text
static const int ResultTimeoutMs = 10000;
// Result writes are a few hundred bytes; anything longer without </vr> is junk
static const int MaxResultSize = 4096;
// When the phone's writes carry no mtu option (BlueZ < 5.62)
static const int AssumedMtu = 247;

static int optionMtu(const QVariantMap &options)
{
    return options.contains("mtu") ? options.value("mtu").toInt() : 0;
}

/* Phone to watch: <vr>...</vr> in as many writes as needed */
class VoiceResultChrc : public Characteristic
{
public:
    VoiceResultChrc(QDBusConnection bus, unsigned int index, VoiceService *service)
      : Characteristic(bus, index, VOICE_RESULT_UUID, {"encrypt-authenticated-write"}, service, service), mVoice(service) {}

public slots:
    void WriteValue(QByteArray value, QVariantMap options) override
    {
        mBuffer += value;
        const int end = mBuffer.indexOf("</vr>");
        if (end < 0) {
            if (mBuffer.size() > MaxResultSize) {
                qWarning() << "Voice result without an end, dropped";
                mBuffer.clear();
            }
            return;
        }
        const QByteArray xml = mBuffer.left(end + 5);
        mBuffer = mBuffer.mid(end + 5);
        mVoice->onResult(xml, optionMtu(options));
    }

private:
    VoiceService *mVoice;
    QByteArray mBuffer;
};

/* The phone reads what this watch sends */
class VoiceCapabilitiesChrc : public Characteristic
{
public:
    VoiceCapabilitiesChrc(QDBusConnection bus, unsigned int index, Service *service)
      : Characteristic(bus, index, VOICE_CAP_UUID, {"encrypt-authenticated-read"}, service, service) {}

public slots:
    QByteArray ReadValue(QVariantMap options) override
    {
        const QByteArray value = VoiceProtocol::capabilitiesXml();
        const int offset = options.value("offset", 0).toInt();
        return offset > 0 ? value.mid(offset) : value;
    }
};

VoiceNotifyChrc::VoiceNotifyChrc(QDBusConnection bus, int index, const QString &uuid, VoiceService *service)
    : Characteristic(bus, index, uuid, {"encrypt-authenticated-notify"}, service, service), mVoice(service)
{
}

void VoiceNotifyChrc::send(const QByteArray &value)
{
    mValue = value;
    emit valueChanged();
    emitPropertiesChanged();
}

void VoiceNotifyChrc::StartNotify()
{
    mNotifying = true;
    mVoice->notifyChanged(this, true);
}

void VoiceNotifyChrc::StopNotify()
{
    mNotifying = false;
    mVoice->notifyChanged(this, false);
}

void VoiceNotifyChrc::emitPropertiesChanged()
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
        fprintf(stderr, "VoiceNotifyChrc: failed to send the PropertiesChanged signal\n");
}

VoiceDBusApi::VoiceDBusApi(VoiceService *service) : QObject(service), mService(service)
{
    connect(service, &VoiceService::partialText, this, &VoiceDBusApi::Partial);
    connect(service, &VoiceService::finalText, this, &VoiceDBusApi::Final);
    connect(service, &VoiceService::failed, this, &VoiceDBusApi::Error);
}

uint VoiceDBusApi::Start(uint notificationId, const QString &source) { return mService->start(notificationId, source); }
bool VoiceDBusApi::Stop() { return mService->stop(); }
bool VoiceDBusApi::Cancel() { return mService->cancel(); }
bool VoiceDBusApi::Send() { return mService->send(); }
bool VoiceDBusApi::Available() { return mService->available(); }
QString VoiceDBusApi::State() { return mService->state(); }

VoiceService::VoiceService(int index, QDBusConnection bus, NotificationService *notifications, QObject *parent)
    : Service(bus, index, VOICE_UUID, parent), mNotifications(notifications)
{
    mEvent = new VoiceNotifyChrc(bus, 0, VOICE_EVENT_UUID, this);
    mAudio = new VoiceNotifyChrc(bus, 1, VOICE_AUDIO_UUID, this);
    addCharacteristic(mEvent);
    addCharacteristic(mAudio);
    addCharacteristic(new VoiceResultChrc(bus, 2, this));
    addCharacteristic(new VoiceCapabilitiesChrc(bus, 3, this));

    mMaxTimer.setSingleShot(true);
    connect(&mMaxTimer, &QTimer::timeout, this, [this] {
        qInfo() << "Voice: longest listening time reached";
        stop();
    });
    mResultTimer.setSingleShot(true);
    connect(&mResultTimer, &QTimer::timeout, this, &VoiceService::onResultTimeout);

    mApi = new VoiceDBusApi(this);
    QDBusConnection session = QDBusConnection::sessionBus();
    if (!session.registerObject(BTSYNCD_VOICE_PATH, mApi,
                                QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllSignals))
        fprintf(stderr, "VoiceService: cannot register %s on the session bus\n", BTSYNCD_VOICE_PATH);
}

bool VoiceService::available() const
{
    return (mPhoneReady || mAudio->notifying()) && mPhoneUnavailable.isEmpty();
}

QString VoiceService::state() const
{
    switch (mState) {
    case Listening: return QStringLiteral("listening");
    case Recognising: return QStringLiteral("recognising");
    case Done: return QStringLiteral("done");
    default: return QStringLiteral("idle");
    }
}

int VoiceService::framesPerNotification() const
{
    const int mtu = mMtu > 0 ? mMtu : AssumedMtu;
    return qMin(MaxFramesPerNotification, (mtu - 3) / VoiceProtocol::FrameBytes);
}

QString VoiceService::sourceSpec(const QString &requested)
{
    if (!requested.isEmpty())
        return requested;
    const QString env = qEnvironmentVariable("BTSYNCD_VOICE_SOURCE");
    if (!env.isEmpty())
        return env;
    const QString conf = MDConfItem("/org/asteroidos/btsyncd/voice-source").value().toString();
    return conf.isEmpty() ? QStringLiteral("mic") : conf;
}

bool VoiceService::startLocal(uint, const QString &)
{
    // No on-watch recogniser yet
    return false;
}

void VoiceService::failLater(const QString &code)
{
    // after the D-Bus reply to Start(), so the caller sees 0 first
    QTimer::singleShot(0, this, [this, code] { emit failed(code); });
}

uint VoiceService::start(uint notificationId, const QString &source)
{
    if (mState == Listening || mState == Recognising)
        cancel();
    mState = Idle;

    if (!available()) {
        if (startLocal(notificationId, source))
            return 0;
        qInfo() << "Voice: no phone for voice replies" << mPhoneUnavailable;
        failLater(mPhoneUnavailable.isEmpty() ? QStringLiteral("no-phone") : mPhoneUnavailable);
        return 0;
    }
    if (framesPerNotification() < 1) {
        qWarning() << "Voice: ATT MTU" << mMtu << "too small for an audio frame";
        failLater(QStringLiteral("mtu"));
        return 0;
    }

    const QString spec = sourceSpec(source);
    mSource = VoiceSource::create(spec, this);
    connect(mSource, &VoiceSource::frame, this, &VoiceService::onFrame);
    connect(mSource, &VoiceSource::finished, this, [this] { stop(); });
    connect(mSource, &VoiceSource::failed, this, &VoiceService::onSourceFailed);
    QString err;
    if (!mSource->start(err)) {
        qWarning() << "Voice: source" << spec << "failed:" << err;
        delete mSource;
        mSource = nullptr;
        failLater(QStringLiteral("source"));
        return 0;
    }

    mSid = quint8(mSid % 255 + 1);
    mNotificationId = notificationId;
    mSeq = 0;
    mFrames = 0;
    mAdpcm = VoiceProtocol::AdpcmState();
    mPending.clear();
    mPendingFrames = 0;
    mLastPartial.clear();
    mFinal.clear();
    mState = Listening;

    const int phoneId = mNotifications ? mNotifications->mKnownAndroidNotifs.key(notificationId, 0) : 0;
    const QString lang = MDConfItem("/org/asteroidos/btsyncd/voice-language").value().toString();
    sendEvent(VoiceProtocol::startXml(mSid, phoneId, lang));
    mMaxTimer.start(MaxListenMs);
    qInfo() << "Voice: session" << mSid << "for notification" << notificationId << "(phone" << phoneId
            << ") from" << spec << "," << framesPerNotification() << "frames per notification";
    return mSid;
}

void VoiceService::onFrame(const QByteArray &pcm)
{
    if (mState != Listening || pcm.size() != VoiceProtocol::FramePcmBytes)
        return;
    mPending += VoiceProtocol::adpcmFrame(mSid, mSeq++, mAdpcm,
                                          reinterpret_cast<const qint16 *>(pcm.constData()));
    mPendingFrames++;
    mFrames++;
    if (mPendingFrames >= framesPerNotification())
        flush();
}

void VoiceService::flush()
{
    if (mPending.isEmpty())
        return;
    mAudio->send(mPending);
    mPending.clear();
    mPendingFrames = 0;
}

void VoiceService::stopSource()
{
    if (!mSource)
        return;
    mSource->stop();
    mSource->disconnect(this);
    mSource->deleteLater();
    mSource = nullptr;
}

bool VoiceService::stop()
{
    if (mState != Listening)
        return false;
    stopSource();
    flush();
    sendEvent(VoiceProtocol::endXml(mSid, mFrames));
    mMaxTimer.stop();
    mState = Recognising;
    mResultTimer.start(ResultTimeoutMs);
    qInfo() << "Voice: session" << mSid << "ended after" << mFrames << "frames";
    return true;
}

bool VoiceService::cancel()
{
    if (mState != Listening && mState != Recognising) {
        mState = Idle;
        mFinal.clear();
        return false;
    }
    stopSource();
    mPending.clear();
    mPendingFrames = 0;
    mMaxTimer.stop();
    mResultTimer.stop();
    sendEvent(VoiceProtocol::cancelXml(mSid));
    mState = Idle;
    qInfo() << "Voice: session" << mSid << "cancelled";
    return true;
}

bool VoiceService::send()
{
    if (mState != Done || mFinal.isEmpty() || !mNotifications)
        return false;
    const bool sent = mNotifications->reply(mNotificationId, mFinal);
    qInfo() << "Voice: reply to notification" << mNotificationId << (sent ? "sent" : "not sent");
    mState = Idle;
    mFinal.clear();
    return sent;
}

void VoiceService::sendEvent(const QByteArray &xml)
{
    // One notification carries at most MTU - 3 bytes; the phone joins the
    // pieces until </voice>
    const int mtu = mMtu > 0 ? mMtu : AssumedMtu;
    const int chunk = qBound(20, mtu - 3, 509);
    for (int pos = 0; pos < xml.size(); pos += chunk)
        mEvent->send(xml.mid(pos, chunk));
}

void VoiceService::onResult(const QByteArray &xml, int mtu)
{
    if (mtu > 0)
        mMtu = mtu;
    const VoiceProtocol::Result r = VoiceProtocol::parseResult(xml);
    if (!r.valid) {
        qWarning() << "Voice: bad result" << xml;
        return;
    }
    if (!r.state.isEmpty()) {
        if (r.state == QLatin1String("ready")) {
            mPhoneReady = true;
            mPhoneUnavailable.clear();
            if (mtu <= 0)
                qInfo() << "Voice: the phone's write has no MTU, assuming" << AssumedMtu;
        } else if (r.state == QLatin1String("unavailable")) {
            mPhoneReady = false;
            mPhoneUnavailable = r.error.isEmpty() ? QStringLiteral("unavailable") : r.error;
        }
        qInfo() << "Voice: phone" << r.state << r.lang << r.error << "MTU" << mMtu;
        return;
    }
    if (r.sid != mSid || (mState != Listening && mState != Recognising)) {
        qDebug() << "Voice: result for session" << r.sid << "ignored (current" << mSid << state() << ")";
        return;
    }
    if (!r.error.isEmpty()) {
        qInfo() << "Voice: session" << mSid << "failed on the phone:" << r.error;
        stopSource();
        mMaxTimer.stop();
        mResultTimer.stop();
        mState = Idle;
        emit failed(r.error);
        return;
    }
    if (r.hasPartial && !r.partial.isEmpty() && r.partial != mLastPartial) {
        mLastPartial = r.partial;
        emit partialText(r.partial);
    }
    if (r.hasFinal) {
        const QString text = r.final.trimmed();
        stopSource();
        mMaxTimer.stop();
        mResultTimer.stop();
        if (text.isEmpty()) {
            mState = Idle;
            emit failed(QStringLiteral("no-match"));
            return;
        }
        mFinal = text;
        mState = Done;
        qInfo() << "Voice: session" << mSid << "final text," << text.size() << "characters";
        emit finalText(text);
    }
}

void VoiceService::onResultTimeout()
{
    if (mState != Recognising)
        return;
    if (!mLastPartial.trimmed().isEmpty()) {
        qInfo() << "Voice: no final text from the phone, using the last partial";
        mFinal = mLastPartial.trimmed();
        mState = Done;
        emit finalText(mFinal);
        return;
    }
    mState = Idle;
    emit failed(QStringLiteral("timeout"));
}

void VoiceService::onSourceFailed(const QString &message)
{
    qWarning() << "Voice: audio source failed:" << message;
    if (mState == Listening) {
        cancel();
        emit failed(QStringLiteral("source"));
    }
}

void VoiceService::notifyChanged(VoiceNotifyChrc *chrc, bool on)
{
    qInfo() << "Voice:" << (chrc == mAudio ? "audio" : "event") << "notifications" << (on ? "on" : "off");
    if (on)
        return;
    // The phone unsubscribed or disconnected: it says again what it can
    // when it is back
    mPhoneReady = false;
    mPhoneUnavailable.clear();
    if (mState == Listening || mState == Recognising) {
        stopSource();
        mMaxTimer.stop();
        mResultTimer.stop();
        mPending.clear();
        mPendingFrames = 0;
        mState = Idle;
        emit failed(QStringLiteral("disconnected"));
    }
}
