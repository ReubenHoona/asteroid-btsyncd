/*
 * Test stub of mlite's MDConfItem: an in-memory key/value store, so the
 * tests need neither dconf nor mlite (Debian has no mlite6). What btsyncd
 * and its tests use: value(), set(), unset(), sync() and valueChanged().
 *
 * As with dconf, a write is seen at once by value() of every item of the key
 * in the process, and valueChanged() comes later (queued, from the event loop)
 * on every item of the key whose value it changed, the writing one included.
 */
#ifndef MDCONFITEM_STUB_H
#define MDCONFITEM_STUB_H

#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QVariant>

class MDConfItem : public QObject
{
    Q_OBJECT
public:
    explicit MDConfItem(const QString &key, QObject *parent = nullptr)
        : QObject(parent), mKey(key), mSeen(store().value(key))
    {
        items().append(this);
    }
    ~MDConfItem() override { items().removeAll(this); }

    QString key() const { return mKey; }
    QVariant value() const { return store().value(mKey); }
    QVariant value(const QVariant &def) const { return store().value(mKey, def); }
    void set(const QVariant &value)
    {
        store().insert(mKey, value);
        sets()++;
        changed(mKey);
    }
    void unset()
    {
        store().remove(mKey);
        unsets()++;
        changed(mKey);
    }
    // dconf's writes are done here at once
    void sync() {}

    // For the tests: the whole store, and how often set()/unset() ran
    static QHash<QString, QVariant> &store() { static QHash<QString, QVariant> s; return s; }
    static int &sets() { static int n = 0; return n; }
    static int &unsets() { static int n = 0; return n; }
    static void reset() { store().clear(); sets() = 0; unsets() = 0; }

signals:
    void valueChanged();

private:
    static QList<MDConfItem *> &items() { static QList<MDConfItem *> l; return l; }
    static void changed(const QString &key)
    {
        for (MDConfItem *item : items()) {
            if (item->mKey != key)
                continue;
            QPointer<MDConfItem> p(item);
            QMetaObject::invokeMethod(item, [p] {
                if (!p)
                    return;
                const QVariant now = store().value(p->mKey);
                if (now == p->mSeen && now.isValid() == p->mSeen.isValid())
                    return;
                p->mSeen = now;
                emit p->valueChanged();
            }, Qt::QueuedConnection);
        }
    }

    QString mKey;
    QVariant mSeen;
};

#endif // MDCONFITEM_STUB_H
