/****************************************************************************
**
** Copyright (C) 2016 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of the QtQml module of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:LGPL$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 3 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL3 included in the
** packaging of this file. Please review the following information to
** ensure the GNU Lesser General Public License version 3 requirements
** will be met: https://www.gnu.org/licenses/lgpl-3.0.html.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 2.0 or (at your option) the GNU General
** Public license version 3 or any later version approved by the KDE Free
** Qt Foundation. The licenses are as published by the Free Software
** Foundation and appearing in the file LICENSE.GPL2 and LICENSE.GPL3
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-2.0.html and
** https://www.gnu.org/licenses/gpl-3.0.html.
**
** $QT_END_LICENSE$
**
****************************************************************************/

#include "qqmlcontext.h"
#include "qqmlcontext_p.h"
#include "qqmlcomponentattached_p.h"

#include "qqmlcomponent_p.h"
#include "qqmlexpression_p.h"
#include "qqmlengine_p.h"
#include "qqmlengine.h"
#include "qqmlinfo.h"
#include "qqmlabstracturlinterceptor.h"

#include <qjsengine.h>
#include <QtCore/qvarlengtharray.h>
#include <private/qmetaobject_p.h>
#include <QtCore/qdebug.h>

QT_BEGIN_NAMESPACE

/*!
    \class QQmlContext
    \brief The QQmlContext class defines a context within a QML engine.
    \inmodule QtQml

    Contexts allow data to be exposed to the QML components instantiated by the
    QML engine.

    Each QQmlContext contains a set of properties, distinct from its QObject
    properties, that allow data to be explicitly bound to a context by name.  The
    context properties are defined and updated by calling
    QQmlContext::setContextProperty().  The following example shows a Qt model
    being bound to a context and then accessed from a QML file.

    \code
    QQmlEngine engine;
    QStringListModel modelData;
    QQmlContext *context = new QQmlContext(engine.rootContext());
    context->setContextProperty("myModel", &modelData);

    QQmlComponent component(&engine);
    component.setData("import QtQuick 2.0; ListView { model: myModel }", QUrl());
    QObject *window = component.create(context);
    \endcode

    \note It is the responsibility of the creator to delete any QQmlContext it
    constructs. If the \c context object in the example is no longer needed when the
    \c window component instance is destroyed, the \c context must be destroyed explicitly.
    The simplest way to ensure this is to set \c window as the parent of \c context.

    To simplify binding and maintaining larger data sets, a context object can be set
    on a QQmlContext.  All the properties of the context object are available
    by name in the context, as though they were all individually added through calls
    to QQmlContext::setContextProperty().  Changes to the property's values are
    detected through the property's notify signal.  Setting a context object is both
    faster and easier than manually adding and maintaining context property values.

    The following example has the same effect as the previous one, but it uses a context
    object.

    \code
    class MyDataSet : public QObject {
        // ...
        Q_PROPERTY(QAbstractItemModel *myModel READ model NOTIFY modelChanged)
        // ...
    };

    MyDataSet myDataSet;
    QQmlEngine engine;
    QQmlContext *context = new QQmlContext(engine.rootContext());
    context->setContextObject(&myDataSet);

    QQmlComponent component(&engine);
    component.setData("import QtQuick 2.0; ListView { model: myModel }", QUrl());
    component.create(context);
    \endcode

    All properties added explicitly by QQmlContext::setContextProperty() take
    precedence over the context object's properties.

    \section2 The Context Hierarchy

    Contexts form a hierarchy. The root of this hierarchy is the QML engine's
    \l {QQmlEngine::rootContext()}{root context}. Child contexts inherit
    the context properties of their parents; if a child context sets a context property
    that already exists in its parent, the new context property overrides that of the
    parent.

    The following example defines two contexts - \c context1 and \c context2.  The
    second context overrides the "b" context property inherited from the first with a
    new value.

    \code
    QQmlEngine engine;
    QQmlContext *context1 = new QQmlContext(engine.rootContext());
    QQmlContext *context2 = new QQmlContext(context1);

    context1->setContextProperty("a", 9001);
    context1->setContextProperty("b", 9001);

    context2->setContextProperty("b", 42);
    \endcode

    While QML objects instantiated in a context are not strictly owned by that
    context, their bindings are.  If a context is destroyed, the property bindings of
    outstanding QML objects will stop evaluating.

    \warning Setting the context object or adding new context properties after an object
    has been created in that context is an expensive operation (essentially forcing all bindings
    to reevaluate). Thus whenever possible you should complete "setup" of the context
    before using it to create any objects.

    \sa {qtqml-cppintegration-exposecppattributes.html}{Exposing Attributes of C++ Types to QML}
*/

/*! \internal */
QQmlContext::QQmlContext(QQmlEngine *e, bool)
    : QObject(*(new QQmlContextPrivate(this, QQmlRefPointer<QQmlContextData>(), e)))
{
}

/*!
    Create a new QQmlContext as a child of \a engine's root context, and the
    QObject \a parent.
*/
QQmlContext::QQmlContext(QQmlEngine *engine, QObject *parent)
    : QObject(*(new QQmlContextPrivate(this, engine
                                       ? QQmlContextData::get(engine->rootContext())
                                       : QQmlRefPointer<QQmlContextData>())), parent)
{
}

/*!
    Create a new QQmlContext with the given \a parentContext, and the
    QObject \a parent.
*/
QQmlContext::QQmlContext(QQmlContext *parentContext, QObject *parent)
    : QObject(*(new QQmlContextPrivate(this, parentContext
                                       ? QQmlContextData::get(parentContext)
                                       : QQmlRefPointer<QQmlContextData>())), parent)
{
}

/*!
    \internal
*/
QQmlContext::QQmlContext(QQmlContextPrivate &dd, QObject *parent)
    : QObject(dd, parent)
{
}

/*!
    Destroys the QQmlContext.

    Any expressions, or sub-contexts dependent on this context will be
    invalidated, but not destroyed (unless they are parented to the QQmlContext
    object).
 */
QQmlContext::~QQmlContext()
{
    Q_D(QQmlContext);
    d->m_data->clearPublicContext();
}

/*!
    Returns whether the context is valid.

    To be valid, a context must have a engine, and it's contextObject(), if any,
    must not have been deleted.
*/
bool QQmlContext::isValid() const
{
    Q_D(const QQmlContext);
    return d->m_data->isValid();
}

/*!
    Return the context's QQmlEngine, or \nullptr if the context has no QQmlEngine or the
    QQmlEngine was destroyed.
*/
QQmlEngine *QQmlContext::engine() const
{
    Q_D(const QQmlContext);
    return d->m_data->engine();
}

/*!
    Return the context's parent QQmlContext, or \nullptr if this context has no
    parent or if the parent has been destroyed.
*/
QQmlContext *QQmlContext::parentContext() const
{
    Q_D(const QQmlContext);

    if (QQmlRefPointer<QQmlContextData> parent = d->m_data->parent())
        return parent->asQQmlContext();
    return nullptr;
}

/*!
    Return the context object, or \nullptr if there is no context object.
*/
QObject *QQmlContext::contextObject() const
{
    Q_D(const QQmlContext);
    return d->m_data->contextObject();
}

/*!
    Set the context \a object.
*/
void QQmlContext::setContextObject(QObject *object)
{
    Q_D(QQmlContext);

    QQmlRefPointer<QQmlContextData> data = d->m_data;

    if (data->isInternal()) {
        qWarning("QQmlContext: Cannot set context object for internal context.");
        return;
    }

    if (!data->isValid()) {
        qWarning("QQmlContext: Cannot set context object on invalid context.");
        return;
    }

    data->setContextObject(object);
    data->refreshExpressions();
}

/*!
    Set a the \a value of the \a name property on this context.
*/
void QQmlContext::setContextProperty(const QString &name, const QVariant &value)
{
    Q_D(QQmlContext);
    if (d->notifyIndex() == -1)
        d->setNotifyIndex(QMetaObjectPrivate::absoluteSignalCount(&QQmlContext::staticMetaObject));

    QQmlRefPointer<QQmlContextData> data = d->m_data;

    if (data->isInternal()) {
        qWarning("QQmlContext: Cannot set property on internal context.");
        return;
    }

    if (!data->isValid()) {
        qWarning("QQmlContext: Cannot set property on invalid context.");
        return;
    }

    int idx = data->propertyIndex(name);
    if (idx == -1) {
        data->addPropertyNameAndIndex(name, data->numIdValues() + d->numPropertyValues());
        d->appendPropertyValue(value);
        data->refreshExpressions();
    } else {
        d->setPropertyValue(idx, value);
        QMetaObject::activate(this, d->notifyIndex(), idx, nullptr);
    }

    if (auto *obj = qvariant_cast<QObject *>(value)) {
        connect(obj, &QObject::destroyed, this, [d, name](QObject *destroyed) {
            d->dropDestroyedQObject(name, destroyed);
        });
    }
}

/*!
    Set the \a value of the \a name property on this context.

    QQmlContext does \b not take ownership of \a value.
*/
void QQmlContext::setContextProperty(const QString &name, QObject *value)
{
    setContextProperty(name, QVariant::fromValue(value));
}

/*!
    \since 5.11

    Set a batch of \a properties on this context.

    Setting all properties in one batch avoids unnecessary
    refreshing  expressions, and is therefore recommended
    instead of calling \l setContextProperty() for each individual property.

    \sa QQmlContext::setContextProperty()
*/
void QQmlContext::setContextProperties(const QList<PropertyPair> &properties)
{
    Q_D(const QQmlContext);

    QQmlRefPointer<QQmlContextData> data = d->m_data;
    QQmlJavaScriptExpression *expressions = data->takeExpressions();
    QQmlRefPointer<QQmlContextData> childContexts = data->takeChildContexts();

    for (const auto &property : properties)
        setContextProperty(property.name, property.value);

    data->setExpressions(expressions);
    data->setChildContexts(childContexts);
    data->refreshExpressions();
}

/*!
    \since 5.11

    \class QQmlContext::PropertyPair
    \inmodule QtQml

    This struct contains a property name and a property value.
    It is used as a parameter for the \c setContextProperties function.

    \sa QQmlContext::setContextProperties()
*/

static bool readObjectProperty(
        const QQmlRefPointer<QQmlContextData> &data, QObject *object, const QString &name,
        QVariant *target)
{
    QQmlPropertyData local;
    if (QQmlPropertyData *property = QQmlPropertyCache::property(object, name, data, &local)) {
        *target = object->metaObject()->property(property->coreIndex()).read(object);
        return true;
    }
    return false;
}

/*!
  Returns the value of the \a name property for this context as a QVariant.
  If you know that the property you're looking for is a QObject assigned using
  a QML id in the current context, \l objectForName() is more convenient and
  faster. In contrast to \l objectForName() and \l nameForObject(), this method
  does traverse the context hierarchy and searches in parent contexts if the
  \a name is not found in the current one. It also considers any
  \l contextObject() you may have set.

  \sa objectForName(), nameForObject(), contextObject()
 */
QVariant QQmlContext::contextProperty(const QString &name) const
{
    Q_D(const QQmlContext);

    const QQmlRefPointer<QQmlContextData> data = d->m_data;

    const int idx = data->propertyIndex(name);
    if (idx == -1) {
        if (QObject *obj = data->contextObject()) {
            QVariant value;
            if (readObjectProperty(data, obj, name, &value))
                return value;
        }

        if (parentContext())
            return parentContext()->contextProperty(name);
    } else {
        if (idx >= d->numPropertyValues())
            return QVariant::fromValue(data->idValue(idx - d->numPropertyValues()));
        else
            return d->propertyValue(idx);
    }

    return QVariant();
}

/*!
  Returns the name of \a object in this context, or an empty string if \a object
  is not named in the context. Objects are named by \l setContextProperty(), or
  as properties of a context object, or by ids in the case of QML created
  contexts.

  If the object has multiple names, the first is returned.

  In contrast to \l contextProperty(), this method does not traverse the
  context hierarchy. If the name is not found in the current context, an empty
  String is returned.

  \sa contextProperty(), objectForName()
*/
QString QQmlContext::nameForObject(const QObject *object) const
{
    Q_D(const QQmlContext);

    return d->m_data->findObjectId(object);
}

/*!
  \since 6.2

  Returns the object for a given \a name in this context. Returns nullptr if
  \a name is not available in the context or if the value associated with
  \a name is not a QObject. Objects are named by \l setContextProperty(),
  or as properties of a context object, or by ids in the case of QML created
  contexts. In contrast to \l contextProperty(), this method does not traverse
  the context hierarchy. If the name is not found in the current context,
  nullptr is returned.

  \sa contextProperty(), nameForObject()
*/
QObject *QQmlContext::objectForName(const QString &name) const
{
    Q_D(const QQmlContext);

    QQmlRefPointer<QQmlContextData> data = d->m_data;
    if (const int propertyIndex = data->propertyIndex(name); propertyIndex >= 0) {
        const int numPropertyValues = d->numPropertyValues();
        if (propertyIndex < numPropertyValues)
            return qvariant_cast<QObject *>(d->propertyValue(propertyIndex));
        return data->idValue(propertyIndex - numPropertyValues);
    }

    if (QObject *obj = data->contextObject()) {
        QVariant result;
        if (readObjectProperty(data, obj, name, &result))
            return qvariant_cast<QObject *>(result);
    }

    return nullptr;
}

/*!
    Resolves the URL \a src relative to the URL of the
    containing component.

    \sa QQmlEngine::baseUrl(), setBaseUrl()
*/
QUrl QQmlContext::resolvedUrl(const QUrl &src) const
{
    Q_D(const QQmlContext);
    return d->m_data->resolvedUrl(src);
}

/*!
    Explicitly sets the url resolvedUrl() will use for relative references to \a baseUrl.

    Calling this function will override the url of the containing
    component used by default.

    \sa resolvedUrl()
*/
void QQmlContext::setBaseUrl(const QUrl &baseUrl)
{
    Q_D(QQmlContext);
    d->m_data->setBaseUrl(baseUrl);
    d->m_data->setBaseUrlString(baseUrl.toString());
}

/*!
    Returns the base url of the component, or the containing component
    if none is set.
*/
QUrl QQmlContext::baseUrl() const
{
    Q_D(const QQmlContext);
    return d->m_data->baseUrl();
}

/*!
 * \internal
 */
QJSValue QQmlContext::importedScript(const QString &name) const
{
    Q_D(const QQmlContext);

    QQmlTypeNameCache::Result r = d->m_data->imports()->query(name);
    QV4::Scope scope(engine()->handle());
    QV4::ScopedObject scripts(scope, d->m_data->importedScripts().valueRef());
    return scripts ? QJSValuePrivate::fromReturnedValue(scripts->get(r.scriptIndex))
                   : QJSValue(QJSValue::UndefinedValue);
}

qsizetype QQmlContextPrivate::context_count(QQmlListProperty<QObject> *prop)
{
    QQmlContext *context = static_cast<QQmlContext*>(prop->object);
    QQmlContextPrivate *d = QQmlContextPrivate::get(context);
    int contextProperty = (int)(quintptr)prop->data;

    if (d->propertyValue(contextProperty).userType() != qMetaTypeId<QList<QObject*> >())
        return 0;
    else
        return ((const QList<QObject> *)d->propertyValue(contextProperty).constData())->count();
}

QObject *QQmlContextPrivate::context_at(QQmlListProperty<QObject> *prop, qsizetype index)
{
    QQmlContext *context = static_cast<QQmlContext*>(prop->object);
    QQmlContextPrivate *d = QQmlContextPrivate::get(context);
    int contextProperty = (int)(quintptr)prop->data;

    if (d->propertyValue(contextProperty).userType() != qMetaTypeId<QList<QObject*> >())
        return nullptr;
    else
        return ((const QList<QObject*> *)d->propertyValue(contextProperty).constData())->at(index);
}

void QQmlContextPrivate::dropDestroyedQObject(const QString &name, QObject *destroyed)
{
    if (!m_data->isValid())
        return;

    const int idx = m_data->propertyIndex(name);
    Q_ASSERT(idx >= 0);
    if (qvariant_cast<QObject *>(propertyValue(idx)) != destroyed)
        return;

    setPropertyValue(idx, QVariant::fromValue<QObject *>(nullptr));
    QMetaObject::activate(q_func(), notifyIndex(), idx, nullptr);
}

void QQmlContextPrivate::emitDestruction()
{
    m_data->emitDestruction();
}

// m_data is owned by the public context. When the public context is reset to nullptr, it will be
// deref'd. It's OK to pass a half-created publicContext here. We will not dereference it during
// construction.
QQmlContextPrivate::QQmlContextPrivate(
        QQmlContext *publicContext, const QQmlRefPointer<QQmlContextData> &parent,
        QQmlEngine *engine) :
    m_data(new QQmlContextData(QQmlContextData::OwnedByPublicContext, publicContext,
                               parent, engine))
{
    Q_ASSERT(publicContext != nullptr);
}

QT_END_NAMESPACE

#include "moc_qqmlcontext.cpp"
