/*
 * Copyright (C) 2026 eos-mainline contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef CALLOBSERVER_H
#define CALLOBSERVER_H

#include <QString>
#include <QStringList>
#include <QtGlobal>

/*
 * What the notification service tells whoever keeps the call log (CallLog) about the
 * phone's call cards: header only, so that the many test programs that link the
 * notification service need nothing else. All calls come from the phone's own frames
 * (nothing is polled), `phoneId` is the id the phone gave the card.
 */
class CallObserver
{
public:
    virtual ~CallObserver() = default;
    // The phone posted (or updated) the card of a call: ringing (the actions answer and
    // reject) or in progress (the action end). `packageName` is empty for a phone call
    // (a VoIP app's call has its package and is not a phone call).
    virtual void callCard(int phoneId, const QString &summary, const QString &body,
                          const QStringList &phoneActions, const QString &packageName) = 0;
    // The user pressed a button of the call card on the watch (reject, end, answer...)
    virtual void callAction(int phoneId, const QString &action) = 0;
    // The phone removed the card (the call ended, or stopped ringing)
    virtual void callGone(int phoneId) = 0;
    // The phone's "Missed call" notification (category missed_call); whenSeconds 0: not given
    virtual void missedCallCard(int phoneId, const QString &summary, const QString &body, qint64 whenSeconds) = 0;
};

#endif // CALLOBSERVER_H
