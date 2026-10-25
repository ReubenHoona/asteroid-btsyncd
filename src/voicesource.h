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

#ifndef VOICESOURCE_H
#define VOICESOURCE_H

#include <QByteArray>
#include <QElapsedTimer>
#include <QObject>
#include <QThread>
#include <QTimer>
#include <atomic>

/*
 * Where the voice audio comes from: 16 kHz mono PCM16 in 20 ms frames
 * (640 bytes), delivered on the main thread.
 */
class VoiceSource : public QObject
{
    Q_OBJECT
public:
    using QObject::QObject;
    virtual bool start(QString &error) = 0;
    virtual void stop() = 0;

    // "mic" (PulseAudio default source) or the path of a WAV file
    static VoiceSource *create(const QString &spec, QObject *parent);

signals:
    void frame(const QByteArray &pcm);
    // A WAV source ran out (the microphone never does)
    void finished();
    void failed(const QString &error);
};

/* Test source: a 16 kHz mono 16-bit WAV, played out in real time */
class WavSource : public VoiceSource
{
    Q_OBJECT
public:
    WavSource(const QString &path, QObject *parent);
    bool start(QString &error) override;
    void stop() override;

private slots:
    void tick();

private:
    QString mPath;
    QByteArray mPcm;
    int mPos = 0;
    qint64 mSent = 0;
    QTimer mTimer;
    QElapsedTimer mClock;
};

/* The microphone through PulseAudio (pa_simple), read on its own thread */
class MicSource : public VoiceSource
{
    Q_OBJECT
public:
    explicit MicSource(QObject *parent);
    ~MicSource() override;
    bool start(QString &error) override;
    void stop() override;

private:
    void run();
    QThread *mThread = nullptr;
    std::atomic<bool> mRunning{false};
};

#endif // VOICESOURCE_H
