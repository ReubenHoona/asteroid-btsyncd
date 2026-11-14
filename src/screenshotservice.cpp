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

#include <QDBusInterface>
#include <QDBusMessage>
#include <QDebug>
#include <QFile>
#include <QFileInfo>

#include "screenshotservice.h"
#include "characteristic.h"
#include "common.h"

void ScreenshotReqChrc::WriteValue(QByteArray, QVariantMap)
{
    QList<QVariant> argumentList;
    argumentList << "/tmp/btsyncd-screenshot.jpg";
    static QDBusInterface notifyApp(SCREENSHOT_SERVICE_NAME, SCREENSHOT_PATH_BASE, SCREENSHOT_MAIN_IFACE, QDBusConnection::systemBus());
    QDBusMessage reply = notifyApp.callWithArgumentList(QDBus::AutoDetect, "saveScreenshot", argumentList);
    if(reply.type() == QDBusMessage::ErrorMessage)
        fprintf(stderr, "ScreenshotReqChrc::WriteValue: D-Bus Error: %s\n", reply.errorMessage().toStdString().c_str());
    emit screenshotTaken("/tmp/btsyncd-screenshot.jpg");
}

void ScreenshotContentChrc::onScreenshotTaken(QString path)
{
    QFile f(path);
    if(!f.open(QIODevice::ReadOnly)) {
        qDebug() << "Failed to open" << path;
        m_value = QByteArray::number(0x0);
        emit valueChanged();
        emitPropertiesChanged();
        return;
    }

    qint64 totalSize = f.bytesAvailable();
    m_value = QByteArray();
    m_value.append((totalSize >> 0) & 0xFF);
    m_value.append((totalSize >> 8) & 0xFF);
    m_value.append((totalSize >> 16) & 0xFF);
    m_value.append((totalSize >> 24) & 0xFF);
    emit valueChanged();
    emitPropertiesChanged();

    while (!f.atEnd()) {
        m_value = f.read(20);
        emit valueChanged();
        emitPropertiesChanged();
    }
    f.close();
}

const char *ScreenshotContentChrc::notAnImage(const QString &path)
{
    const QFileInfo info(path);
    if(!info.isFile())
        return "not a file";
    if(info.size() <= 0 || info.size() > MaxPushBytes)
        return "empty or too large";
    QFile f(path);
    if(!f.open(QIODevice::ReadOnly))
        return "cannot be read";
    const QByteArray head = f.read(8);
    const bool jpeg = head.startsWith("\xff\xd8\xff");
    const bool png = head == QByteArray("\x89PNG\r\n\x1a\n", 8);
    return jpeg || png ? nullptr : "not a JPEG or PNG";
}

bool ScreenshotContentChrc::push(const QString &path)
{
    if(!m_notifying || (m_phoneCheck && !m_phoneCheck())) {
        qInfo() << "Screenshot for the phone: no phone is listening";
        return false;
    }
    if(const char *why = notAnImage(path)) {
        qWarning() << "Screenshot for the phone refused:" << why;
        return false;
    }
    qInfo() << "Screenshot for the phone:" << QFileInfo(path).size() << "bytes";
    onScreenshotTaken(path);
    return true;
}

bool ScreenshotDBusApi::Push(const QString &path)
{
    return m_service->content()->push(path);
}

void ScreenshotContentChrc::emitPropertiesChanged()
{
    QDBusConnection connection = QDBusConnection::systemBus();
    QDBusMessage message = QDBusMessage::createSignal(getPath().path(),
                                                      "org.freedesktop.DBus.Properties",
                                                      "PropertiesChanged");

    QVariantMap changedProperties;
    changedProperties[QStringLiteral("Value")] = QVariant(m_value);

    QList<QVariant> arguments;
    arguments << QVariant(GATT_CHRC_IFACE) << QVariant(changedProperties) << QVariant(QStringList());
    message.setArguments(arguments);

    if (!connection.send(message))
        qDebug() << "Failed to send DBus property notification signal";
}

ScreenshotService::ScreenshotService(int index, QDBusConnection bus, QObject *parent) : Service(bus, index, SCREENSH_UUID, parent)
{
    ScreenshotReqChrc *reqChrc = new ScreenshotReqChrc(bus, 0, this);
    ScreenshotContentChrc *contChrc = new ScreenshotContentChrc(bus, 1, this);
    m_content = contChrc;

    connect(reqChrc, SIGNAL(screenshotTaken(QString)), contChrc, SLOT(onScreenshotTaken(QString)));

    addCharacteristic(reqChrc);
    addCharacteristic(contChrc);

    // The launcher's screenshots for the phone
    if(!QDBusConnection::sessionBus().registerObject(BTSYNCD_SCREENSHOT_PATH, new ScreenshotDBusApi(this),
                                                     QDBusConnection::ExportAllSlots))
        qWarning() << "ScreenshotService: cannot register" << BTSYNCD_SCREENSHOT_PATH << "on the session bus";
}
