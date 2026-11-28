/*
 * Copyright (C) 2026 eos
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef EOSCATALOGUES_H
#define EOSCATALOGUES_H

#include <QDir>
#include <QLocale>
#include <QString>
#include <QStringList>
#include <QTranslator>

/*
 * Which catalogue a locale reads (one rule for every program of the image: this file is the same in qml-asteroid
 * src/app, asteroid-launcher lipstick/src, asteroid-sensorlogd daemon, asteroid-btsyncd src and
 * org.asteroid.contacts src; r6/k05/harness/same-catalogue-rule.sh compares them).
 *
 * 1. English (R6 K-05): CLDR's parent locales put every English region but the United States and its territories
 *    (en_US, en_AS, en_GU, en_MH, en_MP, en_PR, en_UM, en_VI; en_US_POSIX) under en_001, World English, whose
 *    texts are British. Stock ships values-en-rGB in 99 packages (en-rAU, en-rIN, en-rCA in about 40, en-rIE 24,
 *    none for en-NZ) and Android's resource matching walks those parents (libandroidfw LocaleData), so en-NZ reads
 *    British English. Here a region of en_001 without a catalogue of its own (en_NZ, en_AU, en_IE, en_IN...) reads
 *    <name>.en_GB.qm; QTranslator's own search would take <name>.en.qm, the code's (US) English, first.
 * 2. QTranslator's own search (<name>.de_AT.qm, <name>.de.qm; zh_Hant_TW, zh_Hant, zh).
 * 3. The nearest catalogue of the language for a region the image has none for (R5 C-04): the watch takes the
 *    phone's language with the phone's region (de_AT: Austrian formats), as stock's PhoneSettingLocaleListener
 *    does, but the image ships de_DE's catalogue only. The catalogue of the language's main region (QLocale(de):
 *    de_DE) when there is one, else the first of its other regions by name. Not for English (rule 1).
 * None of them: no catalogue, the text stays the English of the code (or of <name>.en.qm, installed first).
 */
namespace EosCatalogues {

// an English locale whose CLDR parent is en_001 (its texts are British)
inline bool britishEnglish(const QLocale &locale)
{
    if (locale.language() != QLocale::English)
        return false;
    static const QStringList american = { QStringLiteral("US"), QStringLiteral("AS"), QStringLiteral("GU"),
                                          QStringLiteral("MH"), QStringLiteral("MP"), QStringLiteral("PR"),
                                          QStringLiteral("UM"), QStringLiteral("VI") };
    return !american.contains(QLocale::territoryToCode(locale.territory()));
}

// rule 1: the catalogue an English region of en_001 reads (its own, else en_GB's); "" for any other locale
inline QString english(const QString &name, const QString &directory, const QLocale &locale)
{
    if (!britishEnglish(locale))
        return QString();
    const QDir dir(directory);
    const QString own = name + QLatin1Char('.') + locale.name() + QStringLiteral(".qm");
    if (dir.exists(own))
        return dir.filePath(own);
    const QString british = name + QStringLiteral(".en_GB.qm");
    return dir.exists(british) ? dir.filePath(british) : QString();
}

// rule 3: the language's catalogue of another region; "" when it has none (and for English)
inline QString nearest(const QString &name, const QString &directory, const QLocale &locale)
{
    if (locale.language() == QLocale::C || locale.language() == QLocale::AnyLanguage
        || locale.language() == QLocale::English)
        return QString();
    const QString lang = QLocale::languageToCode(locale.language());
    const QDir dir(directory);
    const QStringList found = dir.entryList({ name + QLatin1Char('.') + lang + QStringLiteral("_*.qm") },
                                            QDir::Files, QDir::Name);
    if (found.isEmpty())
        return QString();
    const QString main = name + QLatin1Char('.') + QLocale(lang).name() + QStringLiteral(".qm");
    return dir.filePath(found.contains(main) ? main : found.first());
}

// rules 1-3 in order: loads translator with the catalogue of name for locale; false when there is none
inline bool load(QTranslator *translator, const QString &name, const QString &directory, const QLocale &locale)
{
    const QString british = english(name, directory, locale);
    if (!british.isEmpty())
        return translator->load(british);
    if (translator->load(locale, name, QStringLiteral("."), directory, QStringLiteral(".qm")))
        return true;
    const QString other = nearest(name, directory, locale);
    return !other.isEmpty() && translator->load(other);
}

} // namespace EosCatalogues

#endif // EOSCATALOGUES_H
