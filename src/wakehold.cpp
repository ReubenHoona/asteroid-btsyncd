/*
 * Copyright (C) 2026 - Reuben Horner <reubarb.scons@gmail.com>
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

#include "wakehold.h"

#include <backgroundactivity.h>

WakeHold::WakeHold()
    : QObject(nullptr), mActivity(new BackgroundActivity(this))
{
    mLimit.setSingleShot(true);
    mGrace.setSingleShot(true);
    connect(&mLimit, &QTimer::timeout, this, &WakeHold::stop);
    connect(&mGrace, &QTimer::timeout, this, &WakeHold::stop);
}

WakeHold *WakeHold::instance()
{
    static WakeHold *self = new WakeHold();
    return self;
}

void WakeHold::hold()
{
    instance()->doHold();
}

void WakeHold::release(int graceMs)
{
    instance()->doRelease(graceMs);
}

void WakeHold::doHold()
{
    mGrace.stop();
    mLimit.start(HOLD_LIMIT_MS);
    // run() is a no-op while the session is already running; otherwise one
    // asynchronous req_cpu_keepalive_start to mce (no reply is waited for)
    if (!mActivity->isRunning())
        mActivity->run();
}

void WakeHold::doRelease(int graceMs)
{
    if (!mActivity->isRunning())
        return;
    // the grace never goes past the limit of the session
    const int left = qMax(0, mLimit.remainingTime());
    graceMs = qBound(0, graceMs, left);
    if (graceMs == 0) {
        stop();
        return;
    }
    if (!mGrace.isActive() || mGrace.remainingTime() < graceMs)
        mGrace.start(graceMs);
}

void WakeHold::stop()
{
    mLimit.stop();
    mGrace.stop();
    if (mActivity->isRunning())
        mActivity->stop();
}
