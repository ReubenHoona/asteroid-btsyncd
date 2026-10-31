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
#include "contactnames.h"

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
    check(capabilitiesXml().contains("<version>3</version>") && capabilitiesXml().contains("<actions>1</actions>"), "capabilities");
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

    // contact names for the recogniser (version 3), invented names
    check(capabilitiesXml("0123456789abcdef").contains("<version>3</version>")
          && capabilitiesXml("0123456789abcdef").contains("<names>0123456789abcdef</names>")
          && capabilitiesXml().contains("<names></names>"), "capabilities with the names' hash");
    r = parseReply("<cn><g>a1</g><i>1</i><n>3</n><v>Sam Carter</v><v>Sam</v><v>Lena &amp; Co</v></cn>");
    check(r.valid && r.names && !r.namesOff && r.namesGeneration == QLatin1String("a1") && r.namesIndex == 1
          && r.namesTotal == 3 && r.namesList == QStringList({"Sam Carter", "Sam", "Lena & Co"}), "a part of the names");
    r = parseReply("<cn><off/></cn>");
    check(r.valid && r.names && r.namesOff, "names off");
    check(!parseReply("<cn><g>a1</g><i>3</i><n>3</n><v>Sam</v></cn>").valid, "a part past the end");
    check(!parseReply("<cn><i>0</i><n>1</n><v>Sam</v></cn>").valid, "a part without its list");
    check(!parseReply("<cn><g>a1</g><i>x</i><n>1</n></cn>").valid, "a bad index");

    // cleaning: what asteroid-assistant-stt reads (no comment, no bonus, 96 bytes, no repeats)
    const QStringList c = ContactNames::clean({"Sam Carter", "sam carter", "  Priya\tNatarajan ", "#Lena", "Hana: 3",
                                              "1234", "", QString(120, QLatin1Char('a')), QString::fromUtf8("José Álvarez")});
    check(c.size() == 6 && c.at(0) == QLatin1String("Sam Carter") && c.at(1) == QLatin1String("Priya Natarajan")
          && c.at(2) == QLatin1String("Lena") && c.at(3) == QLatin1String("Hana 3") && c.at(4).toUtf8().size() == 96
          && c.at(5) == QString::fromUtf8("José Álvarez"), (QByteArray("names cleaned: ") + c.join(QLatin1Char('|')).left(120).toUtf8()).constData());
    QStringList many;
    for (int i = 0; i < 3500; i++)
        many << QStringLiteral("Contact %1x").arg(i);
    check(ContactNames::clean(many).size() == ContactNames::MaxPhrases, "at most 3000 phrases");
    check(ContactNames::content({"Sam Carter", "Sam"}) == "Sam Carter\nSam\n", "the file's lines");

    // the parts of a list, in any order; a new list drops the old one's parts
    ContactNames::Assembler as;
    check(!as.add("g1", 1, 3, {"Lena Okafor"}), "part 2 of 3");
    check(!as.add("g1", 0, 3, {"Sam Carter"}), "part 1 of 3");
    check(!as.add("g2", 0, 2, {"Freya Lindqvist"}) && as.partsHeld() == 1, "a new list starts over");
    check(as.add("g2", 1, 2, {"Marcus Bell"}), "its last part completes it");
    check(as.take() == QStringList({"Freya Lindqvist", "Marcus Bell"}) && as.partsHeld() == 0, "the list in order");
    check(!as.add("g3", 5, 2, {"x"}) && !as.add("g3", 0, 0, {"x"}) && !as.add("", 0, 1, {"x"}), "bad parts refused");

    fprintf(stdout, "%d failed\n", failures);
    return failures ? 1 : 0;
}
