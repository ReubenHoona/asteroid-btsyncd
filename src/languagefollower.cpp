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

#include "languagefollower.h"
#include "eoscatalogues.h"

#include <QCoreApplication>
#include <QDBusArgument>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusVariant>
#include <QLocale>
#include <QTranslator>

static const char LOCALE1_PATH[] = "/org/freedesktop/locale1";
static const char LOCALE1_SERVICE[] = "org.freedesktop.locale1";
static const char LOCALE1_INTERFACE[] = "org.freedesktop.locale1";
static const char PROPERTIES_INTERFACE[] = "org.freedesktop.DBus.Properties";

LanguageFollower::LanguageFollower(const QString &name, const QString &directory, QObject *parent)
    : QObject(parent), m_name(name), m_dir(directory), m_lang(qEnvironmentVariable("LANG")), m_bus(QString())
{
}

LanguageFollower::~LanguageFollower()
{
    remove();
}

void LanguageFollower::remove()
{
    for (QTranslator *t : std::as_const(m_installed)) {
        QCoreApplication::removeTranslator(t);
        delete t;
    }
    m_installed.clear();
}

int LanguageFollower::install()
{
    remove();
    // English first: Qt searches the most recently installed catalogue first, so the locale's wins wherever it has
    // the id, and an id it lacks shows English
    QTranslator *english = new QTranslator(this);
    if (english->load(m_name + QStringLiteral(".en"), m_dir)) {
        QCoreApplication::installTranslator(english);
        m_installed.append(english);
    } else {
        delete english;
    }
    QTranslator *t = new QTranslator(this);
    if (EosCatalogues::load(t, m_name, m_dir, QLocale())) {
        QCoreApplication::installTranslator(t);
        m_installed.append(t);
    } else {
        delete t;
    }
    return m_installed.size();
}

bool LanguageFollower::watch(const QDBusConnection &bus)
{
    if (!bus.isConnected())
        return false;
    m_bus = bus;
    // no sender in the match: only localed has this path, and a match on its bus-activated name would have QtDBus
    // follow its NameOwnerChanged every time localed starts and exits
    return m_bus.connect(QString(), QLatin1String(LOCALE1_PATH), QLatin1String(PROPERTIES_INTERFACE),
                         QStringLiteral("PropertiesChanged"), this,
                         SLOT(propertiesChanged(QString,QVariantMap,QStringList)));
}

QString LanguageFollower::langOf(const QStringList &locale)
{
    for (const QString &entry : locale) {
        if (entry.startsWith(QLatin1String("LANG=")))
            return entry.mid(5).trimmed();
    }
    return QString();
}

static QStringList stringList(const QVariant &v)
{
    if (v.canConvert<QDBusArgument>())
        return qdbus_cast<QStringList>(v.value<QDBusArgument>());
    if (v.canConvert<QDBusVariant>())
        return stringList(v.value<QDBusVariant>().variant());
    return v.toStringList();
}

void LanguageFollower::propertiesChanged(const QString &interface, const QVariantMap &changed,
                                         const QStringList &invalidated)
{
    if (interface != QLatin1String(LOCALE1_INTERFACE))
        return;
    const auto it = changed.constFind(QStringLiteral("Locale"));
    if (it != changed.constEnd()) {
        apply(langOf(stringList(it.value())));
        return;
    }
    // announced without its value: asked for once
    if (invalidated.contains(QStringLiteral("Locale")))
        fetch();
}

void LanguageFollower::fetch()
{
    if (!m_bus.isConnected())
        return;
    QDBusMessage get = QDBusMessage::createMethodCall(QLatin1String(LOCALE1_SERVICE), QLatin1String(LOCALE1_PATH),
                                                      QLatin1String(PROPERTIES_INTERFACE), QStringLiteral("Get"));
    get << QLatin1String(LOCALE1_INTERFACE) << QStringLiteral("Locale");
    QDBusPendingCallWatcher *w = new QDBusPendingCallWatcher(m_bus.asyncCall(get), this);
    connect(w, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *call) {
        QDBusPendingReply<QDBusVariant> reply = *call;
        if (!reply.isError())
            apply(langOf(stringList(reply.value().variant())));
        call->deleteLater();
    });
}

bool LanguageFollower::apply(const QString &lang)
{
    if (lang.isEmpty() || lang == m_lang)
        return false;
    m_lang = lang;
    qputenv("LANG", lang.toUtf8());
    QLocale::setDefault(QLocale(lang));
    install();
    emit languageChanged(lang);
    return true;
}
