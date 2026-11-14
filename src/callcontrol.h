/*
 * Copyright (C) 2026 eos-mainline contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef CALLCONTROL_H
#define CALLCONTROL_H

/*
 * What the notification service needs from the watch's hands-free link
 * (CallAudio) to run the buttons of a call card: the launcher still sends
 * the card's actions (answer, reject, end), and the card's owner decides where
 * they go. A call the hands-free link knows is handled on the watch (the call
 * audio too, as stock Wear OS does); everything else goes to the phone through
 * Gadgetbridge as before.
 */
class CallControl
{
public:
    virtual ~CallControl() = default;
    // Answer the ringing call on the watch, its audio included. False: not
    // possible (no hands-free link, no ringing call, "Answer calls on watch"
    // off): the answer goes to the phone and the audio stays there.
    virtual bool answerOnWatch() = 0;
    // Decline the ringing call or end the call in progress on the
    // hands-free link. False: it does not know a call.
    virtual bool hangUp() = 0;
};

#endif // CALLCONTROL_H
