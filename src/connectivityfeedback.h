/*
 * Copyright (C) 2026 eos-mainline contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef CONNECTIVITYFEEDBACK_H
#define CONNECTIVITYFEEDBACK_H

#include <QObject>
#include <QString>

/*
 * Vibrate on connectivity change (stock's cw_debug_connectivity_vibrate, off by default;
 * Settings > Developer options, contract row CR-26): while the dconf key Key is true the
 * watch plays the ngfd event ui_confirm (two thuds) when the phone connects and ui_reject
 * (three thuds) when it disconnects, a haptic only (media.vibra, like qml-asteroid's
 * Haptics). The first state given is the starting one and plays nothing. Only changes
 * of the BLE link cause anything: no timer, no polling.
 */
class ConnectivityFeedback : public QObject
{
    Q_OBJECT
public:
    explicit ConnectivityFeedback(QObject *parent = nullptr) : QObject(parent) {}

    static constexpr const char *Key = "/org/asteroidos/btsyncd/connectivity-vibrate";
    static constexpr const char *ConnectEvent = "ui_confirm";
    static constexpr const char *DisconnectEvent = "ui_reject";

    // The phone's BLE link (BlueZManager::connected)
    void setPhoneLink(bool connected);

    static bool enabled();
    // What goes to ngfd (the system bus): Play(event, {media.vibra: true})
    static void play(const QString &event);

private:
    bool mKnown = false;
    bool mConnected = false;
};

#endif // CONNECTIVITYFEEDBACK_H
