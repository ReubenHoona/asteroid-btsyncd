/*
 * Copyright (C) 2026 eos-mainline contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef PHONERELAY_H
#define PHONERELAY_H

#include <QByteArray>
#include <QString>

#include <functional>

/*
 * What btsyncd's own parts need from the assistant relay (AssistantService) to ask the
 * phone for something over an act: header only, so that CallAudio and its test program
 * need not link the relay.
 */
class PhoneRelay
{
public:
    virtual ~PhoneRelay() = default;
    // The connected phone lists this act (assistantprotocol.h) and the relay is up
    virtual bool relayHas(const QString &act) const = 0;
    // Sends the act (its JSON, {"a":"cvol",...}); done is called once with the phone's result,
    // or with {"ok":false,"er":"timeout"|"disconnected"} from btsyncd. False: not sent (no
    // phone, or it does not list the act); done is not called then.
    virtual bool relayAct(const QByteArray &json, const std::function<void(const QString &)> &done) = 0;
};

#endif // PHONERELAY_H
