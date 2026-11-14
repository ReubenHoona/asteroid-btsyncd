/*
 * Copyright (C) 2026 eos-mainline contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "mediaphonedata.h"
#include "common.h"
#include "wakehold.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QXmlStreamReader>

MediaPhoneData::MediaPhoneData(QObject *parent) : QObject(parent)
{
}

QString MediaPhoneData::artDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + "/asteroid-btsyncd/art";
}

bool MediaPhoneData::isHash(const QString &hash)
{
    static const QRegularExpression re("^[0-9a-f]{16}$");
    return re.match(hash).hasMatch();
}

QString MediaPhoneData::artPath(const QString &hash) const
{
    return isHash(hash) ? artDir() + "/" + hash + ".jpg" : QString();
}

void MediaPhoneData::setArt(const QString &path, const QString &color)
{
    if(path == mPath && color == mColor)
        return;
    mPath = path;
    mColor = color;
    emit artChanged(mPath, mColor);
}

void MediaPhoneData::pruneArt()
{
    QDir dir(artDir());
    const QFileInfoList files = dir.entryInfoList(QStringList() << "*.jpg", QDir::Files, QDir::Time);
    for(int i = KeepArt; i < files.size(); i++) {
        if(files.at(i).absoluteFilePath() != mPath)
            QFile::remove(files.at(i).absoluteFilePath());
    }
}

void MediaPhoneData::applyArt(const QByteArray &xml)
{
    static const QRegularExpression colorRe("^[0-9a-fA-F]{6}$"), base64Re("^[A-Za-z0-9+/]+={0,2}$");
    QXmlStreamReader reader(xml);
    if(!reader.readNextStartElement() || reader.name() != QLatin1String("art"))
        return;
    QString hash, color;
    QByteArray data;
    bool hasData = false;
    while(reader.readNextStartElement()) {
        if(reader.name() == QLatin1String("h")) hash = reader.readElementText().trimmed();
        else if(reader.name() == QLatin1String("c")) color = reader.readElementText().trimmed();
        else if(reader.name() == QLatin1String("d")) {
            data = reader.readElementText().toLatin1().trimmed();
            hasData = true;
        }
        else reader.skipCurrentElement();
    }
    if(reader.hasError()) {
        qWarning() << "Media art: the message could not be read:" << reader.errorString();
        return;
    }
    // <art></art>: the player has no picture
    if(hash.isEmpty() && !hasData) {
        qInfo() << "Media art: none";
        setArt(QString(), QString());
        return;
    }
    if(!isHash(hash)) {
        qWarning() << "Media art: not a picture message";
        return;
    }
    if(!colorRe.match(color).hasMatch())
        color.clear();
    else
        color = color.toLower();
    const QString path = artPath(hash);
    if(hasData) {
        if(!QFile::exists(path)) {
            if(data.isEmpty() || data.size() > (MaxArtBytes + 2) / 3 * 4
                    || !base64Re.match(QString::fromLatin1(data)).hasMatch()) {
                qWarning() << "Media art: not a picture message";
                return;
            }
            const QByteArray jpeg = QByteArray::fromBase64(data);
            if(QString::fromLatin1(QCryptographicHash::hash(jpeg, QCryptographicHash::Sha256).toHex().left(16)) != hash) {
                qWarning() << "Media art: its hash does not match";
                return;
            }
            // (the music app's Image decodes the file; the hash and the size say the phone sent what it meant to)
            QDir().mkpath(artDir());
            QSaveFile out(path);
            if(!out.open(QIODevice::WriteOnly) || out.write(jpeg) != jpeg.size() || !out.commit()) {
                qWarning() << "Media art: cannot be written";
                return;
            }
        }
    } else if(!QFile::exists(path)) {
        // The phone took the watch to have it (it sent it earlier): it does not, keep what shows
        qWarning() << "Media art: a picture the watch does not have, without data";
        return;
    }
    // This one is the newest of the cache
    QFile touch(path);
    if(touch.open(QIODevice::ReadWrite)) {
        touch.setFileTime(QDateTime::currentDateTime(), QFileDevice::FileModificationTime);
        touch.close();
    }
    qInfo().noquote() << QString("Media art: %1 bytes, colour %2").arg(QFileInfo(path).size()).arg(color.isEmpty() ? "none" : color);
    setArt(path, color);
    pruneArt();
}

static QString cut(QString text, int max)
{
    text = text.simplified();
    if(text.length() > max)
        text = text.left(max - 1) + QChar(0x2026);
    return text;
}

void MediaPhoneData::applyExtras(const QByteArray &xml)
{
    QXmlStreamReader reader(xml);
    if(!reader.readNextStartElement() || reader.name() != QLatin1String("mx"))
        return;
    QStringList actions;
    QList<QPair<QString, QString>> queue;
    int current = -1;
    while(reader.readNextStartElement()) {
        if(reader.name() == QLatin1String("ca")) {
            while(reader.readNextStartElement()) {
                if(reader.name() == QLatin1String("a")) {
                    const QString a = cut(reader.readElementText(), MaxActionChars);
                    if(!a.isEmpty() && actions.size() < MaxActions)
                        actions << a;
                } else
                    reader.skipCurrentElement();
            }
        } else if(reader.name() == QLatin1String("q")) {
            while(reader.readNextStartElement()) {
                if(reader.name() == QLatin1String("c")) {
                    bool ok = false;
                    const int c = reader.readElementText().trimmed().toInt(&ok);
                    current = ok ? c : -1;
                } else if(reader.name() == QLatin1String("i")) {
                    QString title, subtitle;
                    while(reader.readNextStartElement()) {
                        if(reader.name() == QLatin1String("t")) title = cut(reader.readElementText(), MaxTitleChars);
                        else if(reader.name() == QLatin1String("s")) subtitle = cut(reader.readElementText(), MaxSubtitleChars);
                        else reader.skipCurrentElement();
                    }
                    if(queue.size() < MaxQueue)
                        queue << qMakePair(title, subtitle);
                } else
                    reader.skipCurrentElement();
            }
        } else {
            reader.skipCurrentElement();
        }
    }
    if(reader.hasError()) {
        qWarning() << "Media extras: the message could not be read:" << reader.errorString();
        return;
    }
    if(current >= queue.size())
        current = -1;
    if(actions == mActions && queue == mQueue && current == mCurrent)
        return;
    mActions = actions;
    mQueue = queue;
    mCurrent = current;
    qInfo() << "Media extras:" << mActions.size() << "action(s)," << mQueue.size() << "queue item(s)";
    emit extrasChanged();
}

void MediaPhoneData::reset()
{
    emit wasReset();
    setArt(QString(), QString());
    if(!mActions.isEmpty() || !mQueue.isEmpty() || mCurrent != -1) {
        mActions.clear();
        mQueue.clear();
        mCurrent = -1;
        emit extrasChanged();
    }
}

QString MediaPhoneData::extrasJson() const
{
    QJsonArray actions, queue;
    for(const QString &a : mActions)
        actions.append(a);
    for(const auto &item : mQueue) {
        QJsonObject o;
        o.insert(QStringLiteral("t"), item.first);
        o.insert(QStringLiteral("s"), item.second);
        queue.append(o);
    }
    QJsonObject root;
    root.insert(QStringLiteral("actions"), actions);
    root.insert(QStringLiteral("queue"), queue);
    root.insert(QStringLiteral("current"), mCurrent);
    return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact));
}

bool MediaPhoneData::requestCustomAction(int index)
{
    if(index < 0 || index >= mActions.size())
        return false;
    emit indexedCommand(CommandCustomAction, quint8(index));
    return true;
}

bool MediaPhoneData::requestQueueItem(int index)
{
    if(index < 0 || index >= mQueue.size())
        return false;
    emit indexedCommand(CommandQueueItem, quint8(index));
    return true;
}

MediaArtChrc::MediaArtChrc(MediaPhoneData *data, QDBusConnection bus, int index, Service *service)
    : Characteristic(bus, index, MEDIA_ART_UUID, {"encrypt-authenticated-write"}, service, service), mData(data),
      mJoiner("Media art", {"art"}, 80 * 1024)
{
    connect(data, &MediaPhoneData::wasReset, this, [this] { mJoiner.drop("the phone is gone"); });
}

void MediaArtChrc::WriteValue(QByteArray value, QVariantMap options)
{
    WakeHoldScope wake;
    for(const auto &message : mJoiner.add(value, MessageJoiner::offsetOf(options))) {
        if(message.first == "art")
            mData->applyArt(message.second);
    }
}

MediaExtrasChrc::MediaExtrasChrc(MediaPhoneData *data, QDBusConnection bus, int index, Service *service)
    : Characteristic(bus, index, MEDIA_EXTRAS_UUID, {"encrypt-authenticated-write"}, service, service), mData(data),
      mJoiner("Media extras", {"mx"}, 16 * 1024)
{
    connect(data, &MediaPhoneData::wasReset, this, [this] { mJoiner.drop("the phone is gone"); });
}

void MediaExtrasChrc::WriteValue(QByteArray value, QVariantMap options)
{
    WakeHoldScope wake;
    for(const auto &message : mJoiner.add(value, MessageJoiner::offsetOf(options))) {
        if(message.first == "mx")
            mData->applyExtras(message.second);
    }
}

MediaDBusApi::MediaDBusApi(MediaPhoneData *data) : QObject(data), mData(data)
{
    connect(data, &MediaPhoneData::artChanged, this, &MediaDBusApi::ArtChanged);
    connect(data, &MediaPhoneData::extrasChanged, this, &MediaDBusApi::ExtrasChanged);
}

QString MediaDBusApi::ArtPath() { return mData->currentArtPath(); }
QString MediaDBusApi::ArtColor() { return mData->currentArtColor(); }
QString MediaDBusApi::Extras() { return mData->extrasJson(); }
bool MediaDBusApi::CustomAction(int index) { return mData->requestCustomAction(index); }
bool MediaDBusApi::SkipToQueueItem(int index) { return mData->requestQueueItem(index); }
