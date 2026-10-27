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

#include "voicesource.h"
#include "voiceprotocol.h"

#include <QCoreApplication>
#include <QDebug>
#include <QMetaObject>

#ifdef BTSYNCD_HAVE_PULSE
#include <pulse/error.h>
#include <pulse/simple.h>
#endif

VoiceSource *VoiceSource::create(const QString &spec, QObject *parent)
{
    if (spec.isEmpty() || spec == QLatin1String("mic"))
        return new MicSource(parent);
    return new WavSource(spec, parent);
}

WavSource::WavSource(const QString &path, QObject *parent)
    : VoiceSource(parent), mPath(path)
{
    mTimer.setTimerType(Qt::PreciseTimer);
    mTimer.setInterval(10);
    connect(&mTimer, &QTimer::timeout, this, &WavSource::tick);
}

bool WavSource::start(QString &error)
{
    if (!VoiceProtocol::readWav(mPath, mPcm, error))
        return false;
    // whole frames: pad the last one with silence
    const int rest = mPcm.size() % VoiceProtocol::FramePcmBytes;
    if (rest)
        mPcm.append(QByteArray(VoiceProtocol::FramePcmBytes - rest, '\0'));
    mPos = 0;
    mSent = 0;
    mClock.start();
    mTimer.start();
    QTimer::singleShot(0, this, &WavSource::tick);
    return true;
}

void WavSource::stop()
{
    mTimer.stop();
}

// Sends every frame that is due by now, like a microphone would deliver them
void WavSource::tick()
{
    if (!mTimer.isActive())
        return;
    const qint64 due = mClock.elapsed() / 20 + 1;
    while (mSent < due && mPos < mPcm.size()) {
        emit frame(mPcm.mid(mPos, VoiceProtocol::FramePcmBytes));
        mPos += VoiceProtocol::FramePcmBytes;
        mSent++;
        if (!mTimer.isActive())
            return;     // stopped from a frame handler
    }
    if (mPos >= mPcm.size()) {
        mTimer.stop();
        emit finished();
    }
}

MicSource::MicSource(QObject *parent) : VoiceSource(parent) {}

MicSource::~MicSource()
{
    stop();
}

bool MicSource::start(QString &error)
{
#ifdef BTSYNCD_HAVE_PULSE
    Q_UNUSED(error)
    if (mThread)
        return true;
    mRunning = std::make_shared<std::atomic<bool>>(true);
    const std::shared_ptr<std::atomic<bool>> running = mRunning;
    const QPointer<MicSource> self(this);
    mThread = QThread::create([running, self] { run(running, self); });
    // It deletes itself when it ends, also when it outlives this source
    connect(mThread, &QThread::finished, mThread, &QObject::deleteLater);
    mThread->start();
    return true;
#else
    error = QStringLiteral("built without PulseAudio");
    return false;
#endif
}

void MicSource::stop()
{
    if (mRunning)
        *mRunning = false;
    if (mThread) {
        // A read returns within one fragment (20 ms), but connecting to a
        // PulseAudio that hangs does not: then let the thread end on its own
        // (deleting a running QThread aborts the daemon)
        if (!mThread->wait(2000))
            qWarning() << "Voice: the microphone thread is still blocked in PulseAudio, leaving it to finish";
        mThread = nullptr;
    }
}

void MicSource::run(std::shared_ptr<std::atomic<bool>> running, QPointer<MicSource> self)
{
#ifdef BTSYNCD_HAVE_PULSE
    // Results go to the main thread, where self is checked: the source may be
    // gone by then
    pa_sample_spec ss;
    ss.format = PA_SAMPLE_S16LE;
    ss.rate = VoiceProtocol::SampleRate;
    ss.channels = 1;
    pa_buffer_attr attr;
    attr.maxlength = uint32_t(-1);
    attr.tlength = uint32_t(-1);
    attr.prebuf = uint32_t(-1);
    attr.minreq = uint32_t(-1);
    attr.fragsize = VoiceProtocol::FramePcmBytes;
    int err = 0;
    pa_simple *pa = pa_simple_new(nullptr, "asteroid-btsyncd", PA_STREAM_RECORD, nullptr,
                                  "voice reply", &ss, nullptr, &attr, &err);
    if (!pa) {
        const QString msg = QStringLiteral("PulseAudio: ") + QString::fromUtf8(pa_strerror(err));
        QMetaObject::invokeMethod(qApp, [self, msg] { if (self) emit self->failed(msg); }, Qt::QueuedConnection);
        return;
    }
    QByteArray buf(VoiceProtocol::FramePcmBytes, '\0');
    while (*running) {
        if (pa_simple_read(pa, buf.data(), size_t(buf.size()), &err) < 0) {
            const QString msg = QStringLiteral("PulseAudio read: ") + QString::fromUtf8(pa_strerror(err));
            QMetaObject::invokeMethod(qApp, [self, msg] { if (self) emit self->failed(msg); }, Qt::QueuedConnection);
            break;
        }
        const QByteArray copy = buf;
        QMetaObject::invokeMethod(qApp, [self, running, copy] { if (self && *running) emit self->frame(copy); },
                                  Qt::QueuedConnection);
    }
    pa_simple_free(pa);
#else
    Q_UNUSED(running)
    Q_UNUSED(self)
#endif
}
