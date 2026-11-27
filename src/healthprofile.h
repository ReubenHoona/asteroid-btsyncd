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

#ifndef HEALTHPROFILE_H
#define HEALTHPROFILE_H

#include <QByteArray>
#include <QDate>
#include <QString>

/*
 * The health profile and goals from the phone (M4 WO-11). Stock takes them
 * from the account: Settings > Health profile has only Gender, Height and
 * Weight, the age comes from the birthday on the account and the goals are
 * set in the phone app ("Set goal in %1$s mobile app"). The Fitbit app pushes
 * the account's birthday, weight, height and gender to Health Services the
 * first time and then whenever one of the four differs from the account
 * profile it saw last (utq.b); a change made in the watch's Settings stays
 * until then, and nothing goes back to the phone.
 *
 * Protocol (the health service, capabilities <takes>prof</takes>): the phone
 * writes on the health request characteristic, at every connection and when
 * its user profile changes,
 *   <prof><gd>female|male|other</gd><hc>CM</hc><wk>KG</wk><bd>YYYY-MM-DD</bd>
 *         <sg>STEPS</sg><sl>MINUTES</sl></prof>
 * with only what is set on the phone (any element may be missing, any order;
 * unknown elements are skipped). There is no answer: the write's response is
 * the acknowledgement, and a sync answer the phone waits for is not touched.
 * A watch without <takes>prof</takes> never gets the message (an older
 * btsyncd would answer it with a request error); a phone that never sends it
 * leaves the watch's values as they are.
 *
 * Values outside the ranges the watch takes are left out: height 30 to 272 cm
 * and weight 1 to 907 kg (Settings' pickers, stock MIN_CM / MAX_CM and
 * WeightUnit), a birth date from 1900 to yesterday, a steps goal of 1 to
 * 100000, a sleep goal of 60 to 1440 minutes.
 *
 * Who wins (stock's rule): the phone is the source. Three groups are compared
 * with what the phone sent last time (a hash of the group for the profile, the
 * number for a goal, kept in btsyncd's own dconf keys): the profile (gender,
 * height, weight, birth date, as stock compares the four together), the steps
 * goal and the sleep goal. A group that differs from the last one sent, or the
 * first one ever, is written to the keys below; one the phone sends unchanged
 * writes nothing, so a value set on the watch after it stays until the phone's
 * value changes. A group the phone does not send changes nothing.
 *
 * Written (read by Settings > Health profile, asteroid-sensorlogd, the Health
 * and Exercise apps, the launcher):
 *   /org/asteroidos/health/profile/gender      s  female, male, other
 *   /org/asteroidos/health/profile/height-cm   i
 *   /org/asteroidos/health/profile/weight-kg   d
 *   /org/asteroidos/health/profile/birth-date  s  yyyy-MM-dd
 *   /org/asteroidos/ux/steps-goal              i  steps a day
 *   /org/asteroidos/ux/sleep-goal              i  minutes a night
 * The AZM goal (150 a week) and the exercise days goal stay the watch's own:
 * Gadgetbridge has neither.
 */
namespace HealthProfile {

extern const char GenderKey[];
extern const char HeightKey[];
extern const char WeightKey[];
extern const char BirthDateKey[];
extern const char StepsGoalKey[];
extern const char SleepGoalKey[];
// what the phone sent last: the profile's hash, the goals
extern const char PhoneProfileKey[];
extern const char PhoneStepsGoalKey[];
extern const char PhoneSleepGoalKey[];

// One <prof> message; what it does not carry (or carries out of range) is empty / 0
struct Values {
    QString gender;
    int heightCm = 0;
    double weightKg = 0; // to 0.1 kg
    QDate birthDate;
    int stepsGoal = 0;
    int sleepGoalMin = 0;

    bool hasProfile() const { return !gender.isEmpty() || heightCm > 0 || weightKg > 0 || birthDate.isValid(); }
};

// The groups apply() wrote
enum Group { None = 0, Profile = 1, StepsGoal = 2, SleepGoal = 4 };

// Parses a <prof> message (false: not one, or not well-formed); today: the birth date must be before it
bool parse(const QByteArray &xml, Values *values, const QDate &today = QDate::currentDate());

// The profile group as one text ("gd=female;hc=168;..."), what the hash is made of
QByteArray profileText(const Values &values);

// Writes what the phone changed since it last sent it; returns the groups written (Group flags)
int apply(const Values &values);

} // namespace HealthProfile

#endif // HEALTHPROFILE_H
