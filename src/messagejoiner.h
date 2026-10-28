/*
 * Copyright (C) 2026 - Reuben Horner <reubarb.scons@gmail.com>
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

#ifndef MESSAGEJOINER_H
#define MESSAGEJOINER_H

#include <QByteArray>
#include <QDebug>
#include <QList>
#include <QPair>
#include <QString>
#include <QVariantMap>
#include <cctype>
#include <utility>

/*
 * The phone's writes to one characteristic, joined until the closing tag of
 * each root element: the protocol allows any split (a long write comes as
 * chunks, the last one may end inside the closing tag), and a write may hold
 * more than one message. A new message (one of the characteristic's root
 * elements, at offset 0) while the last one is incomplete means the rest of
 * that one was lost (a disconnection): it is dropped, as is more than
 * maxBytes without an end. Text cannot contain the closing tag (XML escapes
 * '<'), and the protocol nests no root element in another.
 */
class MessageJoiner
{
public:
    MessageJoiner(const char *what, QList<QByteArray> roots, int maxBytes)
        : mWhat(what), mRoots(std::move(roots)), mMax(maxBytes) {}

    // BlueZ's offset of a write (the chunks of a long write)
    static int offsetOf(const QVariantMap &options)
    {
        return options.value("offset", 0).toInt();
    }

    // The complete messages after this write: (root element name, bytes)
    QList<QPair<QByteArray, QByteArray>> add(const QByteArray &value, int offset)
    {
        if(!mBuffer.isEmpty() && offset == 0 && startsMessage(value))
            drop("a new message began");
        mBuffer += value;
        QList<QPair<QByteArray, QByteArray>> messages;
        for(;;) {
            const int lt = mBuffer.indexOf('<');
            if(lt < 0) {
                if(!mBuffer.trimmed().isEmpty())
                    drop("no element");
                mBuffer.clear();
                break;
            }
            if(lt > 0) {
                if(!mBuffer.left(lt).trimmed().isEmpty())
                    qWarning().noquote() << QString("%1: dropped %2 bytes before an element").arg(mWhat).arg(lt);
                mBuffer.remove(0, lt);
            }
            int i = 1;
            while(i < mBuffer.size() && (isalnum((unsigned char) mBuffer.at(i)) || mBuffer.at(i) == '_' || mBuffer.at(i) == '-'))
                i++;
            if(i >= mBuffer.size())
                break;          // the name is still coming
            if(i == 1) {
                // not an element start ("</x>" left of a lost message, "<?", "<!")
                const int next = mBuffer.indexOf('<', 1);
                qWarning().noquote() << QString("%1: dropped %2 bytes of a stray tag").arg(mWhat).arg(next < 0 ? mBuffer.size() : next);
                if(next < 0) {
                    mBuffer.clear();
                    break;
                }
                mBuffer.remove(0, next);
                continue;
            }
            const QByteArray name = mBuffer.mid(1, i - 1);
            const QByteArray close = "</" + name + ">";
            const int c = mBuffer.indexOf(close, i);
            if(c < 0)
                break;          // the rest is still coming
            const int end = c + close.size();
            messages << qMakePair(name, mBuffer.left(end));
            mBuffer.remove(0, end);
        }
        if(mBuffer.size() > mMax)
            drop("too long without an end");
        return messages;
    }

    // Drops an incomplete message (logged)
    void drop(const char *why)
    {
        if(mBuffer.isEmpty())
            return;
        qWarning().noquote() << QString("%1: incomplete message of %2 bytes dropped: %3").arg(mWhat).arg(mBuffer.size()).arg(why);
        mBuffer.clear();
    }

private:
    bool startsMessage(const QByteArray &value) const
    {
        const QByteArray v = value.trimmed();
        for(const QByteArray &root : mRoots) {
            if(v.startsWith("<" + root + ">") || v.startsWith("<" + root + " "))
                return true;
        }
        return false;
    }

    const char *mWhat;
    QList<QByteArray> mRoots;
    int mMax;
    QByteArray mBuffer;
};

#endif // MESSAGEJOINER_H
