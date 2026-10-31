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

#include "voicecond.h"

#include <cmath>

static const int FrameMs = 20;
// the start's click, and the DC step's tail through the blocker (6000 LSB x 0.996^1600 = 10)
static const int MutedFrames = 5;
static const int StartFrames = 4;       // 80 ms of speech starts it
static const double AboveFloorDb = 9;
static const double MinSpeechDb = -55;

void VoiceConditioner::reset(const Options &o)
{
    mOpt = o;
    mX1 = mY1 = 0;
    mFloorDb = -60;
    mLevel = 0;
    mPeakDb = -90;
    mFrame = 0;
    mRun = 0;
    mSilent = 0;
    mStartFrame = -1;
    mEnded = false;
}

VoiceConditioner::Event VoiceConditioner::process(qint16 *pcm, int n)
{
    const int frame = mFrame++;
    double sum = 0;
    for (int i = 0; i < n; i++) {
        const double x = pcm[i];
        // DC blocker: y = x - x1 + R y1, R = 0.996 (a pole near 10 Hz at 16 kHz)
        const double y = x - mX1 + 0.996 * mY1;
        mX1 = x;
        mY1 = y;
        double v = frame < MutedFrames ? 0.0 : y * mOpt.gain;
        // a soft limit above 3/4 of full scale, so loud speech is not clipped hard
        const double a = std::fabs(v);
        if (a > 24576)
            v = (v < 0 ? -1 : 1) * qMin(32767.0, 24576 + (a - 24576) * 0.25);
        pcm[i] = qint16(qBound(-32768.0, std::round(v), 32767.0));
        sum += double(pcm[i]) * pcm[i];
    }
    const double rms = n > 0 ? std::sqrt(sum / n) : 0;
    const double db = rms > 1 ? 20 * std::log10(rms / 32768.0) : -90;
    mLevel = qBound(0.0, (db + 60) / 60, 1.0);
    if (frame < MutedFrames)
        return None;
    mPeakDb = qMax(mPeakDb, db);
    if (mEnded)
        return None;

    // the noise floor: from the first frame heard; down at once, quickly up to
    // frames near it (the room), slowly under speech (1 dB a second)
    if (frame == MutedFrames || db < mFloorDb)
        mFloorDb = db;
    else if (db < mFloorDb + 6)
        mFloorDb += 0.3 * (db - mFloorDb);
    else
        mFloorDb += 0.02;
    const bool speech = db > mFloorDb + AboveFloorDb && db > MinSpeechDb;

    if (mStartFrame < 0) {
        mRun = speech ? mRun + 1 : 0;
        if (mRun >= StartFrames) {
            mStartFrame = frame - StartFrames + 1;
            mSilent = 0;
            return SpeechStart;
        }
        if (mOpt.noSpeechMs > 0 && (frame + 1) * FrameMs >= mOpt.noSpeechMs) {
            mEnded = true;
            return NoSpeech;
        }
        return None;
    }
    mSilent = speech ? 0 : mSilent + 1;
    if (mOpt.endpointMs > 0 && mSilent * FrameMs >= mOpt.endpointMs) {
        mEnded = true;
        return EndOfSpeech;
    }
    if (mOpt.maxMs > 0 && (frame + 1 - mStartFrame) * FrameMs >= mOpt.maxMs) {
        mEnded = true;
        return MaxReached;
    }
    return None;
}
