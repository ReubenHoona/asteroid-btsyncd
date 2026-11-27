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

#include "healthprofile.h"

#include <MDConfItem>
#include <QCryptographicHash>
#include <QXmlStreamReader>
#include <cmath>

namespace HealthProfile {

const char GenderKey[] = "/org/asteroidos/health/profile/gender";
const char HeightKey[] = "/org/asteroidos/health/profile/height-cm";
const char WeightKey[] = "/org/asteroidos/health/profile/weight-kg";
const char BirthDateKey[] = "/org/asteroidos/health/profile/birth-date";
const char StepsGoalKey[] = "/org/asteroidos/ux/steps-goal";
const char SleepGoalKey[] = "/org/asteroidos/ux/sleep-goal";
const char PhoneProfileKey[] = "/org/asteroidos/btsyncd/phone-profile";
const char PhoneStepsGoalKey[] = "/org/asteroidos/btsyncd/phone-steps-goal";
const char PhoneSleepGoalKey[] = "/org/asteroidos/btsyncd/phone-sleep-goal";

static double number(const QString &text)
{
    bool ok = false;
    const double v = text.toDouble(&ok);
    return ok && std::isfinite(v) ? v : -1;
}

bool parse(const QByteArray &xml, Values *values, const QDate &today)
{
    QXmlStreamReader reader(xml);
    if (!reader.readNextStartElement() || reader.name() != QLatin1String("prof"))
        return false;
    Values v;
    while (reader.readNextStartElement()) {
        const QString name = reader.name().toString();
        if (name != QLatin1String("gd") && name != QLatin1String("hc") && name != QLatin1String("wk")
            && name != QLatin1String("bd") && name != QLatin1String("sg") && name != QLatin1String("sl")) {
            reader.skipCurrentElement();
            continue;
        }
        const QString text = reader.readElementText().trimmed();
        if (reader.hasError())
            break;
        if (name == QLatin1String("gd")) {
            if (text == QLatin1String("female") || text == QLatin1String("male") || text == QLatin1String("other"))
                v.gender = text;
        } else if (name == QLatin1String("hc")) {
            const double cm = number(text);
            if (cm >= 30 && cm <= 272)
                v.heightCm = (int)std::lround(cm);
        } else if (name == QLatin1String("wk")) {
            const double kg = number(text);
            if (kg >= 1 && kg <= 907)
                v.weightKg = std::round(kg * 10) / 10;
        } else if (name == QLatin1String("bd")) {
            // yyyy-MM-dd exactly (Qt's ISO date would take a date and time too)
            const QDate born = text.size() == 10 ? QDate::fromString(text, Qt::ISODate) : QDate();
            if (born.isValid() && born.year() >= 1900 && born < today)
                v.birthDate = born;
        } else if (name == QLatin1String("sg")) {
            const double steps = number(text);
            if (steps >= 1 && steps <= 100000 && steps == std::floor(steps))
                v.stepsGoal = (int)steps;
        } else {
            const double minutes = number(text);
            if (minutes >= 60 && minutes <= 1440 && minutes == std::floor(minutes))
                v.sleepGoalMin = (int)minutes;
        }
    }
    if (reader.hasError())
        return false;
    if (values)
        *values = v;
    return true;
}

QByteArray profileText(const Values &values)
{
    QByteArray text;
    if (!values.gender.isEmpty())
        text += "gd=" + values.gender.toLatin1() + ";";
    if (values.heightCm > 0)
        text += "hc=" + QByteArray::number(values.heightCm) + ";";
    if (values.weightKg > 0)
        text += "wk=" + QByteArray::number(values.weightKg, 'f', 1) + ";";
    if (values.birthDate.isValid())
        text += "bd=" + values.birthDate.toString(Qt::ISODate).toLatin1() + ";";
    return text;
}

// The key gets the value unless it has it already (a write of the same value is no change for its readers)
static void store(const char *key, const QVariant &value)
{
    MDConfItem item(QString::fromLatin1(key));
    const QVariant now = item.value();
    const bool same = value.typeId() == QMetaType::QString ? now.toString() == value.toString()
                                                            : now.isValid() && now.toDouble() == value.toDouble();
    if (!same || now.typeId() != value.typeId())
        item.set(value);
}

// A goal: written when the phone's differs from the one it sent last
static bool applyGoal(const char *key, const char *phoneKey, int goal)
{
    if (goal <= 0)
        return false;
    MDConfItem phone(QString::fromLatin1(phoneKey));
    if (phone.value().isValid() && phone.value().toInt() == goal)
        return false;
    store(key, goal);
    phone.set(goal);
    return true;
}

int apply(const Values &values)
{
    int written = None;
    if (values.hasProfile()) {
        const QString hash = QString::fromLatin1(
            QCryptographicHash::hash(profileText(values), QCryptographicHash::Sha256).toHex());
        MDConfItem phone(QString::fromLatin1(PhoneProfileKey));
        if (phone.value().toString() != hash) {
            if (!values.gender.isEmpty())
                store(GenderKey, values.gender);
            if (values.heightCm > 0)
                store(HeightKey, values.heightCm);
            if (values.weightKg > 0)
                store(WeightKey, values.weightKg);
            if (values.birthDate.isValid())
                store(BirthDateKey, values.birthDate.toString(Qt::ISODate));
            phone.set(hash);
            written |= Profile;
        }
    }
    if (applyGoal(StepsGoalKey, PhoneStepsGoalKey, values.stepsGoal))
        written |= StepsGoal;
    if (applyGoal(SleepGoalKey, PhoneSleepGoalKey, values.sleepGoalMin))
        written |= SleepGoal;
    return written;
}

} // namespace HealthProfile
