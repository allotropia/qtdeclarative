/****************************************************************************
**
** Copyright (C) 2013 Digia Plc and/or its subsidiary(-ies).
** Contact: http://www.qt-project.org/legal
**
** This file is part of the QtQml module of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:LGPL$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and Digia.  For licensing terms and
** conditions see http://qt.digia.com/licensing.  For further information
** use the contact form at http://qt.digia.com/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 2.1 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL included in the
** packaging of this file.  Please review the following information to
** ensure the GNU Lesser General Public License version 2.1 requirements
** will be met: http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** In addition, as a special exception, Digia gives you certain additional
** rights.  These rights are described in the Digia Qt LGPL Exception
** version 1.1, included in the file LGPL_EXCEPTION.txt in this package.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3.0 as published by the Free Software
** Foundation and appearing in the file LICENSE.GPL included in the
** packaging of this file.  Please review the following information to
** ensure the GNU General Public License version 3.0 requirements will be
** met: http://www.gnu.org/copyleft/gpl.html.
**
**
** $QT_END_LICENSE$
**
****************************************************************************/
#ifndef QV4REGEXP_H
#define QV4REGEXP_H

#include <QString>
#include <QVector>

#include <wtf/RefPtr.h>
#include <wtf/FastAllocBase.h>
#include <wtf/BumpPointerAllocator.h>

#include <limits.h>

#include <yarr/Yarr.h>
#include <yarr/YarrInterpreter.h>
#include <yarr/YarrJIT.h>

#include "qv4managed_p.h"
#include "qv4engine_p.h"

QT_BEGIN_NAMESPACE

namespace QV4 {

struct ExecutionEngine;

struct RegExpCacheKey
{
    RegExpCacheKey(const QString &pattern, bool ignoreCase, bool multiLine)
        : pattern(pattern)
        , ignoreCase(ignoreCase)
        , multiLine(multiLine)
    { }
    explicit inline RegExpCacheKey(const RegExp *re);

    bool operator==(const RegExpCacheKey &other) const
    { return pattern == other.pattern && ignoreCase == other.ignoreCase && multiLine == other.multiLine; }
    bool operator!=(const RegExpCacheKey &other) const
    { return !operator==(other); }

    QString pattern;
    uint ignoreCase : 1;
    uint multiLine : 1;
};

inline uint qHash(const RegExpCacheKey& key, uint seed = 0) Q_DECL_NOTHROW
{ return qHash(key.pattern, seed); }

class RegExpCache : public QHash<RegExpCacheKey, RegExp*>
{
public:
    ~RegExpCache();
};

class RegExp : public Managed
{
    struct Data : Managed::Data {
        QString pattern;
        OwnPtr<JSC::Yarr::BytecodePattern> byteCode;
#if ENABLE(YARR_JIT)
        JSC::Yarr::YarrCodeBlock jitCode;
#endif
        RegExpCache *cache;
        int subPatternCount;
        bool ignoreCase;
        bool multiLine;
    };
    struct {
        QString pattern;
        OwnPtr<JSC::Yarr::BytecodePattern> byteCode;
#if ENABLE(YARR_JIT)
        JSC::Yarr::YarrCodeBlock jitCode;
#endif
        RegExpCache *cache;
        int subPatternCount;
        bool ignoreCase;
        bool multiLine;
    } __data;
    V4_MANAGED_NEW
    Q_MANAGED_TYPE(RegExp)


    QString pattern() const { return d()->pattern; }
    OwnPtr<JSC::Yarr::BytecodePattern> &byteCode() { return d()->byteCode; }
#if ENABLE(YARR_JIT)
    JSC::Yarr::YarrCodeBlock jitCode() const { return d()->jitCode; }
#endif
    RegExpCache *cache() const { return d()->cache; }
    int subPatternCount() const { return d()->subPatternCount; }
    bool ignoreCase() const { return d()->ignoreCase; }
    bool multiLine() const { return d()->multiLine; }

    static RegExp* create(ExecutionEngine* engine, const QString& pattern, bool ignoreCase = false, bool multiline = false);
    ~RegExp();

    bool isValid() const { return d()->byteCode.get(); }

    uint match(const QString& string, int start, uint *matchOffsets);

    int captureCount() const { return subPatternCount() + 1; }

    static void destroy(Managed *that);
    static void markObjects(Managed *that, QV4::ExecutionEngine *e);

private:
    friend class RegExpCache;
    Q_DISABLE_COPY(RegExp);
    RegExp(ExecutionEngine* engine, const QString& pattern, bool ignoreCase, bool multiline);

};

inline RegExpCacheKey::RegExpCacheKey(const RegExp *re)
    : pattern(re->pattern())
    , ignoreCase(re->ignoreCase())
    , multiLine(re->multiLine())
{}


}

QT_END_NAMESPACE

#endif // QV4REGEXP_H
