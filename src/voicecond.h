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

#ifndef VOICECOND_H
#define VOICECOND_H

#include <QtGlobal>

/*
 * The watch microphone's audio made fit for the phone's recogniser, and the
 * end of speech found on the watch (the phone's recogniser does not end a
 * piped stream by itself: the first real use, 2026-10-31, listened to the
 * 15 s limit). The microphone (stt5's measurements of the eos watch):
 *   - about 30 ms of zeros, then a click of up to full scale;
 *   - a DC offset of 500-6000 LSB that settles over 1.5-3.5 s (the hub's
 *     high-pass), in every utterance;
 *   - speech at -42 to -26 dBFS, the noise floor 25 dB below.
 * Per 20 ms frame: the first 100 ms muted, a DC blocker (one pole at ~10 Hz),
 * a gain (default x4, +12 dB) with a soft limit, the level, and an energy
 * end-of-speech detector against a tracked noise floor:
 *   speech starts after 80 ms above floor + 9 dB (and above -55 dBFS);
 *   it ends after endpointMs below; maxMs after the start it ends anyway;
 *   no speech in noSpeechMs: it gives up (0: never).
 * Qt Core only, for the tests.
 */
class VoiceConditioner
{
public:
    enum Event { None, SpeechStart, EndOfSpeech, MaxReached, NoSpeech };

    struct Options {
        double gain = 4.0;
        int endpointMs = 1500;      // 0: no end of speech (the user taps Done)
        int maxMs = 30000;          // after speech started
        int noSpeechMs = 0;         // 0: wait
    };

    void reset(const Options &o);
    // One frame of 320 samples, conditioned in place; what happened with it
    Event process(qint16 *pcm, int n);
    // The last frame's level, 0..1 (about -60..0 dBFS), for the listening animation
    double level() const { return mLevel; }
    double peakDb() const { return mPeakDb; }
    int speechStartMs() const { return mStartFrame < 0 ? -1 : mStartFrame * 20; }
    int framesSeen() const { return mFrame; }
    bool ended() const { return mEnded; }

private:
    Options mOpt;
    double mX1 = 0, mY1 = 0;        // the DC blocker's state
    double mFloorDb = -60;
    double mLevel = 0;
    double mPeakDb = -90;
    int mFrame = 0;
    int mRun = 0;                   // consecutive speech frames before the start
    int mSilent = 0;                // consecutive quiet frames after it
    int mStartFrame = -1;
    bool mEnded = false;
};

#endif // VOICECOND_H
