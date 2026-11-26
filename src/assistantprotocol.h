/*
 * Copyright (C) 2026 eos
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

#ifndef ASSISTANTPROTOCOL_H
#define ASSISTANTPROTOCOL_H

#include <QByteArray>
#include <QString>
#include <QStringList>

/*
 * The assistant relay (service 0000D071): the watch's assistant
 * (asteroid-assistant) asks a question, the phone (Gadgetbridge) posts it to
 * the OpenAI-compatible chat completions URL set on the phone and sends the
 * answer back. Only the request's text goes, never audio.
 *
 * Watch to phone, notifications of D001, joined by the phone until </ask>:
 *   <ask><id>7</id><rq>{"messages":[...],"max_tokens":96,...}</rq></ask>
 *     rq: the request body as JSON (XML-escaped); the phone adds the URL,
 *     its API key (Authorization: Bearer) and its model if the watch sent none
 *   <ask><id>7</id><cancel/></ask>      the watch does not want it any more
 * Phone to watch, writes of D002 (encrypt-authenticated), joined until </asr>:
 *   <asr><st>ready</st></asr>           the phone relays (a URL is set)
 *   <asr><st>unavailable</st><er>no-url</er></asr>
 *   <asr><id>7</id><tx>The answer.</tx></asr>
 *   <asr><id>7</id><er>timeout|offline|server|auth|bad-request</er></asr>
 * D003 (read): <capabilities><version>4</version><actions>1</actions><names>HASH</names>
 *               <events>nav,cam</events></capabilities>
 *   names: the watch takes the phone's contact names (version 3) and has the
 *   list whose hash is HASH (contactnames.h; empty: none)
 *   events: the phone events the watch takes (version 4)
 *
 * Actions on the phone (version 2: the assistant's directions and text
 * messages; Gadgetbridge 41+). The phone says which it does in every status
 * write, with or without an answer server:
 *   <asr><st>ready</st><ac>nav,contacts,sms</ac></asr>
 *   (no <ac>: an older Gadgetbridge, no actions)
 * Watch to phone, on D001 like the questions, joined until </act>:
 *   <act><id>9</id><js>{"a":"nav","q":"Riverbend Park","mode":"walking"}</js></act>
 *   <act><id>9</id><js>{"a":"contacts","q":"sam carter i am late","split":true}</js></act>
 *   <act><id>9</id><js>{"a":"sms","k":"c1","tx":"I'm late","dry":false}</js></act>
 *   <act><id>9</id><cancel/></act>
 *   js: the action as JSON (XML-escaped); btsyncd checks only its "a"
 * Phone to watch, writes of D002, joined until </acr>:
 *   <acr><id>9</id><js>{"ok":true,...} or {"ok":false,"er":"..."}</js></acr>
 *   (the result of each action: asteroid-launcher's assistant/PhoneActions.qml)
 * btsyncd itself answers {"ok":false,"er":"timeout"} or {"ok":false,"er":"disconnected"}.
 * Actions of version 4 (Gadgetbridge since W2-gb-data), after nav, contacts,
 * sms, call, hangup: convs, msgs, msend, mread (the phone's conversations,
 * see messageservice.h), rsvp, open (the phone's calendar or language
 * settings), wifipw (a Wi-Fi password typed on the phone: the result carries
 * it, never logged), cam (the camera remote). Each waits for its result
 * as long as actTimeoutMs() says.
 *
 * Events the phone sends on its own (version 4), phone to watch on D002,
 * joined until </pe>:
 *   <pe><k>nav</k><js>{"st":"nav","ins":"..."}</js></pe>
 *   <pe><k>cam</k><js>{"ev":"shot"}</js></pe>
 *   k: the kind, one of eventKinds() (the capabilities' <events>: the phone
 *   sends a kind only when the watch lists it); js: the JSON object. They
 *   leave on the Assistant D-Bus interface as PhoneEvent(kind, json).
 *
 * Contact names for the watch's speech recogniser (version 3; the user agreed to
 * names only, never numbers), phone to watch on D002, each part joined until </cn>:
 *   <cn><g>GEN</g><i>0</i><n>5</n><v>Sam Carter</v><v>Sam</v>...</cn>
 *     part i of n of the list GEN (a new GEN starts over); each part at most a
 *     few kB, so 500-1000 names take several parts
 *   <cn><off/></cn>     the setting is off on the phone: the watch drops its list
 */
namespace AssistantProtocol {

QByteArray askXml(uint id, const QByteArray &requestJson);
QByteArray cancelXml(uint id);
QByteArray actXml(uint id, const QByteArray &actionJson);
QByteArray actCancelXml(uint id);
QByteArray capabilitiesXml(const QByteArray &namesHash = QByteArray());

// The kinds of <pe> the watch takes: nav, cam
QStringList eventKinds();
// How long an action waits for the phone's result (ms): wifipw 125 s (the
// phone waits 120 s for the typed password), msend and sms 40 s, convs and
// msgs 20 s, any other 15 s
int actTimeoutMs(const QString &action);

struct Reply {
    bool valid = false;
    uint id = 0;
    QString state;     // ready | unavailable (a status write)
    QString text;      // the answer
    QString error;     // the reason, with unavailable or for a question
    bool act = false;  // <acr>: an action's result, in json
    QString json;
    bool hasActions = false;    // a status that says what the phone does
    QStringList actions;        // nav, contacts, sms
    bool names = false;         // <cn>: contact names (a part, or off)
    bool namesOff = false;
    QString namesGeneration;
    int namesIndex = -1;
    int namesTotal = 0;
    QStringList namesList;
    bool event = false;         // <pe>: a phone event
    QString eventKind;
    QString eventJson;
};
Reply parseReply(const QByteArray &xml);

}

#endif // ASSISTANTPROTOCOL_H
