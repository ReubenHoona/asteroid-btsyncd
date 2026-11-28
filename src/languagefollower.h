/*
 * Copyright (C) 2026 - eos
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

#ifndef LANGUAGEFOLLOWER_H
#define LANGUAGEFOLLOWER_H

#include <QDBusConnection>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantMap>

class QTranslator;

/*
 * The daemon's language (R6 K-05): its texts (the call actions, Connected / Disconnected, Phone disconnected) come
 * from asteroid-btsyncd's catalogues. install() puts in the English one made from the //% texts (<name>.en.qm, so
 * an id a language lacks shows English, never the id) and then the default locale's, chosen as every program of the
 * image chooses it (eoscatalogues.h: an English region of en_001 reads en_GB's, a region without a catalogue its
 * language's: de_AT reads de_DE).
 * A language chosen while the daemon runs is followed (C-03, as the apps' LocaleFollower and sensorlogd):
 * Settings > Language, setup and the phone's language set the locale through systemd-localed, which announces the
 * new Locale with org.freedesktop.DBus.Properties.PropertiesChanged on the system bus; its LANG becomes the default
 * locale and LANG of the daemon and the catalogues are installed again. A notification already posted stays as it
 * is; the next one has the new language (the texts are looked up when a notification is made).
 * Battery: one D-Bus match rule; nothing runs until the language changes.
 */
class LanguageFollower : public QObject
{
    Q_OBJECT

public:
    LanguageFollower(const QString &name, const QString &directory, QObject *parent = nullptr);
    ~LanguageFollower() override;

    // the English catalogue and the default locale's in place of the ones before; the number installed
    int install();
    // listens to localed's announcements on bus; false when the bus is not there
    bool watch(const QDBusConnection &bus);
    // the LANG of a localed Locale list ("LANG=de_DE.utf8", "LC_TIME=..."); "" when it names none
    static QString langOf(const QStringList &locale);
    // the daemon's language: LANG when it started, then the last one applied
    QString language() const { return m_lang; }
    // makes lang the daemon's language; false when it is empty or already the one in use
    bool apply(const QString &lang);

signals:
    void languageChanged(const QString &lang);

public slots:
    // org.freedesktop.DBus.Properties.PropertiesChanged of /org/freedesktop/locale1
    void propertiesChanged(const QString &interface, const QVariantMap &changed, const QStringList &invalidated);

private:
    void remove();
    void fetch();

    QString m_name;
    QString m_dir;
    QString m_lang;
    QDBusConnection m_bus;
    QList<QTranslator *> m_installed;
};

#endif // LANGUAGEFOLLOWER_H
