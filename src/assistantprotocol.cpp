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

#include "assistantprotocol.h"

#include <QXmlStreamReader>
#include <QXmlStreamWriter>

namespace AssistantProtocol {

QByteArray askXml(uint id, const QByteArray &requestJson)
{
    QByteArray out;
    QXmlStreamWriter w(&out);
    w.writeStartElement(QStringLiteral("ask"));
    w.writeTextElement(QStringLiteral("id"), QString::number(id));
    w.writeTextElement(QStringLiteral("rq"), QString::fromUtf8(requestJson));
    w.writeEndElement();
    return out;
}

QByteArray cancelXml(uint id)
{
    return "<ask><id>" + QByteArray::number(id) + "</id><cancel/></ask>";
}

QByteArray actXml(uint id, const QByteArray &actionJson)
{
    QByteArray out;
    QXmlStreamWriter w(&out);
    w.writeStartElement(QStringLiteral("act"));
    w.writeTextElement(QStringLiteral("id"), QString::number(id));
    w.writeTextElement(QStringLiteral("js"), QString::fromUtf8(actionJson));
    w.writeEndElement();
    return out;
}

QByteArray actCancelXml(uint id)
{
    return "<act><id>" + QByteArray::number(id) + "</id><cancel/></act>";
}

QByteArray capabilitiesXml(const QByteArray &namesHash)
{
    return "<capabilities><version>3</version><actions>1</actions><names>" + namesHash + "</names></capabilities>";
}

// <cn>...</cn>: a part of the contact names, or <off/>
static Reply parseNames(QXmlStreamReader &x)
{
    Reply r;
    r.names = true;
    while (!x.atEnd()) {
        x.readNext();
        if (x.isStartElement()) {
            const QStringView n = x.name();
            if (n == QLatin1String("off")) {
                r.namesOff = true;
                x.skipCurrentElement();
            } else if (n == QLatin1String("g")) {
                r.namesGeneration = x.readElementText().trimmed().left(32);
            } else if (n == QLatin1String("i")) {
                bool ok = false;
                r.namesIndex = x.readElementText().trimmed().toInt(&ok);
                if (!ok)
                    return Reply();
            } else if (n == QLatin1String("n")) {
                bool ok = false;
                r.namesTotal = x.readElementText().trimmed().toInt(&ok);
                if (!ok)
                    return Reply();
            } else if (n == QLatin1String("v")) {
                r.namesList << x.readElementText();
            } else {
                x.skipCurrentElement();
            }
        } else if (x.isEndElement() && x.name() == QLatin1String("cn")) {
            r.valid = r.namesOff || (!r.namesGeneration.isEmpty() && r.namesTotal > 0
                                     && r.namesIndex >= 0 && r.namesIndex < r.namesTotal);
            return r.valid ? r : Reply();
        }
    }
    return Reply();
}

Reply parseReply(const QByteArray &xml)
{
    Reply r;
    QXmlStreamReader x(xml);
    bool inside = false;
    while (!x.atEnd()) {
        x.readNext();
        if (x.isStartElement()) {
            const QStringView n = x.name();
            if (!inside && n == QLatin1String("cn"))
                return parseNames(x);
            if (!inside && (n == QLatin1String("asr") || n == QLatin1String("acr"))) {
                inside = true;
                r.act = n == QLatin1String("acr");
            } else if (!inside) {
                return Reply();
            } else if (n == QLatin1String("id")) {
                bool ok = false;
                r.id = x.readElementText().trimmed().toUInt(&ok);
                if (!ok)
                    return Reply();
            } else if (n == QLatin1String("st")) {
                r.state = x.readElementText().trimmed();
            } else if (n == QLatin1String("tx")) {
                r.text = x.readElementText();
            } else if (n == QLatin1String("er")) {
                r.error = x.readElementText().trimmed();
            } else if (n == QLatin1String("js")) {
                r.json = x.readElementText();
            } else if (n == QLatin1String("ac")) {
                r.hasActions = true;
                for (const QString &a : x.readElementText().split(QLatin1Char(','), Qt::SkipEmptyParts))
                    r.actions << a.trimmed();
            } else {
                x.skipCurrentElement();
            }
        } else if (x.isEndElement() && (x.name() == QLatin1String("asr") || x.name() == QLatin1String("acr"))) {
            // an action's result has an id and its JSON; a status or an answer as before
            r.valid = r.act ? (r.id > 0 && !r.json.isEmpty()) : (r.id > 0 || !r.state.isEmpty());
            return r;
        }
    }
    return Reply();
}

}
