/****************************************************************************
**
** Copyright (C) 2020 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of the tools applications of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:GPL-EXCEPT$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3 as published by the Free Software
** Foundation with exceptions as appearing in the file LICENSE.GPL3-EXCEPT
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-3.0.html.
**
** $QT_END_LICENSE$
**
****************************************************************************/

#include "qqmldomtypesreader_p.h"
#include "qqmldomelements_p.h"
#include "qqmldomcompare_p.h"
#include "qqmldomfieldfilter_p.h"

#include <QtQml/private/qqmljsparser_p.h>
#include <QtQml/private/qqmljslexer_p.h>
#include <QtQml/private/qqmljsengine_p.h>
#ifdef QMLDOM_STANDALONE
#    include "qmlcompiler/qqmljstypedescriptionreader_p.h"
#else
#    include <private/qqmljstypedescriptionreader_p.h>
#endif

#include <QtCore/qdir.h>

QT_BEGIN_NAMESPACE

namespace QQmlJS {
namespace Dom {

using namespace QQmlJS::AST;

static ErrorGroups myParseErrors()
{
    static ErrorGroups errs = { { NewErrorGroup("Dom"), NewErrorGroup("QmltypesFile"),
                                  NewErrorGroup("Parsing") } };
    return errs;
}

void QmltypesReader::insertProperty(QQmlJSScope::Ptr jsScope, const QQmlJSMetaProperty &property,
                                    QMap<int, QmlObject> &objs)
{
    PropertyDefinition prop;
    prop.name = property.propertyName();
    prop.typeName = property.typeName();
    prop.isPointer = property.isPointer();
    prop.isReadonly = !property.isWritable();
    prop.isRequired = jsScope->isPropertyLocallyRequired(prop.name);
    prop.isList = property.isList();
    int revision = property.revision();
    prop.isFinal = property.isFinal();
    prop.bindable = property.bindable();
    prop.read = property.read();
    prop.write = property.write();
    prop.notify = property.notify();

    if (prop.name.isEmpty() || prop.typeName.isEmpty()) {
        addError(myParseErrors()
                         .warning(tr("Property object is missing a name or type script binding."))
                         .handle());
        return;
    }
    objs[revision].addPropertyDef(prop, AddOption::KeepExisting);
}

void QmltypesReader::insertSignalOrMethod(const QQmlJSMetaMethod &metaMethod,
                                          QMap<int, QmlObject> &objs)
{
    MethodInfo methodInfo;
    // ### confusion between Method and Slot. Method should be removed.
    switch (metaMethod.methodType()) {
    case QQmlJSMetaMethod::Method:
    case QQmlJSMetaMethod::Slot:
        methodInfo.methodType = MethodInfo::MethodType::Method;
        break;
    case QQmlJSMetaMethod::Signal:
        methodInfo.methodType = MethodInfo::MethodType::Signal;
        break;
    default:
        Q_UNREACHABLE();
    }
    QStringList pNames = metaMethod.parameterNames();
    QStringList pTypes = metaMethod.parameterTypeNames();
    qsizetype nParam = qMax(pNames.size(), pTypes.size());
    for (int i = 0; i < nParam; ++i) {
        MethodParameter param;
        param.name = ((i < pNames.size()) ? pNames.at(i) : QString());
        param.typeName = ((i < pTypes.size()) ? pTypes.at(i) : QString());
        methodInfo.parameters.append(param);
    }
    methodInfo.name = metaMethod.methodName();
    methodInfo.typeName = metaMethod.returnTypeName();
    int revision = metaMethod.revision();
    methodInfo.isConstructor = metaMethod.isConstructor();
    if (methodInfo.name.isEmpty()) {
        addError(myParseErrors().error(tr("Method or signal is missing a name.")).handle());
        return;
    }

    objs[revision].addMethod(methodInfo, AddOption::KeepExisting);
}

EnumDecl QmltypesReader::enumFromMetaEnum(const QQmlJSMetaEnum &metaEnum)
{
    EnumDecl res;
    res.setName(metaEnum.name());
    res.setAlias(metaEnum.alias());
    res.setIsFlag(metaEnum.isFlag());
    QList<EnumItem> values;
    int lastValue = -1;
    for (const auto &k : metaEnum.keys()) {
        if (metaEnum.hasValues())
            lastValue = metaEnum.value(k);
        else
            ++lastValue;
        values.append(EnumItem(k, lastValue));
    }
    res.setValues(values);
    return res;
}

void QmltypesReader::insertComponent(const QQmlJSScope::Ptr &jsScope,
                                     const QList<QQmlJSScope::Export> &exportsList)
{
    QmltypesComponent comp;
    QMap<int, QmlObject> objects;
    objects.insert(0, QmlObject());
    bool incrementedPath = false;
    QString prototype;
    QString defaultPropertyName;
    {
        QHash<QString, QQmlJSMetaProperty> els = jsScope->ownProperties();
        auto it = els.cbegin();
        auto end = els.cend();
        while (it != end) {
            insertProperty(jsScope, it.value(), objects);
            ++it;
        }
    }
    {
        QMultiHash<QString, QQmlJSMetaMethod> els = jsScope->ownMethods();
        auto it = els.cbegin();
        auto end = els.cend();
        while (it != end) {
            insertSignalOrMethod(it.value(), objects);
            ++it;
        }
    }
    {
        QHash<QString, QQmlJSMetaEnum> els = jsScope->ownEnumerations();
        auto it = els.cbegin();
        auto end = els.cend();
        while (it != end) {
            comp.addEnumeration(enumFromMetaEnum(it.value()));
            ++it;
        }
    }
    comp.setFileName(jsScope->fileName());
    comp.setName(jsScope->internalName());
    m_currentPath = m_currentPath.key(comp.name())
                            .index(qmltypesFilePtr()->components().values(comp.name()).length());
    incrementedPath = true;
    prototype = jsScope->baseTypeName();
#if QT_VERSION <= 0x060200
    defaultPropertyName = jsScope->defaultPropertyName();
#else
    defaultPropertyName = jsScope->ownDefaultPropertyName();
#endif // QT_VERSION <= 0x060200
    comp.setInterfaceNames(jsScope->interfaceNames());
    QString typeName = jsScope->ownAttachedTypeName();
    comp.setAttachedTypeName(typeName);
    if (!typeName.isEmpty())
        comp.setAttachedTypePath(Paths::lookupCppTypePath(typeName));
    comp.setIsSingleton(jsScope->isSingleton());
    comp.setIsCreatable(jsScope->isCreatable());
    comp.setIsComposite(jsScope->isComposite());
    comp.setHasCustomParser(jsScope->hasCustomParser());
    comp.setValueTypeName(jsScope->valueTypeName());
    comp.setAccessSemantics(jsScope->accessSemantics());
    comp.setExtensionTypeName(jsScope->extensionTypeName());
    Path exportSourcePath = qmltypesFile().canonicalPath();
    QMap<int, Path> revToPath;
    auto it = objects.end();
    auto begin = objects.begin();
    int objectIndex = 0;
    QList<int> metaRevs;
    Path compPath = qmltypesFile()
                            .canonicalPath()
                            .field(Fields::components)
                            .key(comp.name())
                            .index(qmltypesFilePtr()->components().values(comp.name()).length());
    // emit & map objs

    // exports:
    QList<Export> exports;
    int iExport = 0;
    for (const QQmlJSScope::Export &jsE : exportsList) {
        int metaRev = jsE.version().toEncodedVersion<int>();
        ++iExport;
        Export e;
        e.uri = jsE.package();
        e.typeName = jsE.type();
        auto v = jsE.version();
        e.version = Version((v.hasMajorVersion() ? v.majorVersion() : Version::Latest),
                            (v.hasMinorVersion() ? v.minorVersion() : Version::Latest));
        e.typePath = revToPath.value(metaRev);
        objects[metaRev];
        e.exportSourcePath = exportSourcePath;
        comp.addExport(e);
    }

    while (it != begin) {
        --it;
        if (it.key() < 0) {
            addError(myParseErrors().error(
                    tr("negative meta revision %1 not supported").arg(it.key())));
        }
        revToPath.insert(it.key(), compPath.field(Fields::objects).index(objectIndex));
        Path nextObjectPath = compPath.field(Fields::objects).index(++objectIndex);
        if (it == begin) {
            if (!prototype.isEmpty())
                it->addPrototypePath(Paths::lookupCppTypePath(prototype));
            it->setName(prototype);
        } else {
            it->addPrototypePath(nextObjectPath);
            it->setName(comp.name() + QLatin1String("-") + QString::number(it.key()));
        }
        comp.addObject(*it);
        metaRevs.append(it.key());
    }
    comp.setMetaRevisions(metaRevs);

    if (comp.name().isEmpty()) {
        addError(myParseErrors()
                         .error(tr("Component definition is missing a name binding."))
                         .handle());
        return;
    }
    qmltypesFilePtr()->addComponent(comp, AddOption::KeepExisting);
    if (incrementedPath)
        m_currentPath = m_currentPath.dropTail().dropTail();
}

bool QmltypesReader::parse()
{
    QQmlJSTypeDescriptionReader reader(qmltypesFilePtr()->canonicalFilePath(),
                                       qmltypesFilePtr()->code());
    QStringList dependencies;
#if QT_VERSION < QT_VERSION_CHECK(6, 3, 0)
    QHash<QString, QQmlJSScope::Ptr> objects;
    m_isValid = reader(&objects, &dependencies);
    for (const auto &obj : qAsConst(objects))
        insertComponent(obj, obj->exports());
#else
    QHash<QString, QQmlJSExportedScope> objects;
    m_isValid = reader(&objects, &dependencies);
    for (const auto &obj : qAsConst(objects))
        insertComponent(obj.scope, obj.exports);
#endif
    qmltypesFilePtr()->setIsValid(m_isValid);
    return m_isValid;
}

void QmltypesReader::addError(ErrorMessage message)
{
    if (message.file.isEmpty())
        message.file = qmltypesFile().canonicalFilePath();
    if (!message.path)
        message.path = m_currentPath;
    qmltypesFilePtr()->addErrorLocal(message.handle());
}

} // end namespace Dom
} // end namespace QQmlJS
QT_END_NAMESPACE
