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

#include "mediaservice.h"
#include "characteristic.h"
#include "common.h"
#include "wakehold.h"

#include <QDebug>
#include <QDBusMessage>
#include <QXmlStreamReader>
#include <MprisMetaData>

class MediaTitleChrc : public Characteristic
{
public:
    MediaTitleChrc(MprisPlayer *player, QDBusConnection bus, int index, Service *service)
        : Characteristic(bus, index, MEDIA_TITLE_UUID, {"encrypt-authenticated-write"}, service), m_player(player) {}

private:
    MprisPlayer *m_player;

public slots:
    void WriteValue(QByteArray value, QVariantMap)
    {
        WakeHoldScope wake;
        m_player->metaData()->setTitle(QString(value));
    }
};

class MediaAlbumChrc : public Characteristic
{
public:
    MediaAlbumChrc(MprisPlayer *player, QDBusConnection bus, int index, Service *service)
        : Characteristic(bus, index, MEDIA_ALBUM_UUID, {"encrypt-authenticated-write"}, service), m_player(player) {}

private:
    MprisPlayer *m_player;

public slots:
    void WriteValue(QByteArray value, QVariantMap)
    {
        WakeHoldScope wake;
        m_player->metaData()->setAlbumTitle(QString(value));
    }
};

class MediaArtistChrc : public Characteristic
{
public:
    MediaArtistChrc(MprisPlayer *player, QDBusConnection bus, int index, Service *service)
        : Characteristic(bus, index, MEDIA_ARTIST_UUID, {"encrypt-authenticated-write"}, service), m_player(player) {}

private:
    MprisPlayer *m_player;

public slots:
    void WriteValue(QByteArray value, QVariantMap)
    {
        WakeHoldScope wake;
        m_player->metaData()->setContributingArtist(QString(value));
    }
};

class MediaPlayingChrc : public Characteristic
{
public:
    MediaPlayingChrc(MprisPlayer *player, QDBusConnection bus, int index, Service *service)
        : Characteristic(bus, index, MEDIA_PLAY_UUID, {"encrypt-authenticated-write"}, service), m_player(player) {}

private:
    MprisPlayer *m_player;

public slots:
    void WriteValue(QByteArray value, QVariantMap)
    {
        WakeHoldScope wake;
        if (!hasMinLength(value, 1))
            return;
        m_player->setPlaybackStatus(value[0] ? Mpris::Playing : Mpris::Paused);
    }
};

/*
 * Version 2 of the media service, phone to watch: what plays, beyond the
 * title, album, artist and playing flag:
 *   <mp><st>playing|paused|stopped</st><po>POSITION</po><du>DURATION</du>
 *       <ra>RATE</ra><an>APP</an></mp>
 * POSITION and DURATION in seconds (-1 unknown), RATE in percent (100 plays
 * at normal speed, 0 paused), APP the player's name on the phone; any
 * element may be missing. The watch's MPRIS player gets the position (with
 * the time it was set, so a client can run it on), the length, the rate and
 * the app as its identity.
 */
class MediaStateChrc : public Characteristic
{
public:
    MediaStateChrc(MprisPlayer *player, QDBusConnection bus, int index, Service *service)
        : Characteristic(bus, index, MEDIA_STATE_UUID, {"encrypt-authenticated-write"}, service), m_player(player) {}

private:
    MprisPlayer *m_player;
    QByteArray m_buffer;

public slots:
    void WriteValue(QByteArray value, QVariantMap)
    {
        WakeHoldScope wake;
        m_buffer += value;
        if(m_buffer.size() > 4096) {
            m_buffer.clear();
            return;
        }
        if(!value.endsWith("</mp>"))
            return;
        QXmlStreamReader reader(m_buffer);
        m_buffer.clear();
        if(!reader.readNextStartElement() || reader.name() != QLatin1String("mp"))
            return;
        while(reader.readNextStartElement()) {
            const auto name = reader.name();
            const QString text = reader.readElementText().trimmed();
            bool ok = false;
            if(name == QLatin1String("st")) {
                if(text == "playing") m_player->setPlaybackStatus(Mpris::Playing);
                else if(text == "paused") m_player->setPlaybackStatus(Mpris::Paused);
                else if(text == "stopped") m_player->setPlaybackStatus(Mpris::Stopped);
            } else if(name == QLatin1String("po")) {
                const qlonglong s = text.toLongLong(&ok);
                if(ok && s >= 0)
                    m_player->setPosition(s * 1000);
            } else if(name == QLatin1String("du")) {
                const qlonglong s = text.toLongLong(&ok);
                if(ok)
                    m_player->metaData()->setDuration(s > 0 ? QVariant(s * 1000) : QVariant());
            } else if(name == QLatin1String("ra")) {
                const int r = text.toInt(&ok);
                if(ok && r >= 0)
                    m_player->setRate(r / 100.0);
            } else if(name == QLatin1String("an")) {
                m_player->setIdentity(text.isEmpty() ? QStringLiteral("Asteroid BLE Sync Daemon") : text.left(64));
            }
        }
    }
};

void MediaCommandsChrc::pauseRequested()
{
    m_value.resize(1);
    m_value[0] = MEDIA_COMMAND_PAUSE;
    emit valueChanged();
}

void MediaCommandsChrc::playRequested()
{
    m_value.resize(1);
    m_value[0] = MEDIA_COMMAND_PLAY;
    emit valueChanged();
}

void MediaCommandsChrc::playPauseRequested()
{
    m_value.resize(1);
    if(m_player->playbackStatus() == Mpris::Playing)
        m_value[0] = MEDIA_COMMAND_PAUSE;
    else
        m_value[0] = MEDIA_COMMAND_PLAY;

    emit valueChanged();
}

void MediaCommandsChrc::stopRequested()
{
    m_value.resize(1);
    m_value[0] = MEDIA_COMMAND_PAUSE;
    emit valueChanged();
}

void MediaCommandsChrc::nextRequested()
{
    m_value.resize(1);
    m_value[0] = MEDIA_COMMAND_NEXT;
    emit valueChanged();
}

void MediaCommandsChrc::previousRequested()
{
    m_value.resize(1);
    m_value[0] = MEDIA_COMMAND_PREVIOUS;
    emit valueChanged();
}

void MediaCommandsChrc::volumeRequested(double volume)
{
    // Two bytes: the command and the volume in percent (the value had room
    // for the command only)
    m_value.resize(2);
    m_value[0] = MEDIA_COMMAND_VOLUME;
    m_value[1] = char(qBound(0, int(volume * 100 + 0.5), 100));
    emit valueChanged();
}

void MediaCommandsChrc::emitPropertiesChanged()
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

class MediaVolumeChrc : public Characteristic
{
public:
    MediaVolumeChrc(MprisPlayer *player, QDBusConnection bus, int index, Service *service)
        : Characteristic(bus, index, MEDIA_VOL_UUID, {"encrypt-authenticated-write"}, service), m_player(player) {}

private:
    MprisPlayer *m_player;

public slots:
    void WriteValue(QByteArray value, QVariantMap)
    {
        WakeHoldScope wake;
        if (!hasMinLength(value, 1))
            return;
        m_player->setVolume(int( (unsigned char) value[0])/100.0);
    }
};

MediaService::MediaService(int index, QDBusConnection bus, QObject *parent) : Service(bus, index, MEDIA_UUID, parent)
{
    m_mprisPlayer = new MprisPlayer(this);
    m_mprisPlayer->setServiceName("asteroid-btsyncd");
    m_mprisPlayer->setIdentity("Asteroid BLE Sync Daemon");

    m_mprisPlayer->setCanControl(true);
    m_mprisPlayer->setCanGoNext(true);
    m_mprisPlayer->setCanGoPrevious(true);
    m_mprisPlayer->setCanPause(true);
    m_mprisPlayer->setCanPlay(true);

    m_mprisPlayer->setCanSeek(false);
    m_mprisPlayer->setCanQuit(false);
    m_mprisPlayer->setCanRaise(false);
    m_mprisPlayer->setCanSetFullscreen(false);

    m_mprisPlayer->setPlaybackStatus(Mpris::Stopped);
    m_mprisPlayer->setLoopStatus(Mpris::LoopNone);
    m_mprisPlayer->setShuffle(false);

    addCharacteristic(new MediaTitleChrc(m_mprisPlayer, bus, 0, this));
    addCharacteristic(new MediaAlbumChrc(m_mprisPlayer, bus, 1, this));
    addCharacteristic(new MediaArtistChrc(m_mprisPlayer, bus, 2, this));
    addCharacteristic(new MediaPlayingChrc(m_mprisPlayer, bus, 3, this));
    m_commandsChrc = new MediaCommandsChrc(m_mprisPlayer, bus, 4, this);
    addCharacteristic(m_commandsChrc);
    addCharacteristic(new MediaVolumeChrc(m_mprisPlayer, bus, 5, this));
    addCharacteristic(new MediaStateChrc(m_mprisPlayer, bus, 6, this));

    connect(m_mprisPlayer, SIGNAL(pauseRequested()), m_commandsChrc, SLOT(pauseRequested()));
    connect(m_mprisPlayer, SIGNAL(playRequested()), m_commandsChrc, SLOT(playRequested()));
    connect(m_mprisPlayer, SIGNAL(playPauseRequested()), m_commandsChrc, SLOT(playPauseRequested()));
    connect(m_mprisPlayer, SIGNAL(stopRequested()), m_commandsChrc, SLOT(stopRequested()));
    connect(m_mprisPlayer, SIGNAL(nextRequested()), m_commandsChrc, SLOT(nextRequested()));
    connect(m_mprisPlayer, SIGNAL(previousRequested()), m_commandsChrc, SLOT(previousRequested()));
    connect(m_mprisPlayer, SIGNAL(volumeRequested(double)), m_commandsChrc, SLOT(volumeRequested(double)));
 }
