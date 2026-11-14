/*
 * Copyright (C) 2026 eos-mainline contributors
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

#include "messageservice.h"
#include "assistantservice.h"
#include "common.h"

#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

MessageService::MessageService(AssistantService *assistant, QDBusConnection bus, QObject *parent)
    : QObject(parent), mAssistant(assistant), mBus(bus)
{
    connect(assistant, &AssistantService::actResult, this, &MessageService::onActResult);
    if (!bus.registerObject(QStringLiteral(BTSYNCD_MESSAGES_PATH), this,
                            QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllSignals))
        fprintf(stderr, "MessageService: cannot register %s\n", BTSYNCD_MESSAGES_PATH);
}

bool MessageService::validKey(const QString &key)
{
    static const QRegularExpression re(QStringLiteral("^[A-Za-z0-9_-]{1,64}$"));
    return re.match(key).hasMatch();
}

static QByteArray toJson(const QJsonObject &o)
{
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}

QByteArray MessageService::convsRequest(uint offset, uint count)
{
    return toJson({{QStringLiteral("a"), QStringLiteral("convs")},
                   {QStringLiteral("off"), int(qMin(offset, 100000u))},
                   {QStringLiteral("n"), int(qBound(1u, count, 30u))}});
}

QByteArray MessageService::msgsRequest(const QString &key, qint64 before, uint count)
{
    if (!validKey(key))
        return QByteArray();
    return toJson({{QStringLiteral("a"), QStringLiteral("msgs")},
                   {QStringLiteral("k"), key},
                   {QStringLiteral("before"), qMax<qint64>(0, before)},
                   {QStringLiteral("n"), int(qBound(1u, count, 50u))}});
}

QByteArray MessageService::msendRequest(const QString &key, const QString &text, bool dry)
{
    if (!validKey(key) || text.trimmed().isEmpty() || text.toUtf8().size() > MaxTextBytes)
        return QByteArray();
    return toJson({{QStringLiteral("a"), QStringLiteral("msend")},
                   {QStringLiteral("k"), key},
                   {QStringLiteral("tx"), text},
                   {QStringLiteral("dry"), dry}});
}

QByteArray MessageService::mreadRequest(const QString &key)
{
    if (!validKey(key))
        return QByteArray();
    return toJson({{QStringLiteral("a"), QStringLiteral("mread")},
                   {QStringLiteral("k"), key}});
}

QString MessageService::error(const char *code)
{
    return QStringLiteral("{\"ok\":false,\"er\":\"") + QLatin1String(code) + QStringLiteral("\"}");
}

void MessageService::reply(const QDBusMessage &message, const QString &json)
{
    mBus.send(message.createReply(json));
}

void MessageService::call(const QByteArray &request, const QDBusMessage &message)
{
    message.setDelayedReply(true);
    if (request.isEmpty()) {
        reply(message, error("bad-request"));
        return;
    }
    if (mPending.size() >= MaxPending) {
        reply(message, error("busy"));
        return;
    }
    const QString kind = QJsonDocument::fromJson(request).object().value(QStringLiteral("a")).toString();
    if (mAssistant->actions().isEmpty()) {
        reply(message, error("disconnected"));
        return;
    }
    if (!mAssistant->actions().contains(kind)) {
        reply(message, error("unsupported"));
        return;
    }
    uint id = 0;
    do {
        id = AssistantService::InternalIdBase + (++mNextId & 0x3fffffffu);
    } while (mPending.contains(id));
    if (!mAssistant->act(id, request)) {
        reply(message, error("disconnected"));
        return;
    }
    mPending.insert(id, message);
}

void MessageService::onActResult(uint id, const QString &json)
{
    const auto it = mPending.find(id);
    if (it == mPending.end())
        return;
    const QDBusMessage message = it.value();
    mPending.erase(it);
    reply(message, json);
}

QString MessageService::Conversations(uint offset, uint count, const QDBusMessage &message)
{
    call(convsRequest(offset, count), message);
    return QString();
}

QString MessageService::Messages(const QString &key, qint64 before, uint count, const QDBusMessage &message)
{
    call(msgsRequest(key, before, count), message);
    return QString();
}

QString MessageService::Send(const QString &key, const QString &text, const QDBusMessage &message)
{
    call(msendRequest(key, text, false), message);
    return QString();
}

QString MessageService::SendDry(const QString &key, const QString &text, const QDBusMessage &message)
{
    call(msendRequest(key, text, true), message);
    return QString();
}

QString MessageService::MarkRead(const QString &key, const QDBusMessage &message)
{
    call(mreadRequest(key), message);
    return QString();
}

bool MessageService::Available()
{
    const QStringList actions = mAssistant->actions();
    return actions.contains(QStringLiteral("convs")) && actions.contains(QStringLiteral("msgs"));
}

void MessageService::changed(const QString &key)
{
    emit Changed(key);
}
