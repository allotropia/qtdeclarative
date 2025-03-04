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

#include "qqmlcomponent.h"
#include "qqmlcomponent_p.h"
#include "qqmlcomponentattached_p.h"

#include "qqmlcontext_p.h"
#include "qqmlengine_p.h"
#include "qqmlvme_p.h"
#include "qqml.h"
#include "qqmlengine.h"
#include "qqmlbinding_p.h"
#include "qqmlincubator.h"
#include "qqmlincubator_p.h"
#include <private/qqmljavascriptexpression_p.h>
#include <private/qqmlsourcecoordinate_p.h>

#include <private/qv4functionobject_p.h>
#include <private/qv4script_p.h>
#include <private/qv4scopedvalue_p.h>
#include <private/qv4objectiterator_p.h>
#include <private/qv4qobjectwrapper_p.h>
#include <private/qv4jscall_p.h>

#include <QDir>
#include <QStack>
#include <QStringList>
#include <QThreadStorage>
#include <QtCore/qdebug.h>
#include <QtCore/qloggingcategory.h>
#include <qqmlinfo.h>

namespace {
    QThreadStorage<int> creationDepth;
}

Q_LOGGING_CATEGORY(lcQmlComponentGeneral, "qt.qml.qmlcomponent")

QT_BEGIN_NAMESPACE

class QQmlComponentExtension : public QV4::ExecutionEngine::Deletable
{
public:
    QQmlComponentExtension(QV4::ExecutionEngine *v4);
    virtual ~QQmlComponentExtension();

    QV4::PersistentValue incubationProto;
};
V4_DEFINE_EXTENSION(QQmlComponentExtension, componentExtension);

/*!
    \class QQmlComponent
    \since 5.0
    \inmodule QtQml

    \brief The QQmlComponent class encapsulates a QML component definition.

    Components are reusable, encapsulated QML types with well-defined interfaces.

    A QQmlComponent instance can be created from a QML file.
    For example, if there is a \c main.qml file like this:

    \qml
    import QtQuick 2.0

    Item {
        width: 200
        height: 200
    }
    \endqml

    The following code loads this QML file as a component, creates an instance of
    this component using create(), and then queries the \l Item's \l {Item::}{width}
    value:

    \code
    QQmlEngine *engine = new QQmlEngine;
    QQmlComponent component(engine, QUrl::fromLocalFile("main.qml"));

    QObject *myObject = component.create();
    QQuickItem *item = qobject_cast<QQuickItem*>(myObject);
    int width = item->width();  // width = 200
    \endcode

    To create instances of a component in code where a QQmlEngine instance is
    not available, you can use \l qmlContext() or \l qmlEngine(). For example,
    in the scenario below, child items are being created within a QQuickItem
    subclass:

    \code
    void MyCppItem::init()
    {
        QQmlEngine *engine = qmlEngine(this);
        // Or:
        // QQmlEngine *engine = qmlContext(this)->engine();
        QQmlComponent component(engine, QUrl::fromLocalFile("MyItem.qml"));
        QQuickItem *childItem = qobject_cast<QQuickItem*>(component.create());
        childItem->setParentItem(this);
    }
    \endcode

    Note that these functions will return \c null when called inside the
    constructor of a QObject subclass, as the instance will not yet have
    a context nor engine.

    \section2 Network Components

    If the URL passed to QQmlComponent is a network resource, or if the QML document references a
    network resource, the QQmlComponent has to fetch the network data before it is able to create
    objects. In this case, the QQmlComponent will have a \l {QQmlComponent::Loading}{Loading}
    \l {QQmlComponent::status()}{status}. An application will have to wait until the component
    is \l {QQmlComponent::Ready}{Ready} before calling \l {QQmlComponent::create()}.

    The following example shows how to load a QML file from a network resource. After creating
    the QQmlComponent, it tests whether the component is loading. If it is, it connects to the
    QQmlComponent::statusChanged() signal and otherwise calls the \c {continueLoading()} method
    directly. Note that QQmlComponent::isLoading() may be false for a network component if the
    component has been cached and is ready immediately.

    \code
    MyApplication::MyApplication()
    {
        // ...
        component = new QQmlComponent(engine, QUrl("http://www.example.com/main.qml"));
        if (component->isLoading()) {
            QObject::connect(component, &QQmlComponent::statusChanged,
                             this, &MyApplication::continueLoading);
        } else {
            continueLoading();
        }
    }

    void MyApplication::continueLoading()
    {
        if (component->isError()) {
            qWarning() << component->errors();
        } else {
            QObject *myObject = component->create();
        }
    }
    \endcode
*/

/*!
    \qmltype Component
    \instantiates QQmlComponent
    \ingroup qml-utility-elements
    \inqmlmodule QtQml
    \brief Encapsulates a QML component definition.

    Components are reusable, encapsulated QML types with well-defined interfaces.

    Components are often defined by \l {{QML Documents}}{component files} -
    that is, \c .qml files. The \e Component type essentially allows QML components
    to be defined inline, within a \l {QML Documents}{QML document}, rather than as a separate QML file.
    This may be useful for reusing a small component within a QML file, or for defining
    a component that logically belongs with other QML components within a file.

    For example, here is a component that is used by multiple \l Loader objects.
    It contains a single item, a \l Rectangle:

    \snippet qml/component.qml 0

    Notice that while a \l Rectangle by itself would be automatically
    rendered and displayed, this is not the case for the above rectangle
    because it is defined inside a \c Component. The component encapsulates the
    QML types within, as if they were defined in a separate QML
    file, and is not loaded until requested (in this case, by the
    two \l Loader objects). Because Component is not derived from Item, you cannot
    anchor anything to it.

    Defining a \c Component is similar to defining a \l {QML Documents}{QML document}.
    A QML document has a single top-level item that defines the behavior and
    properties of that component, and cannot define properties or behavior outside
    of that top-level item. In the same way, a \c Component definition contains a single
    top level item (which in the above example is a \l Rectangle) and cannot define any
    data outside of this item, with the exception of an \e id (which in the above example
    is \e redSquare).

    The \c Component type is commonly used to provide graphical components
    for views. For example, the ListView::delegate property requires a \c Component
    to specify how each list item is to be displayed.

    \c Component objects can also be created dynamically using
    \l{QtQml::Qt::createComponent()}{Qt.createComponent()}.

    \section2 Creation Context

    The creation context of a Component corresponds to the context where the Component was declared.
    This context is used as the parent context (creating a \l{qtqml-documents-scope.html#component-instance-hierarchy}{context hierarchy})
    when the component is instantiated by an object such as a ListView or a Loader.

    In the following example, \c comp1 is created within the root context of MyItem.qml, and any objects
    instantiated from this component will have access to the ids and properties within that context,
    such as \c internalSettings.color. When \c comp1 is used as a ListView delegate in another context
    (as in main.qml below), it will continue to have access to the properties of its creation context
    (which would otherwise be private to external users).

    \table
    \row
    \li MyItem.qml
    \li \snippet qml/component/MyItem.qml 0
    \row
    \li main.qml
    \li \snippet qml/component/main.qml 0
    \endtable

    It is important that the lifetime of the creation context outlive any created objects. See
    \l{Maintaining Dynamically Created Objects} for more details.
*/

/*!
    \qmlattachedsignal Component::completed()

    Emitted after the object has been instantiated. This can be used to
    execute script code at startup, once the full QML environment has been
    established.

    The \c onCompleted signal handler can be declared on any object. The order
    of running the handlers is undefined.

    \qml
    Rectangle {
        Component.onCompleted: console.log("Completed Running!")
        Rectangle {
            Component.onCompleted: console.log("Nested Completed Running!")
        }
    }
    \endqml
*/

/*!
    \qmlattachedsignal Component::destruction()

    Emitted as the object begins destruction. This can be used to undo
    work done in response to the \l {completed}{completed()} signal, or other
    imperative code in your application.

    The \c onDestruction signal handler can be declared on any object. The
    order of running the handlers is undefined.

    \qml
    Rectangle {
        Component.onDestruction: console.log("Destruction Beginning!")
        Rectangle {
            Component.onDestruction: console.log("Nested Destruction Beginning!")
        }
    }
    \endqml

    \sa {Qt QML}
*/

/*!
    \enum QQmlComponent::Status

    Specifies the loading status of the QQmlComponent.

    \value Null This QQmlComponent has no data. Call loadUrl() or setData() to add QML content.
    \value Ready This QQmlComponent is ready and create() may be called.
    \value Loading This QQmlComponent is loading network data.
    \value Error An error has occurred. Call errors() to retrieve a list of \l {QQmlError}{errors}.
*/

/*!
    \enum QQmlComponent::CompilationMode

    Specifies whether the QQmlComponent should load the component immediately, or asynchonously.

    \value PreferSynchronous Prefer loading/compiling the component immediately, blocking the thread.
           This is not always possible; for example, remote URLs will always load asynchronously.
    \value Asynchronous Load/compile the component in a background thread.
*/

void QQmlComponentPrivate::typeDataReady(QQmlTypeData *)
{
    Q_Q(QQmlComponent);

    Q_ASSERT(typeData);

    fromTypeData(typeData);
    typeData.reset();
    progress = 1.0;

    emit q->statusChanged(q->status());
    emit q->progressChanged(progress);
}

void QQmlComponentPrivate::typeDataProgress(QQmlTypeData *, qreal p)
{
    Q_Q(QQmlComponent);

    progress = p;

    emit q->progressChanged(p);
}

void QQmlComponentPrivate::fromTypeData(const QQmlRefPointer<QQmlTypeData> &data)
{
    url = data->finalUrl();
    compilationUnit.reset(data->compilationUnit());

    if (!compilationUnit) {
        Q_ASSERT(data->isError());
        state.errors = data->errors();
    }
}

RequiredProperties &QQmlComponentPrivate::requiredProperties()
{
    return state.creator->requiredProperties();
}

bool QQmlComponentPrivate::hadRequiredProperties() const
{
    return state.creator->componentHadRequiredProperties();
}

void QQmlComponentPrivate::clear()
{
    if (typeData) {
        typeData->unregisterCallback(this);
        typeData.reset();
    }

    compilationUnit.reset();
}

QObject *QQmlComponentPrivate::doBeginCreate(QQmlComponent *q, QQmlContext *context)
{
    if (!engine) {
        // ###Qt6: In Qt 6, it should be impossible for users to create a QQmlComponent without an engine, and we can remove this check
        qWarning("QQmlComponent: Must provide an engine before calling create");
        return nullptr;
    }
    if (!context)
        context = engine->rootContext();
    return q->beginCreate(context);
}

bool QQmlComponentPrivate::setInitialProperty(
        QObject *base, const QString &name, const QVariant &value)
{
    const QStringList properties = name.split(u'.');

    if (properties.size() > 1) {
        QV4::Scope scope(engine->handle());
        QV4::ScopedObject object(scope, QV4::QObjectWrapper::wrap(scope.engine, base));
        QV4::ScopedString segment(scope);

        for (int i = 0; i < properties.length() - 1; ++i) {
            segment = scope.engine->newString(properties.at(i));
            object = object->get(segment);
            if (scope.engine->hasException)
                break;
        }
        segment = scope.engine->newString(properties.last());
        object->put(segment, scope.engine->metaTypeToJS(value.metaType(), value.constData()));
        if (scope.engine->hasException) {
            state.errors.push_back(scope.engine->catchExceptionAsQmlError());
            scope.engine->hasException = false;
            return false;
        }
        return true;
    }

    QQmlProperty prop = QQmlComponentPrivate::removePropertyFromRequired(
            base, name, requiredProperties(), engine);
    QQmlPropertyPrivate *privProp = QQmlPropertyPrivate::get(prop);
    const bool isValid = prop.isValid();
    if (!isValid || !privProp->writeValueProperty(value, {})) {
        QQmlError error{};
        error.setUrl(url);
        if (isValid) {
            error.setDescription(QStringLiteral("Could not set initial property %1").arg(name));
        } else {
            error.setDescription(QStringLiteral("Setting initial properties failed: "
                                                "%2 does not have a property called %1")
                                         .arg(name, QQmlMetaType::prettyTypeName(base)));
        }
        state.errors.push_back(error);
        return false;
    }

    return true;

}

/*!
    \internal
*/
QQmlComponent::QQmlComponent(QObject *parent)
    : QObject(*(new QQmlComponentPrivate), parent)
{
}

/*!
    Destruct the QQmlComponent.
*/
QQmlComponent::~QQmlComponent()
{
    Q_D(QQmlComponent);

    if (d->state.completePending) {
        qWarning("QQmlComponent: Component destroyed while completion pending");

        if (isError()) {
            qWarning() << "This may have been caused by one of the following errors:";
            for (const QQmlError &error : qAsConst(d->state.errors))
                qWarning().nospace().noquote() << QLatin1String("    ") << error;
        }

        d->completeCreate();
    }

    if (d->typeData) {
        d->typeData->unregisterCallback(d);
        d->typeData.reset();
    }
}

/*!
    \qmlproperty enumeration Component::status

    This property holds the status of component loading. The status can be one of the
    following:

    \list
    \li \c{Component.Null} - no data is available for the component
    \li \c{Component.Ready} - the component has been loaded, and can be used to create instances.
    \li \c{Component.Loading} - the component is currently being loaded
    \li \c{Component.Error} - an error occurred while loading the component.
        Calling \l errorString() will provide a human-readable description of any errors.
    \endlist
 */

/*!
    \property QQmlComponent::status
    The component's current \l{QQmlComponent::Status} {status}.
 */
QQmlComponent::Status QQmlComponent::status() const
{
    Q_D(const QQmlComponent);

    if (d->typeData)
        return Loading;
    else if (!d->state.errors.isEmpty())
        return Error;
    else if (d->engine && d->compilationUnit)
        return Ready;
    else
        return Null;
}

/*!
    Returns true if status() == QQmlComponent::Null.
*/
bool QQmlComponent::isNull() const
{
    return status() == Null;
}

/*!
    Returns true if status() == QQmlComponent::Ready.
*/
bool QQmlComponent::isReady() const
{
    return status() == Ready;
}

/*!
    Returns true if status() == QQmlComponent::Error.
*/
bool QQmlComponent::isError() const
{
    return status() == Error;
}

/*!
    Returns true if status() == QQmlComponent::Loading.
*/
bool QQmlComponent::isLoading() const
{
    return status() == Loading;
}

/*!
    \qmlproperty real Component::progress
    The progress of loading the component, from 0.0 (nothing loaded)
    to 1.0 (finished).
*/

/*!
    \property QQmlComponent::progress
    The progress of loading the component, from 0.0 (nothing loaded)
    to 1.0 (finished).
*/
qreal QQmlComponent::progress() const
{
    Q_D(const QQmlComponent);
    return d->progress;
}

/*!
    \fn void QQmlComponent::progressChanged(qreal progress)

    Emitted whenever the component's loading progress changes. \a progress will be the
    current progress between 0.0 (nothing loaded) and 1.0 (finished).
*/

/*!
    \fn void QQmlComponent::statusChanged(QQmlComponent::Status status)

    Emitted whenever the component's status changes. \a status will be the
    new status.
*/

/*!
    Create a QQmlComponent with no data and give it the specified
    \a engine and \a parent. Set the data with setData().
*/
QQmlComponent::QQmlComponent(QQmlEngine *engine, QObject *parent)
    : QObject(*(new QQmlComponentPrivate), parent)
{
    Q_D(QQmlComponent);
    d->engine = engine;
    QObject::connect(engine, &QObject::destroyed, this, [d]() {
        d->state.creator.reset();
        d->engine = nullptr;
    });
}

/*!
    Create a QQmlComponent from the given \a url and give it the
    specified \a parent and \a engine.

    \include qqmlcomponent.qdoc url-note

    \sa loadUrl()
*/
QQmlComponent::QQmlComponent(QQmlEngine *engine, const QUrl &url, QObject *parent)
    : QQmlComponent(engine, url, QQmlComponent::PreferSynchronous, parent)
{
}

/*!
    Create a QQmlComponent from the given \a url and give it the
    specified \a parent and \a engine. If \a mode is \l Asynchronous,
    the component will be loaded and compiled asynchronously.

    \include qqmlcomponent.qdoc url-note

    \sa loadUrl()
*/
QQmlComponent::QQmlComponent(QQmlEngine *engine, const QUrl &url, CompilationMode mode,
                             QObject *parent)
    : QQmlComponent(engine, parent)
{
    Q_D(QQmlComponent);
    d->loadUrl(url, mode);
}

/*!
    Create a QQmlComponent from the given \a fileName and give it the specified
    \a parent and \a engine.

    \sa loadUrl()
*/
QQmlComponent::QQmlComponent(QQmlEngine *engine, const QString &fileName,
                             QObject *parent)
    : QQmlComponent(engine, fileName, QQmlComponent::PreferSynchronous, parent)
{
}

/*!
    Create a QQmlComponent from the given \a fileName and give it the specified
    \a parent and \a engine. If \a mode is \l Asynchronous,
    the component will be loaded and compiled asynchronously.

    \sa loadUrl()
*/
QQmlComponent::QQmlComponent(QQmlEngine *engine, const QString &fileName,
                             CompilationMode mode, QObject *parent)
    : QQmlComponent(engine, parent)
{
    Q_D(QQmlComponent);
    const QUrl url = QDir::isAbsolutePath(fileName) ? QUrl::fromLocalFile(fileName) : QUrl(fileName);
    d->loadUrl(url, mode);
}

/*!
    \internal
*/
QQmlComponent::QQmlComponent(QQmlEngine *engine, QV4::ExecutableCompilationUnit *compilationUnit,
                             int start, QObject *parent)
    : QQmlComponent(engine, parent)
{
    Q_D(QQmlComponent);
    d->compilationUnit.reset(compilationUnit);
    d->start = start;
    d->url = compilationUnit->finalUrl();
    d->progress = 1.0;
}

/*!
    Sets the QQmlComponent to use the given QML \a data. If \a url
    is provided, it is used to set the component name and to provide
    a base path for items resolved by this component.
*/
void QQmlComponent::setData(const QByteArray &data, const QUrl &url)
{
    Q_D(QQmlComponent);

    if (!d->engine) {
        // ###Qt6: In Qt 6, it should be impossible for users to create a QQmlComponent without an engine, and we can remove this check
        qWarning("QQmlComponent: Must provide an engine before calling setData");
        return;
    }

    d->clear();

    d->url = url;

    QQmlRefPointer<QQmlTypeData> typeData = QQmlEnginePrivate::get(d->engine)->typeLoader.getType(data, url);

    if (typeData->isCompleteOrError()) {
        d->fromTypeData(typeData);
    } else {
        d->typeData = typeData;
        d->typeData->registerCallback(d);
    }

    d->progress = 1.0;
    emit statusChanged(status());
    emit progressChanged(d->progress);
}

/*!
    Returns the QQmlContext the component was created in. This is only
    valid for components created directly from QML.
*/
QQmlContext *QQmlComponent::creationContext() const
{
    Q_D(const QQmlComponent);
    if (!d->creationContext.isNull())
        return d->creationContext->asQQmlContext();

    return qmlContext(this);
}

/*!
    Returns the QQmlEngine of this component.

    \since 5.12
*/
QQmlEngine *QQmlComponent::engine() const
{
    Q_D(const QQmlComponent);
    return d->engine;
}

/*!
    Load the QQmlComponent from the provided \a url.

    \include qqmlcomponent.qdoc url-note
*/
void QQmlComponent::loadUrl(const QUrl &url)
{
    Q_D(QQmlComponent);
    d->loadUrl(url);
}

/*!
    Load the QQmlComponent from the provided \a url.
    If \a mode is \l Asynchronous, the component will be loaded and compiled asynchronously.

    \include qqmlcomponent.qdoc url-note
*/
void QQmlComponent::loadUrl(const QUrl &url, QQmlComponent::CompilationMode mode)
{
    Q_D(QQmlComponent);
    d->loadUrl(url, mode);
}

void QQmlComponentPrivate::loadUrl(const QUrl &newUrl, QQmlComponent::CompilationMode mode)
{
    Q_Q(QQmlComponent);
    clear();

    if (newUrl.isRelative()) {
        // The new URL is a relative URL like QUrl("main.qml").
        url = engine->baseUrl().resolved(QUrl(newUrl.toString()));
    } else if (engine->baseUrl().isLocalFile() && newUrl.isLocalFile() && !QDir::isAbsolutePath(newUrl.toLocalFile())) {
        // The new URL is a file on disk but it's a relative path; e.g.:
        // QUrl::fromLocalFile("main.qml") or QUrl("file:main.qml")
        // We need to remove the scheme so that it becomes a relative URL with a relative path:
        QUrl fixedUrl(newUrl);
        fixedUrl.setScheme(QString());
        // Then, turn it into an absolute URL with an absolute path by resolving it against the engine's baseUrl().
        // This is a compatibility hack for QTBUG-58837.
        url = engine->baseUrl().resolved(fixedUrl);
    } else {
        url = newUrl;
    }

    if (newUrl.isEmpty()) {
        QQmlError error;
        error.setDescription(QQmlComponent::tr("Invalid empty URL"));
        state.errors << error;
        return;
    }

    if (progress != 0.0) {
        progress = 0.0;
        emit q->progressChanged(progress);
    }

    QQmlTypeLoader::Mode loaderMode = (mode == QQmlComponent::Asynchronous)
            ? QQmlTypeLoader::Asynchronous
            : QQmlTypeLoader::PreferSynchronous;

    QQmlRefPointer<QQmlTypeData> data = QQmlEnginePrivate::get(engine)->typeLoader.getType(url, loaderMode);

    if (data->isCompleteOrError()) {
        fromTypeData(data);
        progress = 1.0;
    } else {
        typeData = data;
        typeData->registerCallback(this);
        progress = data->progress();
    }

    emit q->statusChanged(q->status());
    if (progress != 0.0)
        emit q->progressChanged(progress);
}

/*!
    Returns the list of errors that occurred during the last compile or create
    operation. An empty list is returned if isError() is not set.
*/
QList<QQmlError> QQmlComponent::errors() const
{
    Q_D(const QQmlComponent);
    if (isError())
        return d->state.errors;
    else
        return QList<QQmlError>();
}

/*!
    \qmlmethod string Component::errorString()

    Returns a human-readable description of any error.

    The string includes the file, location, and description of each error.
    If multiple errors are present, they are separated by a newline character.

    If no errors are present, an empty string is returned.
*/

/*!
    \internal
    errorString() is only meant as a way to get the errors from QML side.
*/
QString QQmlComponent::errorString() const
{
    Q_D(const QQmlComponent);
    QString ret;
    if(!isError())
        return ret;
    for (const QQmlError &e : d->state.errors) {
        ret += e.url().toString() + QLatin1Char(':') +
               QString::number(e.line()) + QLatin1Char(' ') +
               e.description() + QLatin1Char('\n');
    }
    return ret;
}

/*!
    \qmlproperty url Component::url
    The component URL. This is the URL that was used to construct the component.
*/

/*!
    \property QQmlComponent::url
    The component URL. This is the URL passed to either the constructor,
    or the loadUrl(), or setData() methods.
*/
QUrl QQmlComponent::url() const
{
    Q_D(const QQmlComponent);
    return d->url;
}

/*!
    \internal
*/
QQmlComponent::QQmlComponent(QQmlComponentPrivate &dd, QObject *parent)
    : QObject(dd, parent)
{
}

/*!
    Create an object instance from this component, within the specified \a context.
    Returns \nullptr if creation failed.

    If \a context is \nullptr (the default), it will create the instance in the
    \l {QQmlEngine::rootContext()}{root context} of the engine.

    The ownership of the returned object instance is transferred to the caller.

    If the object being created from this component is a visual item, it must
    have a visual parent, which can be set by calling
    QQuickItem::setParentItem(). See \l {Concepts - Visual Parent in Qt Quick}
    for more details.

    \sa QQmlEngine::ObjectOwnership
*/
QObject *QQmlComponent::create(QQmlContext *context)
{
    Q_D(QQmlComponent);

    QObject *rv = d->doBeginCreate(this, context);
    if (rv)
        completeCreate();
    if (rv && !d->requiredProperties().empty()) {
        delete  rv;
        return nullptr;
    }
    return rv;
}

/*!
    Create an object instance of this component, within the specified \a context,
    and initialize its top-level properties with \a initialProperties.

    \omit
    TODO: also mention errorString() when QTBUG-93239 is fixed
    \endomit

    If any of the \c initialProperties cannot be set, \l isError() will return
    \c true, and the \l errors() function can be used to
    get detailed information about the error(s).

    \sa QQmlComponent::create
    \since 5.14
*/
QObject *QQmlComponent::createWithInitialProperties(const QVariantMap& initialProperties, QQmlContext *context)
{
    Q_D(QQmlComponent);

    QObject *rv = d->doBeginCreate(this, context);
    if (rv) {
        setInitialProperties(rv, initialProperties);
        completeCreate();
    }
    if (!d->requiredProperties().empty()) {
        d->requiredProperties().clear();
        return nullptr;
    }
    return rv;
}

/*!
    Create an object instance from this component, within the specified \a context.
    Returns \nullptr if creation failed.

    \note This method provides advanced control over component instance creation.
    In general, programmers should use QQmlComponent::create() to create object
    instances.

    When QQmlComponent constructs an instance, it occurs in three steps:

    \list 1
    \li The object hierarchy is created, and constant values are assigned.
    \li Property bindings are evaluated for the first time.
    \li If applicable, QQmlParserStatus::componentComplete() is called on objects.
    \endlist

    QQmlComponent::beginCreate() differs from QQmlComponent::create() in that it
    only performs step 1. QQmlComponent::completeCreate() must be called to
    complete steps 2 and 3.

    This breaking point is sometimes useful when using attached properties to
    communicate information to an instantiated component, as it allows their
    initial values to be configured before property bindings take effect.

    The ownership of the returned object instance is transferred to the caller.

    \sa completeCreate(), QQmlEngine::ObjectOwnership
*/
QObject *QQmlComponent::beginCreate(QQmlContext *context)
{
    Q_D(QQmlComponent);
    Q_ASSERT(context);
    return d->beginCreate(QQmlContextData::get(context));
}

QObject *QQmlComponentPrivate::beginCreate(QQmlRefPointer<QQmlContextData> context)
{
    Q_Q(QQmlComponent);
    auto cleanup = qScopeGuard([this] {
        if (!state.errors.isEmpty() && lcQmlComponentGeneral().isDebugEnabled()) {
            for (const auto &e : qAsConst(state.errors)) {
                qCDebug(lcQmlComponentGeneral) << "QQmlComponent: " << e.toString();
            }
        }
    });
    if (!context) {
        qWarning("QQmlComponent: Cannot create a component in a null context");
        return nullptr;
    }

    if (!context->isValid()) {
        qWarning("QQmlComponent: Cannot create a component in an invalid context");
        return nullptr;
    }

    if (context->engine() != engine) {
        qWarning("QQmlComponent: Must create component in context from the same QQmlEngine");
        return nullptr;
    }

    if (state.completePending) {
        qWarning("QQmlComponent: Cannot create new component instance before completing the previous");
        return nullptr;
    }

    if (!q->isReady()) {
        qWarning("QQmlComponent: Component is not ready");
        return nullptr;
    }

    // Do not create infinite recursion in object creation
    static const int maxCreationDepth = 10;
    if (creationDepth.localData() >= maxCreationDepth) {
        qWarning("QQmlComponent: Component creation is recursing - aborting");
        return nullptr;
    }

    QQmlEnginePrivate *enginePriv = QQmlEnginePrivate::get(engine);

    enginePriv->inProgressCreations++;
    state.errors.clear();
    state.completePending = true;

    enginePriv->referenceScarceResources();
    QObject *rv = nullptr;
    state.creator.reset(new QQmlObjectCreator(std::move(context), compilationUnit, creationContext));
    rv = state.creator->create(start);
    if (!rv)
        state.errors = state.creator->errors;
    enginePriv->dereferenceScarceResources();

    if (rv) {
        QQmlData *ddata = QQmlData::get(rv);
        Q_ASSERT(ddata);
        // top-level objects should never get JS ownership.
        // if JS ownership is needed this needs to be explicitly undone (like in createObject())
        ddata->indestructible = true;
        ddata->explicitIndestructibleSet = true;
        ddata->rootObjectInCreation = false;
    }

    return rv;
}

void QQmlComponentPrivate::beginDeferred(QQmlEnginePrivate *enginePriv,
                                                 QObject *object, DeferredState *deferredState)
{
    QQmlData *ddata = QQmlData::get(object);
    Q_ASSERT(!ddata->deferredData.isEmpty());

    deferredState->reserve(ddata->deferredData.size());

    for (QQmlData::DeferredData *deferredData : qAsConst(ddata->deferredData)) {
        enginePriv->inProgressCreations++;

        ConstructionState state;
        state.completePending = true;

        state.creator.reset(new QQmlObjectCreator(
                                 deferredData->context->parent(), deferredData->compilationUnit,
                                 QQmlRefPointer<QQmlContextData>()));

        if (!state.creator->populateDeferredProperties(object, deferredData))
            state.errors << state.creator->errors;
        deferredData->bindings.clear();

        deferredState->push_back(std::move(state));
    }
}

void QQmlComponentPrivate::completeDeferred(QQmlEnginePrivate *enginePriv, QQmlComponentPrivate::DeferredState *deferredState)
{
    for (ConstructionState &state : *deferredState)
        complete(enginePriv, &state);
}

void QQmlComponentPrivate::complete(QQmlEnginePrivate *enginePriv, ConstructionState *state)
{
    if (state->completePending) {
        QQmlInstantiationInterrupt interrupt;
        state->creator->finalize(interrupt);

        state->completePending = false;

        enginePriv->inProgressCreations--;

        if (0 == enginePriv->inProgressCreations) {
            while (enginePriv->erroredBindings) {
                enginePriv->warning(enginePriv->erroredBindings->removeError());
            }
        }
    }
}

/*!
    \internal
    Finds the matching top-level property with name \a name of the component \a createdComponent.
    If it was a required property or an alias to a required property contained in \a
    requiredProperties, it is removed from it.

    If wasInRequiredProperties is non-null, the referenced boolean is set to true iff the property
    was found in requiredProperties.

    Returns the QQmlProperty with name \a name (which might be invalid if there is no such property),
    for further processing (for instance, actually setting the property value).

    Note: This method is used in QQmlComponent and QQmlIncubator to manage required properties. Most
    classes which create components should not need it and should only need to call
    setInitialProperties.
 */
QQmlProperty QQmlComponentPrivate::removePropertyFromRequired(
        QObject *createdComponent, const QString &name, RequiredProperties &requiredProperties,
        QQmlEngine *engine, bool *wasInRequiredProperties)
{
    QQmlProperty prop(createdComponent, name, engine);
    auto privProp = QQmlPropertyPrivate::get(prop);
    if (prop.isValid()) {
        // resolve outstanding required properties
        auto targetProp = &privProp->core;
        if (targetProp->isAlias()) {
            auto target = createdComponent;
            QQmlPropertyIndex originalIndex(targetProp->coreIndex());
            QQmlPropertyIndex propIndex;
            QQmlPropertyPrivate::findAliasTarget(target, originalIndex, &target, &propIndex);
            QQmlData *data = QQmlData::get(target);
            Q_ASSERT(data && data->propertyCache);
            targetProp = data->propertyCache->property(propIndex.coreIndex());
        } else {
            // we need to get the pointer from the property cache instead of directly using
            // targetProp else the lookup will fail
            QQmlData *data = QQmlData::get(createdComponent);
            Q_ASSERT(data && data->propertyCache);
            targetProp = data->propertyCache->property(targetProp->coreIndex());
        }
        auto it = requiredProperties.find(targetProp);
        if (it != requiredProperties.end()) {
            if (wasInRequiredProperties)
                *wasInRequiredProperties = true;
            requiredProperties.erase(it);
        } else {
            if (wasInRequiredProperties)
                *wasInRequiredProperties = false;
        }
    }
    return prop;
}

/*!
    This method provides advanced control over component instance creation.
    In general, programmers should use QQmlComponent::create() to create a
    component.

    This function completes the component creation begun with QQmlComponent::beginCreate()
    and must be called afterwards.

    \sa beginCreate()
*/
void QQmlComponent::completeCreate()
{
    Q_D(QQmlComponent);

    d->completeCreate();
}

void QQmlComponentPrivate::completeCreate()
{
    const RequiredProperties& unsetRequiredProperties = requiredProperties();
    for (const auto& unsetRequiredProperty: unsetRequiredProperties) {
        QQmlError error = unsetRequiredPropertyToQQmlError(unsetRequiredProperty);
        state.errors.push_back(error);
    }
    if (state.completePending) {
        ++creationDepth.localData();
        QQmlEnginePrivate *ep = QQmlEnginePrivate::get(engine);
        complete(ep, &state);
        --creationDepth.localData();
    }
}

QQmlComponentAttached::QQmlComponentAttached(QObject *parent)
: QObject(parent), m_prev(nullptr), m_next(nullptr)
{
}

QQmlComponentAttached::~QQmlComponentAttached()
{
    if (m_prev) *m_prev = m_next;
    if (m_next) m_next->m_prev = m_prev;
    m_prev = nullptr;
    m_next = nullptr;
}

/*!
    \internal
*/
QQmlComponentAttached *QQmlComponent::qmlAttachedProperties(QObject *obj)
{
    QQmlComponentAttached *a = new QQmlComponentAttached(obj);

    QQmlEngine *engine = qmlEngine(obj);
    if (!engine)
        return a;

    QQmlEnginePrivate *p = QQmlEnginePrivate::get(engine);
    if (p->activeObjectCreator) { // XXX should only be allowed during begin
        a->insertIntoList(p->activeObjectCreator->componentAttachment());
    } else {
        QQmlData *d = QQmlData::get(obj);
        Q_ASSERT(d);
        Q_ASSERT(d->context);
        d->context->addComponentAttached(a);
    }

    return a;
}

/*!
    Create an object instance from this component using the provided
    \a incubator. \a context specifies the context within which to create the object
    instance.

    If \a context is \nullptr (by default), it will create the instance in the
    engine's \l {QQmlEngine::rootContext()}{root context}.

    \a forContext specifies a context that this object creation depends upon.
    If the \a forContext is being created asynchronously, and the
    \l QQmlIncubator::IncubationMode is \l QQmlIncubator::AsynchronousIfNested,
    this object will also be created asynchronously.
    If \a forContext is \nullptr (by default), the \a context will be used for this decision.

    The created object and its creation status are available via the
    \a incubator.

    \sa QQmlIncubator
*/

void QQmlComponent::create(QQmlIncubator &incubator, QQmlContext *context, QQmlContext *forContext)
{
    Q_D(QQmlComponent);

    if (!context)
        context = d->engine->rootContext();

    QQmlRefPointer<QQmlContextData> contextData = QQmlContextData::get(context);
    QQmlRefPointer<QQmlContextData> forContextData =
            forContext ?  QQmlContextData::get(forContext) : contextData;

    if (!contextData->isValid()) {
        qWarning("QQmlComponent: Cannot create a component in an invalid context");
        return;
    }

    if (contextData->engine() != d->engine) {
        qWarning("QQmlComponent: Must create component in context from the same QQmlEngine");
        return;
    }

    if (!isReady()) {
        qWarning("QQmlComponent: Component is not ready");
        return;
    }

    incubator.clear();
    QExplicitlySharedDataPointer<QQmlIncubatorPrivate> p(incubator.d);

    QQmlEnginePrivate *enginePriv = QQmlEnginePrivate::get(d->engine);

    p->compilationUnit = d->compilationUnit;
    p->enginePriv = enginePriv;
    p->creator.reset(new QQmlObjectCreator(contextData, d->compilationUnit, d->creationContext, p.data()));
    p->subComponentToCreate = d->start;

    enginePriv->incubate(incubator, forContextData);
}

/*!
   Set top-level \a properties of the \a component.

   This method provides advanced control over component instance creation.
   In general, programmers should use
   \l QQmlComponent::createWithInitialProperties to create a component.

   Use this method after beginCreate and before completeCreate has been called.
   If a provided property does not exist, a warning is issued.

   \since 5.14
*/
void QQmlComponent::setInitialProperties(QObject *component, const QVariantMap &properties)
{
    Q_D(QQmlComponent);
    for (auto it = properties.constBegin(); it != properties.constEnd(); ++it)
        d->setInitialProperty(component, it.key(), it.value());
}

/*
    This is essentially a copy of QQmlComponent::create(); except it takes the QQmlContextData
    arguments instead of QQmlContext which means we don't have to construct the rather weighty
    wrapper class for every delegate item.

    This is used by QQmlDelegateModel.
*/
void QQmlComponentPrivate::incubateObject(
        QQmlIncubator *incubationTask,
        QQmlComponent *component,
        QQmlEngine *engine,
        const QQmlRefPointer<QQmlContextData> &context,
        const QQmlRefPointer<QQmlContextData> &forContext)
{
    QQmlIncubatorPrivate *incubatorPriv = QQmlIncubatorPrivate::get(incubationTask);
    QQmlEnginePrivate *enginePriv = QQmlEnginePrivate::get(engine);
    QQmlComponentPrivate *componentPriv = QQmlComponentPrivate::get(component);

    incubatorPriv->compilationUnit = componentPriv->compilationUnit;
    incubatorPriv->enginePriv = enginePriv;
    incubatorPriv->creator.reset(new QQmlObjectCreator(context, componentPriv->compilationUnit, componentPriv->creationContext));
    incubatorPriv->subComponentToCreate = componentPriv->start;

    enginePriv->incubate(*incubationTask, forContext);
}



class QQmlComponentIncubator;

namespace QV4 {

namespace Heap {

#define QmlIncubatorObjectMembers(class, Member) \
    Member(class, HeapValue, HeapValue, valuemap) \
    Member(class, HeapValue, HeapValue, statusChanged) \
    Member(class, Pointer, QmlContext *, qmlContext) \
    Member(class, NoMark, QQmlComponentIncubator *, incubator) \
    Member(class, NoMark, QV4QPointer<QObject>, parent)

DECLARE_HEAP_OBJECT(QmlIncubatorObject, Object) {
    DECLARE_MARKOBJECTS(QmlIncubatorObject);

    void init(QQmlIncubator::IncubationMode = QQmlIncubator::Asynchronous);
    inline void destroy();
};

}

struct QmlIncubatorObject : public QV4::Object
{
    V4_OBJECT2(QmlIncubatorObject, Object)
    V4_NEEDS_DESTROY

    static ReturnedValue method_get_statusChanged(const FunctionObject *, const Value *thisObject, const Value *argv, int argc);
    static ReturnedValue method_set_statusChanged(const FunctionObject *, const Value *thisObject, const Value *argv, int argc);
    static ReturnedValue method_get_status(const FunctionObject *, const Value *thisObject, const Value *argv, int argc);
    static ReturnedValue method_get_object(const FunctionObject *, const Value *thisObject, const Value *argv, int argc);
    static ReturnedValue method_forceCompletion(const FunctionObject *, const Value *thisObject, const Value *argv, int argc);

    void statusChanged(QQmlIncubator::Status);
    void setInitialState(QObject *, RequiredProperties &requiredProperties);
};

}

DEFINE_OBJECT_VTABLE(QV4::QmlIncubatorObject);

class QQmlComponentIncubator : public QQmlIncubator
{
public:
    QQmlComponentIncubator(QV4::Heap::QmlIncubatorObject *inc, IncubationMode mode)
        : QQmlIncubator(mode)
    {
        incubatorObject.set(inc->internalClass->engine, inc);
    }

    void statusChanged(Status s) override {
        QV4::Scope scope(incubatorObject.engine());
        QV4::Scoped<QV4::QmlIncubatorObject> i(scope, incubatorObject.as<QV4::QmlIncubatorObject>());
        i->statusChanged(s);
    }

    void setInitialState(QObject *o) override {
        QV4::Scope scope(incubatorObject.engine());
        QV4::Scoped<QV4::QmlIncubatorObject> i(scope, incubatorObject.as<QV4::QmlIncubatorObject>());
        auto d = QQmlIncubatorPrivate::get(this);
        i->setInitialState(o, d->requiredProperties());
    }

    QV4::PersistentValue incubatorObject; // keep a strong internal reference while incubating
};


static void QQmlComponent_setQmlParent(QObject *me, QObject *parent)
{
    if (parent) {
        me->setParent(parent);
        typedef QQmlPrivate::AutoParentFunction APF;
        QList<APF> functions = QQmlMetaType::parentFunctions();

        bool needParent = false;
        for (int ii = 0; ii < functions.count(); ++ii) {
            QQmlPrivate::AutoParentResult res = functions.at(ii)(me, parent);
            if (res == QQmlPrivate::Parented) {
                needParent = false;
                break;
            } else if (res == QQmlPrivate::IncompatibleParent) {
                needParent = true;
            }
        }
        if (needParent)
            qWarning("QQmlComponent: Created graphical object was not "
                     "placed in the graphics scene.");
    }
}

/*!
    \qmlmethod QtObject Component::createObject(QtObject parent, object properties)

    Creates and returns an object instance of this component that will have
    the given \a parent and \a properties. The \a properties argument is optional.
    Returns null if object creation fails.

    The object will be created in the same context as the one in which the component
    was created. This function will always return null when called on components
    which were not created in QML.

    If you wish to create an object without setting a parent, specify \c null for
    the \a parent value. Note that if the returned object is to be displayed, you
    must provide a valid \a parent value or set the returned object's \l{Item::parent}{parent}
    property, otherwise the object will not be visible.

    If a \a parent is not provided to createObject(), a reference to the returned object must be held so that
    it is not destroyed by the garbage collector. This is true regardless of whether \l{Item::parent} is set afterwards,
    because setting the Item parent does not change object ownership. Only the graphical parent is changed.

    As of \c {QtQuick 1.1}, this method accepts an optional \a properties argument that specifies a
    map of initial property values for the created object. These values are applied before the object
    creation is finalized. This is more efficient than setting property values after object creation,
    particularly where large sets of property values are defined, and also allows property bindings
    to be set up (using \l{Qt::binding}{Qt.binding}) before the object is created.

    The \a properties argument is specified as a map of property-value items. For example, the code
    below creates an object with initial \c x and \c y values of 100 and 100, respectively:

    \js
        const component = Qt.createComponent("Button.qml");
        if (component.status === Component.Ready) {
            component.createObject(parent, { x: 100, y: 100 });
        }
    \endjs

    Dynamically created instances can be deleted with the \c destroy() method.
    See \l {Dynamic QML Object Creation from JavaScript} for more information.

    \sa incubateObject()
*/


void QQmlComponentPrivate::setInitialProperties(QV4::ExecutionEngine *engine, QV4::QmlContext *qmlContext, const QV4::Value &o, const QV4::Value &v, RequiredProperties &requiredProperties, QObject *createdComponent)
{
    QV4::Scope scope(engine);
    QV4::ScopedObject object(scope);
    QV4::ScopedObject valueMap(scope, v);
    QV4::ObjectIterator it(scope, valueMap, QV4::ObjectIterator::EnumerableOnly);
    QV4::ScopedString name(scope);
    QV4::ScopedValue val(scope);
    if (engine->hasException)
        return;

    // js modules (mjs) have no qmlContext
    QV4::ScopedStackFrame frame(scope, qmlContext ? qmlContext : engine->scriptContext());

    while (1) {
        name = it.nextPropertyNameAsString(val);
        if (!name)
            break;
        object = o;
        const QStringList properties = name->toQString().split(QLatin1Char('.'));
        bool isTopLevelProperty = properties.size() == 1;
        for (int i = 0; i < properties.length() - 1; ++i) {
            name = engine->newString(properties.at(i));
            object = object->get(name);
            if (engine->hasException || !object) {
                break;
            }
        }
        if (engine->hasException || !object) {
            qmlWarning(createdComponent, engine->catchExceptionAsQmlError());
            continue;
        }
        name = engine->newString(properties.last());
        object->put(name, val);
        if (engine->hasException) {
            qmlWarning(createdComponent, engine->catchExceptionAsQmlError());
            continue;
        } else if (isTopLevelProperty) {
            auto prop = removePropertyFromRequired(createdComponent, name->toQString(),
                                                   requiredProperties, engine->qmlEngine());
        }
    }

    engine->hasException = false;
}

QQmlError QQmlComponentPrivate::unsetRequiredPropertyToQQmlError(const RequiredPropertyInfo &unsetRequiredProperty)
{
    QQmlError error;
    QString description = QLatin1String("Required property %1 was not initialized").arg(unsetRequiredProperty.propertyName);
    switch (unsetRequiredProperty.aliasesToRequired.size()) {
    case 0:
        break;
    case 1: {
        const auto info = unsetRequiredProperty.aliasesToRequired.first();
        description += QLatin1String("\nIt can be set via the alias property %1 from %2\n").arg(info.propertyName, info.fileUrl.toString());
        break;
    }
    default:
        description += QLatin1String("\nIt can be set via one of the following alias properties:");
        for (auto aliasInfo: unsetRequiredProperty.aliasesToRequired) {
            description += QLatin1String("\n- %1 (%2)").arg(aliasInfo.propertyName, aliasInfo.fileUrl.toString());
        }
        description += QLatin1Char('\n');
    }
    error.setDescription(description);
    error.setUrl(unsetRequiredProperty.fileUrl);
    error.setLine(qmlConvertSourceCoordinate<quint32, int>(unsetRequiredProperty.location.line));
    error.setColumn(qmlConvertSourceCoordinate<quint32, int>(unsetRequiredProperty.location.column));
    return  error;
}

#if QT_DEPRECATED_SINCE(6, 3)
/*!
    \internal
*/
void QQmlComponent::createObject(QQmlV4Function *args)
{
    Q_D(QQmlComponent);
    Q_ASSERT(d->engine);
    Q_ASSERT(args);

    qmlWarning(this) << "Unsuitable arguments passed to createObject(). The first argument should "
                        "be a QObject* or null, and the second argument should be a JavaScript "
                        "object or a QVariantMap";

    QObject *parent = nullptr;
    QV4::ExecutionEngine *v4 = args->v4engine();
    QV4::Scope scope(v4);
    QV4::ScopedValue valuemap(scope, QV4::Value::undefinedValue());

    if (args->length() >= 1) {
        QV4::Scoped<QV4::QObjectWrapper> qobjectWrapper(scope, (*args)[0]);
        if (qobjectWrapper)
            parent = qobjectWrapper->object();
    }

    if (args->length() >= 2) {
        QV4::ScopedValue v(scope, (*args)[1]);
        if (!v->as<QV4::Object>() || v->as<QV4::ArrayObject>()) {
            qmlWarning(this) << tr("createObject: value is not an object");
            args->setReturnValue(QV4::Encode::null());
            return;
        }
        valuemap = v;
    }

    QQmlContext *ctxt = creationContext();
    if (!ctxt) ctxt = d->engine->rootContext();

    QObject *rv = beginCreate(ctxt);

    if (!rv) {
        args->setReturnValue(QV4::Encode::null());
        return;
    }

    QQmlComponent_setQmlParent(rv, parent);

    QV4::ScopedValue object(scope, QV4::QObjectWrapper::wrap(v4, rv));
    Q_ASSERT(object->isObject());

    if (!valuemap->isUndefined()) {
        QV4::Scoped<QV4::QmlContext> qmlContext(scope, v4->qmlContext());
        QQmlComponentPrivate::setInitialProperties(v4, qmlContext, object, valuemap, d->requiredProperties(), rv);
    }
    if (!d->requiredProperties().empty()) {
        QList<QQmlError> errors;
        for (const auto &requiredProperty: d->requiredProperties()) {
            errors.push_back(QQmlComponentPrivate::unsetRequiredPropertyToQQmlError(requiredProperty));
        }
        qmlWarning(rv, errors);
        args->setReturnValue(QV4::Encode::null());
        delete rv;
        return;
    }

    d->completeCreate();

    Q_ASSERT(QQmlData::get(rv));
    QQmlData::get(rv)->explicitIndestructibleSet = false;
    QQmlData::get(rv)->indestructible = false;

    args->setReturnValue(object->asReturnedValue());
}
#endif

/*!
    \internal
 */
QObject *QQmlComponent::createObject(QObject *parent, const QVariantMap &properties)
{
    Q_D(QQmlComponent);
    Q_ASSERT(d->engine);

    QQmlContext *ctxt = creationContext();
    if (!ctxt)
        ctxt = d->engine->rootContext();

    QObject *rv = beginCreate(ctxt);
    if (!rv)
        return nullptr;

    Q_ASSERT(d->state.errors.isEmpty()); // otherwise beginCreate() would return nullptr

    QQmlComponent_setQmlParent(rv, parent);

    if (!properties.isEmpty()) {
        setInitialProperties(rv, properties);
        if (!d->state.errors.isEmpty()) {
            qmlWarning(rv, d->state.errors);
            d->state.errors.clear();
        }
    }

    if (!d->requiredProperties().empty()) {
        QList<QQmlError> errors;
        for (const auto &requiredProperty: qAsConst(d->requiredProperties())) {
            errors.push_back(QQmlComponentPrivate::unsetRequiredPropertyToQQmlError(
                    requiredProperty));
        }
        if (!errors.isEmpty())
            qmlWarning(rv, errors);
        delete rv;
        return nullptr;
    }

    d->completeCreate();

    QQmlData *qmlData = QQmlData::get(rv);
    Q_ASSERT(qmlData);
    qmlData->explicitIndestructibleSet = false;
    qmlData->indestructible = false;

    return rv;
}

/*!
    \qmlmethod object Component::incubateObject(QtObject parent, object properties, enumeration mode)

    Creates an incubator for an instance of this component. Incubators allow new component
    instances to be instantiated asynchronously and do not cause freezes in the UI.

    The \a parent argument specifies the parent the created instance will have. Omitting the
    parameter or passing null will create an object with no parent. In this case, a reference
    to the created object must be held so that it is not destroyed by the garbage collector.

    The \a properties argument is specified as a map of property-value items which will be
    set on the created object during its construction. \a mode may be Qt.Synchronous or
    Qt.Asynchronous, and controls whether the instance is created synchronously or asynchronously.
    The default is asynchronous. In some circumstances, even if Qt.Synchronous is specified,
    the incubator may create the object asynchronously. This happens if the component calling
    incubateObject() is itself being created asynchronously.

    All three arguments are optional.

    If successful, the method returns an incubator, otherwise null. The incubator has the following
    properties:

    \list
    \li \c status - The status of the incubator. Valid values are Component.Ready, Component.Loading and
       Component.Error.
    \li \c object - The created object instance. Will only be available once the incubator is in the
       Ready status.
    \li \c onStatusChanged - Specifies a callback function to be invoked when the status changes. The
       status is passed as a parameter to the callback.
    \li \c{forceCompletion()} - Call to complete incubation synchronously.
    \endlist

    The following example demonstrates how to use an incubator:

    \js
        const component = Qt.createComponent("Button.qml");

        const incubator = component.incubateObject(parent, { x: 10, y: 10 });
        if (incubator.status !== Component.Ready) {
            incubator.onStatusChanged = function(status) {
                if (status === Component.Ready) {
                    print("Object", incubator.object, "is now ready!");
                }
            };
        } else {
            print("Object", incubator.object, "is ready immediately!");
        }
    \endjs

    Dynamically created instances can be deleted with the \c destroy() method.
    See \l {Dynamic QML Object Creation from JavaScript} for more information.

    \sa createObject()
*/

/*!
    \internal
*/
void QQmlComponent::incubateObject(QQmlV4Function *args)
{
    Q_D(QQmlComponent);
    Q_ASSERT(d->engine);
    Q_UNUSED(d);
    Q_ASSERT(args);
    QV4::ExecutionEngine *v4 = args->v4engine();
    QV4::Scope scope(v4);

    QObject *parent = nullptr;
    QV4::ScopedValue valuemap(scope, QV4::Value::undefinedValue());
    QQmlIncubator::IncubationMode mode = QQmlIncubator::Asynchronous;

    if (args->length() >= 1) {
        QV4::Scoped<QV4::QObjectWrapper> qobjectWrapper(scope, (*args)[0]);
        if (qobjectWrapper)
            parent = qobjectWrapper->object();
    }

    if (args->length() >= 2) {
        QV4::ScopedValue v(scope, (*args)[1]);
        if (v->isNull()) {
        } else if (!v->as<QV4::Object>() || v->as<QV4::ArrayObject>()) {
            qmlWarning(this) << tr("createObject: value is not an object");
            args->setReturnValue(QV4::Encode::null());
            return;
        } else {
            valuemap = v;
        }
    }

    if (args->length() >= 3) {
        QV4::ScopedValue val(scope, (*args)[2]);
        quint32 v = val->toUInt32();
        if (v == 0)
            mode = QQmlIncubator::Asynchronous;
        else if (v == 1)
            mode = QQmlIncubator::AsynchronousIfNested;
    }

    QQmlComponentExtension *e = componentExtension(args->v4engine());

    QV4::Scoped<QV4::QmlIncubatorObject> r(scope, v4->memoryManager->allocate<QV4::QmlIncubatorObject>(mode));
    QV4::ScopedObject p(scope, e->incubationProto.value());
    r->setPrototypeOf(p);

    if (!valuemap->isUndefined())
        r->d()->valuemap.set(scope.engine, valuemap);
    r->d()->qmlContext.set(scope.engine, v4->qmlContext());
    r->d()->parent = parent;

    QQmlIncubator *incubator = r->d()->incubator;
    create(*incubator, creationContext());

    if (incubator->status() == QQmlIncubator::Null) {
        args->setReturnValue(QV4::Encode::null());
    } else {
        args->setReturnValue(r.asReturnedValue());
    }
}

// XXX used by QSGLoader
void QQmlComponentPrivate::initializeObjectWithInitialProperties(QV4::QmlContext *qmlContext, const QV4::Value &valuemap, QObject *toCreate, RequiredProperties &requiredProperties)
{
    QV4::ExecutionEngine *v4engine = engine->handle();
    QV4::Scope scope(v4engine);

    QV4::ScopedValue object(scope, QV4::QObjectWrapper::wrap(v4engine, toCreate));
    Q_ASSERT(object->as<QV4::Object>());

    if (!valuemap.isUndefined())
        setInitialProperties(v4engine, qmlContext, object, valuemap, requiredProperties, toCreate);
}

QQmlComponentExtension::QQmlComponentExtension(QV4::ExecutionEngine *v4)
{
    QV4::Scope scope(v4);
    QV4::ScopedObject proto(scope, v4->newObject());
    proto->defineAccessorProperty(QStringLiteral("onStatusChanged"),
                                  QV4::QmlIncubatorObject::method_get_statusChanged, QV4::QmlIncubatorObject::method_set_statusChanged);
    proto->defineAccessorProperty(QStringLiteral("status"), QV4::QmlIncubatorObject::method_get_status, nullptr);
    proto->defineAccessorProperty(QStringLiteral("object"), QV4::QmlIncubatorObject::method_get_object, nullptr);
    proto->defineDefaultProperty(QStringLiteral("forceCompletion"), QV4::QmlIncubatorObject::method_forceCompletion);

    incubationProto.set(v4, proto);
}

QV4::ReturnedValue QV4::QmlIncubatorObject::method_get_object(const FunctionObject *b, const Value *thisObject, const Value *, int)
{
    QV4::Scope scope(b);
    QV4::Scoped<QmlIncubatorObject> o(scope, thisObject->as<QmlIncubatorObject>());
    if (!o)
        THROW_TYPE_ERROR();

    return QV4::QObjectWrapper::wrap(scope.engine, o->d()->incubator->object());
}

QV4::ReturnedValue QV4::QmlIncubatorObject::method_forceCompletion(const FunctionObject *b, const Value *thisObject, const Value *, int)
{
    QV4::Scope scope(b);
    QV4::Scoped<QmlIncubatorObject> o(scope, thisObject->as<QmlIncubatorObject>());
    if (!o)
        THROW_TYPE_ERROR();

    o->d()->incubator->forceCompletion();

    RETURN_UNDEFINED();
}

QV4::ReturnedValue QV4::QmlIncubatorObject::method_get_status(const FunctionObject *b, const Value *thisObject, const Value *, int)
{
    QV4::Scope scope(b);
    QV4::Scoped<QmlIncubatorObject> o(scope, thisObject->as<QmlIncubatorObject>());
    if (!o)
        THROW_TYPE_ERROR();

    return QV4::Encode(o->d()->incubator->status());
}

QV4::ReturnedValue QV4::QmlIncubatorObject::method_get_statusChanged(const FunctionObject *b, const Value *thisObject, const Value *, int)
{
    QV4::Scope scope(b);
    QV4::Scoped<QmlIncubatorObject> o(scope, thisObject->as<QmlIncubatorObject>());
    if (!o)
        THROW_TYPE_ERROR();

    return QV4::Encode(o->d()->statusChanged);
}

QV4::ReturnedValue QV4::QmlIncubatorObject::method_set_statusChanged(const FunctionObject *b, const Value *thisObject, const Value *argv, int argc)
{
    QV4::Scope scope(b);
    QV4::Scoped<QmlIncubatorObject> o(scope, thisObject->as<QmlIncubatorObject>());
    if (!o || argc < 1)
        THROW_TYPE_ERROR();

    o->d()->statusChanged.set(scope.engine, argv[0]);

    RETURN_UNDEFINED();
}

QQmlComponentExtension::~QQmlComponentExtension()
{
}

void QV4::Heap::QmlIncubatorObject::init(QQmlIncubator::IncubationMode m)
{
    Object::init();
    valuemap.set(internalClass->engine, QV4::Value::undefinedValue());
    statusChanged.set(internalClass->engine, QV4::Value::undefinedValue());
    parent.init();
    qmlContext.set(internalClass->engine, nullptr);
    incubator = new QQmlComponentIncubator(this, m);
}

void QV4::Heap::QmlIncubatorObject::destroy() {
    delete incubator;
    parent.destroy();
    Object::destroy();
}

void QV4::QmlIncubatorObject::setInitialState(QObject *o, RequiredProperties &requiredProperties)
{
    QQmlComponent_setQmlParent(o, d()->parent);

    if (!d()->valuemap.isUndefined()) {
        QV4::ExecutionEngine *v4 = engine();
        QV4::Scope scope(v4);
        QV4::ScopedObject obj(scope, QV4::QObjectWrapper::wrap(v4, o));
        QV4::Scoped<QV4::QmlContext> qmlCtxt(scope, d()->qmlContext);
        QQmlComponentPrivate::setInitialProperties(v4, qmlCtxt, obj, d()->valuemap, requiredProperties, o);
    }
}

void QV4::QmlIncubatorObject::statusChanged(QQmlIncubator::Status s)
{
    QV4::Scope scope(engine());
    // hold the incubated object in a scoped value to prevent it's destruction before this method returns
    QV4::ScopedObject incubatedObject(scope, QV4::QObjectWrapper::wrap(scope.engine, d()->incubator->object()));

    if (s == QQmlIncubator::Ready) {
        Q_ASSERT(QQmlData::get(d()->incubator->object()));
        QQmlData::get(d()->incubator->object())->explicitIndestructibleSet = false;
        QQmlData::get(d()->incubator->object())->indestructible = false;
    }

    QV4::ScopedFunctionObject f(scope, d()->statusChanged);
    if (f) {
        QV4::JSCallArguments jsCallData(scope, 1);
        *jsCallData.thisObject = this;
        jsCallData.args[0] = QV4::Value::fromUInt32(s);
        f->call(jsCallData);
        if (scope.hasException()) {
            QQmlError error = scope.engine->catchExceptionAsQmlError();
            QQmlEnginePrivate::warning(QQmlEnginePrivate::get(scope.engine->qmlEngine()), error);
        }
    }

    if (s != QQmlIncubator::Loading)
        d()->incubator->incubatorObject.clear();
}

#undef INITIALPROPERTIES_SOURCE

QT_END_NAMESPACE

#include "moc_qqmlcomponent.cpp"
