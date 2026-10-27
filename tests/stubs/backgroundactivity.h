/*
 * Test stub of libkeepalive's BackgroundActivity (the mce cpu-keepalive
 * session WakeHold uses): records run()/stop() instead of talking to mce.
 */
#ifndef BACKGROUNDACTIVITY_STUB_H
#define BACKGROUNDACTIVITY_STUB_H

#include <QObject>

class BackgroundActivity : public QObject
{
public:
    explicit BackgroundActivity(QObject *parent = nullptr) : QObject(parent) { last() = this; }
    ~BackgroundActivity() override { if (last() == this) last() = nullptr; }

    bool isRunning() const { return mRunning; }
    void run() { mRunning = true; runs()++; }
    void stop() { mRunning = false; stops()++; }

    // The newest instance (WakeHold has one)
    static BackgroundActivity *&last() { static BackgroundActivity *p = nullptr; return p; }
    static bool lastRunning() { return last() && last()->isRunning(); }
    static int &runs() { static int n = 0; return n; }
    static int &stops() { static int n = 0; return n; }

private:
    bool mRunning = false;
};

#endif // BACKGROUNDACTIVITY_STUB_H
