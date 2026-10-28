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

QByteArray capabilitiesXml()
{
    return QByteArrayLiteral("<capabilities><version>1</version></capabilities>");
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
            if (n == QLatin1String("asr")) {
                inside = true;
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
            } else {
                x.skipCurrentElement();
            }
        } else if (x.isEndElement() && x.name() == QLatin1String("asr")) {
            r.valid = r.id > 0 || !r.state.isEmpty();
            return r;
        }
    }
    return Reply();
}

}
