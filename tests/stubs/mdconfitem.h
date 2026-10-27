/*
 * Test stub of mlite's MDConfItem: an in-memory key/value store, so the
 * tests need neither dconf nor mlite (Debian has no mlite6). Only what
 * btsyncd uses: value(), set(), unset().
 */
#ifndef MDCONFITEM_STUB_H
#define MDCONFITEM_STUB_H

#include <QHash>
#include <QObject>
#include <QString>
#include <QVariant>

class MDConfItem : public QObject
{
public:
    explicit MDConfItem(const QString &key, QObject *parent = nullptr) : QObject(parent), mKey(key) {}

    QString key() const { return mKey; }
    QVariant value() const { return store().value(mKey); }
    QVariant value(const QVariant &def) const { return store().value(mKey, def); }
    void set(const QVariant &value) { store().insert(mKey, value); sets()++; }
    void unset() { store().remove(mKey); unsets()++; }

    // For the tests: the whole store, and how often set()/unset() ran
    static QHash<QString, QVariant> &store() { static QHash<QString, QVariant> s; return s; }
    static int &sets() { static int n = 0; return n; }
    static int &unsets() { static int n = 0; return n; }
    static void reset() { store().clear(); sets() = 0; unsets() = 0; }

private:
    QString mKey;
};

#endif // MDCONFITEM_STUB_H
