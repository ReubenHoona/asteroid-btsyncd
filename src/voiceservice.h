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

#ifndef VOICESERVICE_H
#define VOICESERVICE_H

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QTimer>

#include "service.h"
#include "voiceprotocol.h"

class NotificationService;
class VoiceService;
class VoiceSource;

/*
 * Watch to phone notifications: the session events (XML, joined by the
 * phone until </voice>) and the audio frames (binary, whole frames per
 * notification).
 */
class VoiceNotifyChrc : public Characteristic
{
    Q_OBJECT
    Q_PROPERTY(QByteArray Value READ getValue NOTIFY valueChanged)

public:
    VoiceNotifyChrc(QDBusConnection bus, int index, const QString &uuid, VoiceService *service);
    void send(const QByteArray &value);
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
    VoiceService *mVoice;
    bool mNotifying = false;
};

/*
 * Session bus API for the launcher: org.asteroidos.btsyncd.Voice on
 * /org/asteroidos/btsyncd/voice. One session at a time.
 *   Start(u notificationId, s source) -> u session id, 0 if it cannot start
 *     (then Error() says why). source: "" = the configured source (env
 *     BTSYNCD_VOICE_SOURCE, else dconf /org/asteroidos/btsyncd/voice-source,
 *     else the microphone), "mic", or the path of a 16 kHz mono WAV (test);
 *     "phone:" before any of them: the phone's recogniser whatever the
 *     setting.
 *     Where the speech is recognised: dconf /org/asteroidos/voice/recognition
 *     "watch" (the default: asteroid-assistant's Dictate, the same on-watch
 *     recogniser as the assistant; the phone's only when the watch's cannot
 *     load) or "phone" (streamed to Gadgetbridge, as before).
 *   Stop()   the user finished speaking: the phone gives the final text
 *   Cancel() drop the session
 *   Send()   reply to the notification with the final text (the existing
 *            reply path, as Notifications.Reply)
 *   Available() speech can be recognised: on the watch, or by a connected
 *     phone that does voice replies
 *   State()  idle | listening | recognising | done
 * Signals Partial(s text), Final(s text), Error(s code).
 */
class VoiceDBusApi : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", BTSYNCD_VOICE_IFACE)

public:
    explicit VoiceDBusApi(VoiceService *service);

public slots:
    uint Start(uint notificationId, const QString &source);
    bool Stop();
    bool Cancel();
    bool Send();
    bool Available();
    QString State();

signals:
    void Partial(const QString &text);
    void Final(const QString &text);
    void Error(const QString &code);

private:
    VoiceService *mService;
};

/*
 * Voice replies (PROTOCOL-voice.md): the watch streams IMA ADPCM audio to
 * the phone, the phone sends back partial and final text.
 */
class VoiceService : public Service
{
    Q_OBJECT
public:
    VoiceService(int index, QDBusConnection bus, NotificationService *notifications, QObject *parent = 0);

    uint start(uint notificationId, const QString &source);
    bool stop();
    bool cancel();
    bool send();
    bool available() const;
    QString state() const;

    // From the characteristics
    void onResult(const QByteArray &xml, int mtu);
    void notifyChanged(VoiceNotifyChrc *chrc, bool on);

signals:
    void partialText(const QString &text);
    void finalText(const QString &text);
    void failed(const QString &code);

private slots:
    void onLocalPartial(uint session, const QString &text);
    void onLocalResult(uint session, const QString &json);
    void onFrame(const QByteArray &pcm);
    void onSourceFailed(const QString &message);
    void onResultTimeout();

private:
    enum State { Idle, Listening, Recognising, Done };

    void sendEvent(const QByteArray &xml);
    void flush();
    void stopSource();
    void failLater(const QString &code);
    int framesPerNotification() const;
    static QString sourceSpec(const QString &requested);
    bool phoneAvailable() const;
    /* The watch's recogniser is there (asteroid-assistant, its helper and
     * model) and the setting says the watch */
    static bool localWanted();
    static bool localInstalled();
    uint startPhone(uint notificationId, const QString &source);
    // On the watch: asteroid-assistant's Dictate, its signals relayed
    uint startLocal(uint notificationId, const QString &source);
    void endLocal();

    NotificationService *mNotifications;
    VoiceNotifyChrc *mEvent;
    VoiceNotifyChrc *mAudio;
    VoiceDBusApi *mApi;

    // The phone: what it said in <st>, and the ATT MTU it writes with
    bool mPhoneReady = false;
    QString mPhoneUnavailable;
    int mMtu = 0;

    // The session
    State mState = Idle;
    quint8 mSid = 0;
    uint mNotificationId = 0;
    quint16 mSeq = 0;
    int mFrames = 0;
    VoiceProtocol::AdpcmState mAdpcm;
    QByteArray mPending;
    int mPendingFrames = 0;
    VoiceSource *mSource = nullptr;
    QString mLastPartial;
    QString mFinal;
    // a session of asteroid-assistant's (0: none), and what to fall back on
    uint mLocalSession = 0;
    QString mLocalSource;
    QTimer mMaxTimer;
    QTimer mResultTimer;
};

#endif // VOICESERVICE_H
