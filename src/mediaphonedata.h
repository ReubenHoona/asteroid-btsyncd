/*
 * Copyright (C) 2026 eos-mainline contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef MEDIAPHONEDATA_H
#define MEDIAPHONEDATA_H

#include <QByteArray>
#include <QObject>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QList>
#include <QDBusConnection>

#include "characteristic.h"
#include "service.h"
#include "messagejoiner.h"

/*
 * What the phone knows about the player beyond title, artist and state (media service, W2):
 *
 *   artwork, characteristic 00007008, <art><h>HASH</h><c>RRGGBB</c><d>BASE64 JPEG</d></art>
 *     HASH: the first 16 hex digits of the JPEG's SHA-256, c: the picture's dominant colour
 *     (ARGB without alpha, the seed of the music app's colours). The phone sends the
 *     picture once per hash and connection; a hash sent before (the watch keeps the last
 *     KeepArt) comes as <art><h>..</h><c>..</c></art> without data; no art is <art></art>.
 *     The pictures are files in ~/.cache/asteroid-btsyncd/art/HASH.jpg.
 *   extras, characteristic 00007009, <mx><ca><a>NAME</a>...</ca><q><c>N</c><i><t>TITLE</t>
 *     <s>SUBTITLE</s></i>...</q></mx>: the player's custom actions (names, up to 10; the
 *     icons are app resources and cannot come) and its queue from the item that plays (c, the
 *     index of that item in the list; -1 when the player does not say; at most 24 items).
 *     The phone writes only changes; the watch starts from <mx></mx> (nothing) at every
 *     connection.
 *
 * Both writes are joined until the closing tag (the phone writes at most 500 bytes at a
 * time). The session bus API org.asteroidos.btsyncd.Media on /org/asteroidos/btsyncd/media
 * gives them to the music app: ArtPath(), ArtColor() ("" without art), Extras() as JSON
 * {"actions":["Shuffle",..],"queue":[{"t":TITLE,"s":SUBTITLE},..],"current":N}, signals
 * ArtChanged(path, color) and ExtrasChanged(), and CustomAction(index) and
 * SkipToQueueItem(index) which send the media command 5 or 6 with the index (one byte, the
 * index in the last <mx> the watch received) to the phone. They return false for an index
 * the lists do not have.
 */
class MediaPhoneData : public QObject
{
    Q_OBJECT
public:
    explicit MediaPhoneData(QObject *parent = nullptr);

    // How many pictures the watch keeps, and the largest JPEG taken
    static const int KeepArt = 8;
    static const int MaxArtBytes = 48 * 1024;
    static const int MaxActions = 10;
    static const int MaxQueue = 24;
    static const int MaxActionChars = 40;
    static const int MaxTitleChars = 40;
    static const int MaxSubtitleChars = 30;
    // The media commands that carry an index
    static const int CommandCustomAction = 5;
    static const int CommandQueueItem = 6;

    static QString artDir();
    static bool isHash(const QString &hash);
    QString artPath(const QString &hash) const;

    // What the phone wrote (complete <art>, <mx> elements)
    void applyArt(const QByteArray &xml);
    void applyExtras(const QByteArray &xml);
    // A new connection (or none): no art, nothing in the lists
    void reset();

    QString currentArtPath() const { return mPath; }
    QString currentArtColor() const { return mColor; }
    QString extrasJson() const;
    int actionCount() const { return mActions.size(); }
    int queueCount() const { return mQueue.size(); }

    // The commands for the phone: false for an index that is not in the list
    bool requestCustomAction(int index);
    bool requestQueueItem(int index);

signals:
    void artChanged(const QString &path, const QString &color);
    void extrasChanged();
    // The command byte (5 or 6) and the index byte for the phone (the media command characteristic)
    void indexedCommand(quint8 command, quint8 index);
    // The state was reset (no phone): what is half written is dropped
    void wasReset();

private:
    void setArt(const QString &path, const QString &color);
    void pruneArt();

    QString mPath, mColor;
    QStringList mActions;
    QList<QPair<QString, QString>> mQueue;
    int mCurrent = -1;
};

// Phone to watch, the artwork (see MediaPhoneData)
class MediaArtChrc : public Characteristic
{
    Q_OBJECT
public:
    MediaArtChrc(MediaPhoneData *data, QDBusConnection bus, int index, Service *service);

public slots:
    void WriteValue(QByteArray value, QVariantMap options) override;

private:
    MediaPhoneData *mData;
    MessageJoiner mJoiner;
};

// Phone to watch, the custom actions and the queue (see MediaPhoneData)
class MediaExtrasChrc : public Characteristic
{
    Q_OBJECT
public:
    MediaExtrasChrc(MediaPhoneData *data, QDBusConnection bus, int index, Service *service);

public slots:
    void WriteValue(QByteArray value, QVariantMap options) override;

private:
    MediaPhoneData *mData;
    MessageJoiner mJoiner;
};

// Session bus API for the music app (see MediaPhoneData)
class MediaDBusApi : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", BTSYNCD_MEDIA_IFACE)
public:
    explicit MediaDBusApi(MediaPhoneData *data);

public slots:
    QString ArtPath();
    QString ArtColor();
    QString Extras();
    bool CustomAction(int index);
    bool SkipToQueueItem(int index);

signals:
    void ArtChanged(const QString &path, const QString &color);
    void ExtrasChanged();

private:
    MediaPhoneData *mData;
};

#endif // MEDIAPHONEDATA_H
