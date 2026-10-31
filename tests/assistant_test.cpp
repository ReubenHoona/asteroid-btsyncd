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

// The assistant relay's XML (see assistantprotocol.h), without D-Bus

#include "assistantprotocol.h"

#include <QCoreApplication>
#include <QXmlStreamReader>
#include <cstdio>

static int failures = 0;
static void check(bool ok, const char *what)
{
    if (!ok) {
        failures++;
        fprintf(stdout, "FAIL %s\n", what);
    } else {
        fprintf(stdout, "ok   %s\n", what);
    }
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    using namespace AssistantProtocol;

    // the request JSON survives the XML (quotes, <, &)
    const QByteArray json = R"({"messages":[{"role":"user","content":"is 3 < 4 & \"why\"?"}],"max_tokens":96})";
    const QByteArray ask = askXml(7, json);
    check(ask.startsWith("<ask><id>7</id><rq>") && ask.endsWith("</rq></ask>"), "ask framing");
    QXmlStreamReader x(ask);
    QString rq;
    while (!x.atEnd()) {
        x.readNext();
        if (x.isStartElement() && x.name() == QLatin1String("rq"))
            rq = x.readElementText();
    }
    check(rq.toUtf8() == json, "the request comes back unchanged");
    check(cancelXml(7) == "<ask><id>7</id><cancel/></ask>", "cancel");

    Reply r = parseReply("<asr><st>ready</st></asr>");
    check(r.valid && r.state == QLatin1String("ready"), "ready");
    r = parseReply("<asr><st>unavailable</st><er>no-url</er></asr>");
    check(r.valid && r.state == QLatin1String("unavailable") && r.error == QLatin1String("no-url"), "unavailable");
    r = parseReply("<asr><id>7</id><tx>About 384,400 km &amp; a bit.</tx></asr>");
    check(r.valid && r.id == 7 && r.text == QLatin1String("About 384,400 km & a bit.") && r.error.isEmpty(), "answer");
    r = parseReply("<asr><id>8</id><er>timeout</er></asr>");
    check(r.valid && r.id == 8 && r.error == QLatin1String("timeout"), "error");
    check(!parseReply("<vr><id>8</id></vr>").valid, "not ours");
    check(!parseReply("<asr><id>x</id><tx>a</tx></asr>").valid, "bad id");
    check(!parseReply("<asr><tx>no id</tx></asr>").valid, "no id, no state");

    // actions on the phone (version 2)
    check(capabilitiesXml().contains("<version>2</version>") && capabilitiesXml().contains("<actions>1</actions>"), "capabilities");
    const QByteArray nav = R"({"a":"nav","q":"Riverbend Park & <Mount Alder>","mode":"walking"})";
    const QByteArray act = actXml(9, nav);
    check(act.startsWith("<act><id>9</id><js>") && act.endsWith("</js></act>") && !act.contains("<Mount"), "act framing, escaped");
    QXmlStreamReader xa(act);
    QString js;
    while (!xa.atEnd()) {
        xa.readNext();
        if (xa.isStartElement() && xa.name() == QLatin1String("js"))
            js = xa.readElementText();
    }
    check(js.toUtf8() == nav, "the action comes back unchanged");
    check(actCancelXml(9) == "<act><id>9</id><cancel/></act>", "act cancel");
    r = parseReply("<asr><st>ready</st><ac>nav,contacts,sms</ac></asr>");
    check(r.valid && r.hasActions && r.actions == QStringList({"nav", "contacts", "sms"}), "status with actions");
    r = parseReply("<asr><st>unavailable</st><er>no-url</er><ac>nav</ac></asr>");
    check(r.valid && r.state == QLatin1String("unavailable") && r.actions == QStringList({"nav"}), "actions without an answer server");
    r = parseReply("<asr><st>ready</st></asr>");
    check(r.valid && !r.hasActions && r.actions.isEmpty(), "an older phone: no actions");
    r = parseReply(R"(<acr><id>9</id><js>{"ok":true,"how":"started"}</js></acr>)");
    check(r.valid && r.act && r.id == 9 && r.json == QLatin1String(R"({"ok":true,"how":"started"})"), "action result");
    r = parseReply(R"(<acr><id>10</id><js>{"ok":true,"c":[{"n":"Sam &amp; Co"}]}</js></acr>)");
    check(r.valid && r.json.contains(QLatin1String("Sam & Co")), "action result unescaped");
    check(!parseReply("<acr><js>{}</js></acr>").valid, "action result without an id");
    check(!parseReply("<acr><id>3</id></acr>").valid, "action result without JSON");

    fprintf(stdout, "%d failed\n", failures);
    return failures ? 1 : 0;
}
