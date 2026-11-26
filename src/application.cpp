/*
 * Copyright (C) 2016 - Florent Revest <revestflo@gmail.com>
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

#include "application.h"
#include "notificationservice.h"
#include "weatherservice.h"
#include "mediaservice.h"
#include "batteryservice.h"
#include "screenshotservice.h"
#include "timeservice.h"
#include "healthservice.h"
#include "voiceservice.h"
#include "phonestatusservice.h"
#include "assistantservice.h"
#include "callaudio.h"
#include "calllog.h"
#include "messageservice.h"
#include "heartrateservice.h"
#ifdef BTSYNCD_HAVE_CALENDAR
#include "calendarservice.h"
#endif

#include <QDBusMetaType>

Application::Application(QDBusConnection bus, QObject *parent) : QObject(parent)
{
    mPath = "/";

    NotificationService *notifications = new NotificationService(0, bus);
    mNotifications = notifications;
    addService(notifications);
    addService(new WeatherService(1, bus));
    MediaService *media = new MediaService(2, bus);
    addService(media);
    connect(notifications, &NotificationService::phoneConnectedChanged, media, &MediaService::phoneConnectedChanged);
    addService(new BatteryService(3, bus));
    ScreenshotService *screenshots = new ScreenshotService(4, bus);
    // A screenshot the watch took goes to a phone that is there (BlueZ keeps its subscriptions over a disconnection)
    screenshots->content()->setPhoneCheck([notifications] { return notifications->phoneConnected(); });
    addService(screenshots);
    addService(new TimeService(5, bus));
    HealthService *health = new HealthService(6, bus);
    addService(health);
    connect(notifications, &NotificationService::phoneConnectedChanged, health, &HealthService::phoneConnectedChanged);
    addService(new VoiceService(7, bus, notifications));
#ifdef BTSYNCD_HAVE_CALENDAR
    // The phone's calendar into mkcal
    CalendarService *calendar = new CalendarService(8, bus);
    addService(calendar);
    connect(notifications, &NotificationService::phoneConnectedChanged, calendar, &CalendarService::phoneConnectedChanged);
#endif
    // The phone's battery and Do Not Disturb (protocol version 4)
    addService(new PhoneStatusService(9, bus, notifications));
    // Questions of the assistant (asteroid-assistant), relayed by the
    // phone to the answer server set there
    AssistantService *assistant = new AssistantService(10, bus, notifications);
    addService(assistant);
    mAssistant = assistant;
    // The call log for the Phone app's Recents (CR-08), from the phone's call cards
    notifications->setCallObserver(new CallLog(QString(), QDBusConnection::sessionBus(), this));
    // The phone's conversations for the messages app, over the same relay
    new MessageService(assistant, QDBusConnection::sessionBus(), assistant);
    // "Share heart rate": the standard Heart Rate Service, advertised only
    // while the quick panel's toggle is on
    addService(new HeartRateService(11, bus));

    qDBusRegisterMetaType<InterfaceList>();
    qDBusRegisterMetaType<ManagedObjectList>();

    bus.registerObject(mPath, this, QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllProperties);
}

QDBusObjectPath Application::getPath()
{
    return QDBusObjectPath(mPath);
}

void Application::addService(Service *service)
{
    mServices.append(service);
}

void Application::setPhoneLink(bool connected)
{
    mNotifications->setLinkConnected(connected);
}

void Application::setCallControl(CallControl *calls)
{
    mNotifications->setCallControl(calls);
}

void Application::setCallAudio(CallAudio *calls)
{
    calls->setPhoneRelay(mAssistant);
}

/* Exposed slots */
ManagedObjectList Application::GetManagedObjects()
{
    ManagedObjectList response;

    foreach(Service *serv, mServices) {
        InterfaceList serviceInterfaces;
        QVariantMap serviceProperties;
        serviceProperties.insert("UUID", serv->getUuid());
        serviceProperties.insert("Primary", serv->getPrimary());
        serviceProperties.insert("Characteristics", QVariant::fromValue(serv->getCharacteristicPaths()));
        serviceInterfaces.insert(GATT_SERVICE_IFACE, serviceProperties);
        response.insert(serv->getPath(), serviceInterfaces);

        QList<Characteristic *> chrcs = serv->getCharacteristics();
        foreach(Characteristic *chrc, chrcs) {
            InterfaceList chrcInterfaces;
            QVariantMap chrcProperties;
            chrcProperties.insert("Service", QVariant::fromValue(chrc->getService()));
            chrcProperties.insert("UUID", chrc->getUuid());
            chrcProperties.insert("Flags", chrc->getFlags());
            chrcProperties.insert("Descriptors", QVariant::fromValue(chrc->getDescriptorPaths()));
            chrcInterfaces.insert(GATT_CHRC_IFACE, chrcProperties);
            response.insert(chrc->getPath(), chrcInterfaces);

            QList<Descriptor *> descs = chrc->getDescriptors();
            foreach(Descriptor *desc, descs) {
                InterfaceList descInterfaces;
                QVariantMap descProperties;
                descProperties.insert("UUID", desc->getUuid());
                descProperties.insert("Characteristic", QVariant::fromValue(desc->getCharacteristic()));
                descProperties.insert("Flags", desc->getFlags());
                descInterfaces.insert(GATT_DESC_IFACE, descProperties);
                response.insert(desc->getPath(), descInterfaces);
            }
        }
    }

    return response;
}
