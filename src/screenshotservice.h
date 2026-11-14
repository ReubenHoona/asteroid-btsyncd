/*
 * Copyright (C) 2017 - Florent Revest <revestflo@gmail.com>
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

#ifndef SCREENSHOTSERVICE_H
#define SCREENSHOTSERVICE_H

#include <QObject>
#include <functional>

#include "service.h"

class ScreenshotReqChrc : public Characteristic
{
    Q_OBJECT
public:
    ScreenshotReqChrc(QDBusConnection bus, int index, Service *service)
        : Characteristic(bus, index, SCREENSH_REQ_UUID, {"encrypt-authenticated-write"}, service) {}

public slots:
    void WriteValue(QByteArray, QVariantMap);

signals:
    void screenshotTaken(QString);
};


class ScreenshotContentChrc : public Characteristic
{
    Q_OBJECT
    Q_PROPERTY(QByteArray Value READ getValue NOTIFY valueChanged)
public:
    ScreenshotContentChrc(QDBusConnection bus, int index, Service *service) : Characteristic(bus, index, SCREENSH_CON_UUID, {"encrypt-authenticated-read", "encrypt-authenticated-notify"}, service)
    {}

public:
    // Sends the file the way a requested screenshot goes (a 4 byte size, then the bytes in pieces of 20):
    // the launcher's screenshot of the watch for the phone. False, and nothing sent, when no phone
    // listens or the file is not a small image (JPEG or PNG). The phone takes a transfer it did not ask for.
    bool push(const QString &path);
    // Whether a phone is there: BlueZ keeps a subscription over a disconnection (set by the application)
    void setPhoneCheck(const std::function<bool()> &check) { m_phoneCheck = check; }
    bool notifying() const { return m_notifying; }
    static const qint64 MaxPushBytes = 1024 * 1024;
    // Why a file is not one to push (the file's problem as text), nullptr when it is
    static const char *notAnImage(const QString &path);

public slots:
    QByteArray ReadValue(QVariantMap)
    {
        return m_value;
    }

    void StartNotify() { m_notifying = true; }
    void StopNotify() { m_notifying = false; }

private slots:
    void emitPropertiesChanged();
    void onScreenshotTaken(QString);

signals:
    void valueChanged();

private:
    QByteArray m_value;
    bool m_notifying = false;
    std::function<bool()> m_phoneCheck;

    QByteArray getValue()
    {
        return m_value;
    }
};

class ScreenshotService : public Service
{
    Q_OBJECT
public:
    explicit ScreenshotService(int index, QDBusConnection bus, QObject *parent = 0);
    ScreenshotContentChrc *content() const { return m_content; }

private:
    ScreenshotContentChrc *m_content;
};

/*
 * Session bus API for the launcher: org.asteroidos.btsyncd.Screenshot on
 * /org/asteroidos/btsyncd/screenshot. Push(path) hands the phone a screenshot the watch took itself (the
 * side button chord), as stock does after saving it (SEND_SCREENSHOT): true when it goes out.
 */
class ScreenshotDBusApi : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", BTSYNCD_SCREENSHOT_IFACE)
public:
    explicit ScreenshotDBusApi(ScreenshotService *service) : QObject(service), m_service(service) {}

public slots:
    bool Push(const QString &path);

private:
    ScreenshotService *m_service;
};

#endif // SCREENSHOTSERVICE_H
