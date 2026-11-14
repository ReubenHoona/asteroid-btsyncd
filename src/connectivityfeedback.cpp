/*
 * Copyright (C) 2026 eos-mainline contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <MDConfItem>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDebug>
#include <QVariantMap>

#include "connectivityfeedback.h"
#include "wakehold.h"

static const char NGF_SERVICE[] = "com.nokia.NonGraphicFeedback1.Backend";
static const char NGF_PATH[] = "/com/nokia/NonGraphicFeedback1";
static const char NGF_INTERFACE[] = "com.nokia.NonGraphicFeedback1";

bool ConnectivityFeedback::enabled()
{
    return MDConfItem(QString::fromLatin1(Key)).value(false).toBool();
}

void ConnectivityFeedback::play(const QString &event)
{
    QDBusMessage msg = QDBusMessage::createMethodCall(QLatin1String(NGF_SERVICE), QLatin1String(NGF_PATH),
                                                      QLatin1String(NGF_INTERFACE), QStringLiteral("Play"));
    // A haptic only, whatever the event file says: ngfd's resource plugin turns off every
    // media.* resource the request does not name
    QVariantMap props;
    props.insert(QStringLiteral("media.vibra"), true);
    msg << event << props;
    // send(), not call(): the reply (an event id) is not needed
    if (!QDBusConnection::systemBus().send(msg))
        qWarning() << "Connectivity feedback: could not send" << event;
}

void ConnectivityFeedback::setPhoneLink(bool connected)
{
    const bool changed = mKnown && connected != mConnected;
    mKnown = true;
    mConnected = connected;
    if (!changed || !enabled())
        return;
    qInfo() << "Connectivity feedback: the phone" << (connected ? "connected" : "disconnected");
    // The link change woke the watch; keep it up until ngfd has taken over
    WakeHoldScope hold(WakeHold::GRACE_NOTIFICATION_MS);
    play(QLatin1String(connected ? ConnectEvent : DisconnectEvent));
}
