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

#ifndef MESSAGESERVICE_H
#define MESSAGESERVICE_H

#include <QDBusConnection>
#include <QDBusMessage>
#include <QHash>
#include <QObject>
#include <QString>

class AssistantService;

/*
 * The phone's text conversations for the watch's messages app: a session bus
 * API, org.asteroidos.btsyncd.Messages on /org/asteroidos/btsyncd/messages,
 * over the assistant relay's actions convs, msgs, msend and mread
 * (assistantprotocol.h; the phone's side is Gadgetbridge's
 * AsteroidOSMessages). Nothing is fetched on a schedule: the app asks when
 * it is open, a page at a time, and every call is one BLE round trip.
 *
 * Each method replies, when the phone has answered, with the phone's result
 * as a JSON string, so a caller needs nothing else:
 *   Conversations(u offset, u count) -> s
 *     {"ok":true,"c":[{"k":"t17","n":"Name","tx":"last text","t":SEC,"u":2,"me":0}],"more":true}
 *     (the newest first; count 1..30; a phone number never comes)
 *   Messages(s key, x before, u count) -> s
 *     {"ok":true,"k":"t17","m":[{"i":903,"t":SEC,"me":0,"tx":"..","at":1}],"more":true}
 *     (oldest first; before: seconds, 0 = the newest; count 1..50)
 *   Send(s key, s text) -> s     {"ok":true} | {"ok":true,"pending":true} | an error
 *   SendDry(s key, s text) -> s  the phone checks the thread without sending ({"ok":true,"dry":true})
 *   MarkRead(s key) -> s         {"ok":true}
 *   Available() -> b             a phone that does these is connected
 * An error is {"ok":false,"er":CODE}: from the phone no-permission, not-found,
 * bad-request, no-sms, expired, failed; from btsyncd bad-request (nothing
 * sent), disconnected (no phone), unsupported (a Gadgetbridge without
 * messages), busy (too many calls waiting), timeout. The key is the phone's
 * own ("t17", "n..."): letters, digits, '_' and '-', at most 64.
 * Signal Changed(s key): a conversation changed on the phone (see changed()).
 *
 * The logs carry the kind and the sizes only, never a key, a name or a text.
 */
class MessageService : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.asteroidos.btsyncd.Messages")

public:
    static const int MaxPending = 6;
    static const int MaxTextBytes = 4000;

    MessageService(AssistantService *assistant, QDBusConnection bus = QDBusConnection::sessionBus(),
                   QObject *parent = nullptr);

    // The act's JSON for a call; empty when the arguments are not acceptable
    static QByteArray convsRequest(uint offset, uint count);
    static QByteArray msgsRequest(const QString &key, qint64 before, uint count);
    static QByteArray msendRequest(const QString &key, const QString &text, bool dry);
    static QByteArray mreadRequest(const QString &key);
    static bool validKey(const QString &key);

    // Tell the app a conversation changed (a new message came in): for the
    // notification path to call; an empty key means "the list"
    void changed(const QString &key);

public slots:
    // Delayed replies: the last argument is the caller's message
    QString Conversations(uint offset, uint count, const QDBusMessage &message);
    QString Messages(const QString &key, qint64 before, uint count, const QDBusMessage &message);
    QString Send(const QString &key, const QString &text, const QDBusMessage &message);
    QString SendDry(const QString &key, const QString &text, const QDBusMessage &message);
    QString MarkRead(const QString &key, const QDBusMessage &message);
    bool Available();

signals:
    void Changed(const QString &key);

private slots:
    void onActResult(uint id, const QString &json);

private:
    void call(const QByteArray &request, const QDBusMessage &message);
    void reply(const QDBusMessage &message, const QString &json);
    static QString error(const char *code);

    AssistantService *mAssistant;
    QDBusConnection mBus;
    QHash<uint, QDBusMessage> mPending;
    uint mNextId = 0;
};

#endif // MESSAGESERVICE_H
