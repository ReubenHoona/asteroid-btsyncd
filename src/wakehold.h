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

#ifndef WAKEHOLD_H
#define WAKEHOLD_H

#include <QObject>
#include <QTimer>

class BackgroundActivity;

/*
 * Keeps the system out of autosleep while a write from the phone is handled
 * (Android's partial wakelock around a received message). A GATT write from
 * the phone wakes the watch; the kernel holds it awake for a short while
 * only, so without this autosleep may suspend between two chunks of a
 * notification or before lipstick has shown it.
 *
 * One mce cpu-keepalive session (libkeepalive BackgroundActivity), started
 * by the first write and stopped when the work is done:
 *   hold()     at the start of each write: starts the session if needed and
 *              (re)arms the safety limit (HOLD_LIMIT_MS after the last
 *              write, so a lost final chunk cannot keep the system up)
 *   release(g) when the write is handled: stops the session g ms later
 *              (time for lipstick, ngfd and mce to take over), unless
 *              another write holds again first
 * Nothing runs while idle: no timer, no D-Bus traffic.
 */
class WakeHold : public QObject
{
    Q_OBJECT
public:
    static const int HOLD_LIMIT_MS = 5000;
    static const int GRACE_NOTIFICATION_MS = 1000;
    static const int GRACE_SHORT_MS = 300;

    static void hold();
    static void release(int graceMs);

private:
    WakeHold();
    static WakeHold *instance();
    void doHold();
    void doRelease(int graceMs);

private slots:
    void stop();

private:
    BackgroundActivity *mActivity;
    QTimer mLimit;
    QTimer mGrace;
};

// hold() for the lifetime of a scope, release(graceMs) at its end
class WakeHoldScope
{
public:
    explicit WakeHoldScope(int graceMs = WakeHold::GRACE_SHORT_MS) : mGrace(graceMs) { WakeHold::hold(); }
    ~WakeHoldScope() { WakeHold::release(mGrace); }

private:
    Q_DISABLE_COPY(WakeHoldScope)
    int mGrace;
};

#endif // WAKEHOLD_H
