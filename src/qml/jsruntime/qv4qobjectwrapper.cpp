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

#include "qv4qobjectwrapper_p.h"

#include <private/qqmlobjectorgadget_p.h>
#include <private/qqmlengine_p.h>
#include <private/qqmlvmemetaobject_p.h>
#include <private/qqmlbinding_p.h>
#include <private/qjsvalue_p.h>
#include <private/qqmlexpression_p.h>
#include <private/qqmlglobal_p.h>
#include <private/qqmltypewrapper_p.h>
#include <private/qqmlvaluetypewrapper_p.h>
#include <private/qqmllistwrapper_p.h>
#include <private/qqmlbuiltinfunctions_p.h>

#include <private/qv4arraybuffer_p.h>
#include <private/qv4functionobject_p.h>
#include <private/qv4runtime_p.h>
#include <private/qv4variantobject_p.h>
#include <private/qv4identifiertable_p.h>
#include <private/qv4lookup_p.h>
#include <private/qv4qmlcontext_p.h>
#include <private/qv4sequenceobject_p.h>
#include <private/qv4objectproto_p.h>
#include <private/qv4jsonobject_p.h>
#include <private/qv4regexpobject_p.h>
#include <private/qv4dateobject_p.h>
#include <private/qv4scopedvalue_p.h>
#include <private/qv4jscall_p.h>
#include <private/qv4mm_p.h>
#include <private/qqmlscriptstring_p.h>
#include <private/qv4compileddata_p.h>
#include <private/qqmlpropertybinding_p.h>

#include <QtQml/qjsvalue.h>
#include <QtCore/qjsonarray.h>
#include <QtCore/qjsonobject.h>
#include <QtCore/qjsonvalue.h>
#include <QtCore/qvarlengtharray.h>
#include <QtCore/qtimer.h>
#include <QtCore/qatomic.h>
#include <QtCore/qmetaobject.h>
#if QT_CONFIG(qml_itemmodel)
#include <QtCore/qabstractitemmodel.h>
#endif
#include <QtCore/qloggingcategory.h>

#include <vector>
QT_BEGIN_NAMESPACE

Q_LOGGING_CATEGORY(lcBindingRemoval, "qt.qml.binding.removal", QtWarningMsg)
Q_LOGGING_CATEGORY(lcObjectConnect, "qt.qml.object.connect", QtWarningMsg)

// The code in this file does not violate strict aliasing, but GCC thinks it does
// so turn off the warnings for us to have a clean build
QT_WARNING_DISABLE_GCC("-Wstrict-aliasing")

namespace QV4 {

QPair<QObject *, int> QObjectMethod::extractQtMethod(const FunctionObject *function)
{
    ExecutionEngine *v4 = function->engine();
    if (v4) {
        Scope scope(v4);
        Scoped<QObjectMethod> method(scope, function->as<QObjectMethod>());
        if (method)
            return qMakePair(method->object(), method->methodIndex());
    }

    return qMakePair((QObject *)nullptr, -1);
}

static QPair<QObject *, int> extractQtSignal(const Value &value)
{
    if (value.isObject()) {
        ExecutionEngine *v4 = value.as<Object>()->engine();
        Scope scope(v4);
        ScopedFunctionObject function(scope, value);
        if (function)
            return QObjectMethod::extractQtMethod(function);

        Scoped<QmlSignalHandler> handler(scope, value);
        if (handler)
            return qMakePair(handler->object(), handler->signalIndex());
    }

    return qMakePair((QObject *)nullptr, -1);
}

static ReturnedValue loadProperty(ExecutionEngine *v4, QObject *object,
                                       const QQmlPropertyData &property)
{
    Q_ASSERT(!property.isFunction());
    Scope scope(v4);

    const QMetaType propMetaType = property.propType();
    if (property.isQObject()) {
        QObject *rv = nullptr;
        property.readProperty(object, &rv);
        ReturnedValue ret = QObjectWrapper::wrap(v4, rv);
        if (propMetaType.flags().testFlag(QMetaType::IsConst)) {
            ScopedValue v(scope, ret);
            if (auto obj = v->as<Object>()) {
                obj->setInternalClass(obj->internalClass()->cryopreserved());
                return obj->asReturnedValue();
            }
        }
        return ret;
    }

    if (property.isQList() && propMetaType.flags().testFlag(QMetaType::IsQmlList))
        return QmlListWrapper::create(v4, object, property.coreIndex(), propMetaType);

    switch (property.isEnum() ? QMetaType::Int : propMetaType.id()) {
    case QMetaType::Int: {
        int v = 0;
        property.readProperty(object, &v);
        return Encode(v);
    }
    case QMetaType::Bool: {
        bool v = false;
        property.readProperty(object, &v);
        return Encode(v);
    }
    case QMetaType::QString: {
        QString v;
        property.readProperty(object, &v);
        return v4->newString(v)->asReturnedValue();
    }
    case QMetaType::UInt: {
        uint v = 0;
        property.readProperty(object, &v);
        return Encode(v);
    }
    case QMetaType::Float: {
        float v = 0;
        property.readProperty(object, &v);
        return Encode(v);
    }
    case QMetaType::Double: {
        double v = 0;
        property.readProperty(object, &v);
        return Encode(v);
    }
    default:
        break;
    }

    if (propMetaType == QMetaType::fromType<QJSValue>()) {
        QJSValue v;
        property.readProperty(object, &v);
        return QJSValuePrivate::convertToReturnedValue(v4, v);
    }

    if (property.isQVariant()) {
        QVariant v;
        property.readProperty(object, &v);

        if (QQmlMetaType::isValueType(v.metaType())) {
            if (const QMetaObject *valueTypeMetaObject = QQmlMetaType::metaObjectForValueType(v.metaType()))
                return QQmlValueTypeWrapper::create(v4, object, property.coreIndex(), valueTypeMetaObject, v.metaType()); // VariantReference value-type.
        }

        return scope.engine->fromVariant(v);
    }

    if (QQmlMetaType::isValueType(propMetaType)) {
        if (const QMetaObject *valueTypeMetaObject = QQmlMetaType::metaObjectForValueType(propMetaType))
            return QQmlValueTypeWrapper::create(v4, object, property.coreIndex(), valueTypeMetaObject, propMetaType);
    }

    // see if it's a sequence type
    bool succeeded = false;
    QV4::ScopedValue retn(scope, QV4::SequencePrototype::newSequence(
                              v4, propMetaType, object, property.coreIndex(),
                              !property.isWritable(), &succeeded));
    if (succeeded)
        return retn->asReturnedValue();

    if (!propMetaType.isValid()) {
        QMetaProperty p = object->metaObject()->property(property.coreIndex());
        qWarning("QMetaProperty::read: Unable to handle unregistered datatype '%s' for property "
                 "'%s::%s'", p.typeName(), object->metaObject()->className(), p.name());
        return Encode::undefined();
    } else {
        QVariant v(propMetaType);
        property.readProperty(object, v.data());
        return scope.engine->fromVariant(v);
    }
}

void QObjectWrapper::initializeBindings(ExecutionEngine *engine)
{
    engine->functionPrototype()->defineDefaultProperty(QStringLiteral("connect"), method_connect);
    engine->functionPrototype()->defineDefaultProperty(QStringLiteral("disconnect"), method_disconnect);
}

QQmlPropertyData *QObjectWrapper::findProperty(
        const QQmlRefPointer<QQmlContextData> &qmlContext, String *name,
        RevisionMode revisionMode, QQmlPropertyData *local) const
{
    return findProperty(d()->object(), qmlContext, name, revisionMode, local);
}

QQmlPropertyData *QObjectWrapper::findProperty(
        QObject *o, const QQmlRefPointer<QQmlContextData> &qmlContext,
        String *name, RevisionMode revisionMode, QQmlPropertyData *local)
{
    Q_UNUSED(revisionMode);

    QQmlData *ddata = QQmlData::get(o, false);
    QQmlPropertyData *result = nullptr;
    if (ddata && ddata->propertyCache)
        result = ddata->propertyCache->property(name, o, qmlContext);
    else
        result = QQmlPropertyCache::property(o, name, qmlContext, local);
    return result;
}

ReturnedValue QObjectWrapper::getProperty(ExecutionEngine *engine, QObject *object, QQmlPropertyData *property)
{
    QQmlData::flushPendingBinding(object, property->coreIndex());

    if (property->isFunction() && !property->isVarProperty()) {
        if (property->isVMEFunction()) {
            QQmlVMEMetaObject *vmemo = QQmlVMEMetaObject::get(object);
            Q_ASSERT(vmemo);
            return vmemo->vmeMethod(property->coreIndex());
        } else if (property->isV4Function()) {
            Scope scope(engine);
            ScopedContext global(scope, engine->qmlContext());
            if (!global)
                global = engine->rootContext();
            return QObjectMethod::create(global, object, property->coreIndex());
        } else if (property->isSignalHandler()) {
            QmlSignalHandler::initProto(engine);
            return engine->memoryManager->allocate<QmlSignalHandler>(object, property->coreIndex())->asReturnedValue();
        } else {
            ExecutionContext *global = engine->rootContext();
            return QObjectMethod::create(global, object, property->coreIndex());
        }
    }

    QQmlEnginePrivate *ep = engine->qmlEngine() ? QQmlEnginePrivate::get(engine->qmlEngine()) : nullptr;

    if (ep && ep->propertyCapture && !property->isConstant())
        if (!property->isBindable() || ep->propertyCapture->expression->mustCaptureBindableProperty())
            ep->propertyCapture->captureProperty(object, property->coreIndex(), property->notifyIndex());

    if (property->isVarProperty()) {
        QQmlVMEMetaObject *vmemo = QQmlVMEMetaObject::get(object);
        Q_ASSERT(vmemo);
        return vmemo->vmeProperty(property->coreIndex());
    } else {
        return loadProperty(engine, object, *property);
    }
}

static OptionalReturnedValue getDestroyOrToStringMethod(ExecutionEngine *v4, String *name, QObject *qobj, bool *hasProperty = nullptr)
{
    int index = 0;
    if (name->equals(v4->id_destroy()))
        index = QObjectMethod::DestroyMethod;
    else if (name->equals(v4->id_toString()))
        index = QObjectMethod::ToStringMethod;
    else
        return OptionalReturnedValue();

    if (hasProperty)
        *hasProperty = true;
    ExecutionContext *global = v4->rootContext();
    return OptionalReturnedValue(QObjectMethod::create(global, qobj, index));
}

static OptionalReturnedValue getPropertyFromImports(
        ExecutionEngine *v4, String *name, const QQmlRefPointer<QQmlContextData> &qmlContext,
        QObject *qobj, bool *hasProperty = nullptr)
{
    if (!qmlContext || !qmlContext->imports())
        return OptionalReturnedValue();

    QQmlTypeNameCache::Result r = qmlContext->imports()->query(name);

    if (hasProperty)
        *hasProperty = true;

    if (!r.isValid())
        return OptionalReturnedValue();

    if (r.scriptIndex != -1) {
        return OptionalReturnedValue(Encode::undefined());
    } else if (r.type.isValid()) {
        return OptionalReturnedValue(QQmlTypeWrapper::create(v4, qobj,r.type, Heap::QQmlTypeWrapper::ExcludeEnums));
    } else if (r.importNamespace) {
        return OptionalReturnedValue(QQmlTypeWrapper::create(
                                         v4, qobj, qmlContext->imports(), r.importNamespace,
                                         Heap::QQmlTypeWrapper::ExcludeEnums));
    }
    Q_UNREACHABLE();
    return OptionalReturnedValue();
}

ReturnedValue QObjectWrapper::getQmlProperty(
        const QQmlRefPointer<QQmlContextData> &qmlContext, String *name,
        QObjectWrapper::RevisionMode revisionMode, bool *hasProperty, bool includeImports) const
{
    // Keep this code in sync with ::virtualResolveLookupGetter

    if (QQmlData::wasDeleted(d()->object())) {
        if (hasProperty)
            *hasProperty = false;
        return Encode::undefined();
    }

    ExecutionEngine *v4 = engine();

    if (auto methodValue = getDestroyOrToStringMethod(v4, name, d()->object(), hasProperty))
        return *methodValue;

    QQmlPropertyData local;
    QQmlPropertyData *result = findProperty(qmlContext, name, revisionMode, &local);

    if (!result) {
        // Check for attached properties
        if (includeImports && name->startsWithUpper()) {
            if (auto importProperty = getPropertyFromImports(v4, name, qmlContext, d()->object(), hasProperty))
                return *importProperty;
        }
        return Object::virtualGet(this, name->propertyKey(), this, hasProperty);
    }

    QQmlData *ddata = QQmlData::get(d()->object(), false);

    if (revisionMode == QObjectWrapper::CheckRevision && result->hasRevision()) {
        if (ddata && ddata->propertyCache && !ddata->propertyCache->isAllowedInRevision(result)) {
            if (hasProperty)
                *hasProperty = false;
            return Encode::undefined();
        }
    }

    if (hasProperty)
        *hasProperty = true;

    return getProperty(v4, d()->object(), result);
}

ReturnedValue QObjectWrapper::getQmlProperty(
        ExecutionEngine *engine, const QQmlRefPointer<QQmlContextData> &qmlContext,
        QObject *object, String *name, QObjectWrapper::RevisionMode revisionMode, bool *hasProperty,
        QQmlPropertyData **property)
{
    if (QQmlData::wasDeleted(object)) {
        if (hasProperty)
            *hasProperty = false;
        return Encode::null();
    }

    if (auto methodValue = getDestroyOrToStringMethod(engine, name, object, hasProperty))
        return *methodValue;

    QQmlData *ddata = QQmlData::get(object, false);
    QQmlPropertyData local;
    QQmlPropertyData *result = findProperty(object, qmlContext, name, revisionMode, &local);

    if (result) {
        if (revisionMode == QObjectWrapper::CheckRevision && result->hasRevision()) {
            if (ddata && ddata->propertyCache && !ddata->propertyCache->isAllowedInRevision(result)) {
                if (hasProperty)
                    *hasProperty = false;
                return Encode::undefined();
            }
        }

        if (hasProperty)
            *hasProperty = true;

        if (property && result != &local)
            *property = result;

        return getProperty(engine, object, result);
    } else {
        // Check if this object is already wrapped.
        if (!ddata || (ddata->jsWrapper.isUndefined() &&
                        (ddata->jsEngineId == 0 || // Nobody owns the QObject
                          !ddata->hasTaintedV4Object))) { // Someone else has used the QObject, but it isn't tainted

            // Not wrapped. Last chance: try query QObjectWrapper's prototype.
            // If it can't handle this, then there is no point
            // to wrap the QObject just to look at an empty set of JS props.
            Object *proto = QObjectWrapper::defaultPrototype(engine);
            return proto->get(name, hasProperty);
        }
    }

    // If we get here, we must already be wrapped (which implies a ddata).
    // There's no point wrapping again, as there wouldn't be any new props.
    Q_ASSERT(ddata);

    Scope scope(engine);
    Scoped<QObjectWrapper> wrapper(scope, wrap(engine, object));
    if (!wrapper) {
        if (hasProperty)
            *hasProperty = false;
        return Encode::null();
    }
    return wrapper->getQmlProperty(qmlContext, name, revisionMode, hasProperty);
}


bool QObjectWrapper::setQmlProperty(
        ExecutionEngine *engine, const QQmlRefPointer<QQmlContextData> &qmlContext, QObject *object,
        String *name, QObjectWrapper::RevisionMode revisionMode, const Value &value)
{
    if (QQmlData::wasDeleted(object))
        return false;

    QQmlPropertyData local;
    QQmlPropertyData *result = QQmlPropertyCache::property(object, name, qmlContext, &local);
    if (!result)
        return false;

    if (revisionMode == QObjectWrapper::CheckRevision && result->hasRevision()) {
        QQmlData *ddata = QQmlData::get(object);
        if (ddata && ddata->propertyCache && !ddata->propertyCache->isAllowedInRevision(result))
            return false;
    }

    setProperty(engine, object, result, value);
    return true;
}

void QObjectWrapper::setProperty(
        ExecutionEngine *engine, QObject *object,
        const QQmlPropertyData *property, const Value &value)
{
    if (!property->isWritable() && !property->isQList()) {
        QString error = QLatin1String("Cannot assign to read-only property \"") +
                        property->name(object) + QLatin1Char('\"');
        engine->throwTypeError(error);
        return;
    }

    Scope scope(engine);
    if (ScopedFunctionObject f(scope, value); f) {
        if (!f->isBinding()) {
            const bool isAliasToAllowed = [&]() {
                if (property->isAlias()) {
                    const QQmlPropertyIndex originalIndex(property->coreIndex(), -1);
                    auto [targetObject, targetIndex] = QQmlPropertyPrivate::findAliasTarget(object, originalIndex);
                    Q_ASSERT(targetObject);
                    QQmlPropertyCache *targetCache
                            = QQmlData::get(targetObject)->propertyCache.data();
                    Q_ASSERT(targetCache);
                    QQmlPropertyData *targetProperty = targetCache->property(targetIndex.coreIndex());
                    object = targetObject;
                    property = targetProperty;
                    return targetProperty->isVarProperty() || targetProperty->propType() == QMetaType::fromType<QJSValue>();
                } else {
                    return false;
                }
            }();
            if (!isAliasToAllowed && !property->isVarProperty()
                    && property->propType() != QMetaType::fromType<QJSValue>()) {
                // assigning a JS function to a non var or QJSValue property or is not allowed.
                QString error = QLatin1String("Cannot assign JavaScript function to ");
                if (!QMetaType(property->propType()).name())
                    error += QLatin1String("[unknown property type]");
                else
                    error += QLatin1String(QMetaType(property->propType()).name());
                scope.engine->throwError(error);
                return;
            }
        } else {

            QQmlRefPointer<QQmlContextData> callingQmlContext = scope.engine->callingQmlContext();
            Scoped<QQmlBindingFunction> bindingFunction(scope, (const Value &)f);
            ScopedFunctionObject f(scope, bindingFunction->bindingFunction());
            ScopedContext ctx(scope, f->scope());

            // binding assignment.
            if (property->isBindable()) {
                const QQmlPropertyIndex idx(property->coreIndex(), /*not a value type*/-1);
                auto [targetObject, targetIndex] = QQmlPropertyPrivate::findAliasTarget(object, idx);
                QUntypedPropertyBinding binding;
                if (f->isBoundFunction()) {
                    auto boundFunction = static_cast<BoundFunction *>(f.getPointer());
                    binding = QQmlPropertyBinding::createFromBoundFunction(property, boundFunction, object, callingQmlContext,
                                                                           ctx, targetObject, targetIndex);
                } else {
                    binding = QQmlPropertyBinding::create(property, f->function(), object, callingQmlContext,
                                                           ctx, targetObject, targetIndex);
                }
                QUntypedBindable bindable;
                void *argv = {&bindable};
                // indirect metacall in case interceptors are installed
                targetObject->metaObject()->metacall(targetObject, QMetaObject::BindableProperty, targetIndex.coreIndex(), &argv);
                bool ok = bindable.setBinding(binding);
                if (!ok) {
                    auto error = QStringLiteral("Failed to set binding on %1::%2.").
                                                arg(QString::fromUtf8(object->metaObject()->className()), property->name(object));
                    scope.engine->throwError(error);
                }
            } else {
                QQmlBinding *newBinding = QQmlBinding::create(property, f->function(), object, callingQmlContext, ctx);
                newBinding->setSourceLocation(bindingFunction->currentLocation());
                if (f->isBoundFunction())
                newBinding->setBoundFunction(static_cast<BoundFunction *>(f.getPointer()));
                newBinding->setTarget(object, *property, nullptr);
                QQmlPropertyPrivate::setBinding(newBinding);
            }
            return;
        }
    }

    if (Q_UNLIKELY(lcBindingRemoval().isInfoEnabled())) {
        if (auto binding = QQmlPropertyPrivate::binding(object, QQmlPropertyIndex(property->coreIndex()))) {
            Q_ASSERT(!binding->isValueTypeProxy());
            const auto qmlBinding = static_cast<const QQmlBinding*>(binding);
            const auto stackFrame = engine->currentStackFrame;
            qCInfo(lcBindingRemoval,
                   "Overwriting binding on %s::%s at %s:%d that was initially bound at %s",
                   object->metaObject()->className(), qPrintable(property->name(object)),
                   qPrintable(stackFrame->source()), stackFrame->lineNumber(),
                   qPrintable(qmlBinding->expressionIdentifier()));
        }
    }
    QQmlPropertyPrivate::removeBinding(object, QQmlPropertyIndex(property->coreIndex()));

    if (property->isVarProperty()) {
        // allow assignment of "special" values (null, undefined, function) to var properties
        QQmlVMEMetaObject *vmemo = QQmlVMEMetaObject::get(object);
        Q_ASSERT(vmemo);
        vmemo->setVMEProperty(property->coreIndex(), value);
        return;
    }

#define PROPERTY_STORE(cpptype, value) \
    cpptype o = value; \
    int status = -1; \
    int flags = 0; \
    void *argv[] = { &o, 0, &status, &flags }; \
    QMetaObject::metacall(object, QMetaObject::WriteProperty, property->coreIndex(), argv);

    const QMetaType propType = property->propType();
    // functions are already handled, except for the QJSValue case
    Q_ASSERT(!value.as<FunctionObject>() || propType == QMetaType::fromType<QJSValue>());

    if (value.isNull() && property->isQObject()) {
        PROPERTY_STORE(QObject*, nullptr);
    } else if (value.isUndefined() && property->isResettable()) {
        void *a[] = { nullptr };
        QMetaObject::metacall(object, QMetaObject::ResetProperty, property->coreIndex(), a);
    } else if (value.isUndefined() && propType == QMetaType::fromType<QVariant>()) {
        PROPERTY_STORE(QVariant, QVariant());
    } else if (value.isUndefined() && propType == QMetaType::fromType<QJsonValue>()) {
        PROPERTY_STORE(QJsonValue, QJsonValue(QJsonValue::Undefined));
    } else if (propType == QMetaType::fromType<QJSValue>()) {
        PROPERTY_STORE(QJSValue, QJSValuePrivate::fromReturnedValue(value.asReturnedValue()));
    } else if (value.isUndefined() && propType != QMetaType::fromType<QQmlScriptString>()) {
        QString error = QLatin1String("Cannot assign [undefined] to ");
        if (!propType.name())
            error += QLatin1String("[unknown property type]");
        else
            error += QLatin1String(propType.name());
        scope.engine->throwError(error);
        return;
    } else if (propType == QMetaType::fromType<int>() && value.isNumber()) {
        PROPERTY_STORE(int, value.asDouble());
    } else if (propType == QMetaType::fromType<qreal>() && value.isNumber()) {
        PROPERTY_STORE(qreal, qreal(value.asDouble()));
    } else if (propType == QMetaType::fromType<float>() && value.isNumber()) {
        PROPERTY_STORE(float, float(value.asDouble()));
    } else if (propType == QMetaType::fromType<double>() && value.isNumber()) {
        PROPERTY_STORE(double, double(value.asDouble()));
    } else if (propType == QMetaType::fromType<QString>() && value.isString()) {
        PROPERTY_STORE(QString, value.toQStringNoThrow());
    } else if (property->isVarProperty()) {
        QQmlVMEMetaObject *vmemo = QQmlVMEMetaObject::get(object);
        Q_ASSERT(vmemo);
        vmemo->setVMEProperty(property->coreIndex(), value);
    } else if (propType == QMetaType::fromType<QQmlScriptString>()
               && (value.isUndefined() || value.isPrimitive())) {
        QQmlScriptString ss(value.toQStringNoThrow(), nullptr /* context */, object);
        if (value.isNumber()) {
            ss.d->numberValue = value.toNumber();
            ss.d->isNumberLiteral = true;
        } else if (value.isString()) {
            ss.d->script = CompiledData::Binding::escapedString(ss.d->script);
            ss.d->isStringLiteral = true;
        }
        PROPERTY_STORE(QQmlScriptString, ss);
    } else {
        QVariant v;
        if (property->isQList())
            v = scope.engine->toVariant(value, QMetaType::fromType<QList<QObject *> >());
        else
            v = scope.engine->toVariant(value, propType);

        QQmlRefPointer<QQmlContextData> callingQmlContext = scope.engine->callingQmlContext();
        if (!QQmlPropertyPrivate::write(object, *property, v, callingQmlContext)) {
            const char *valueType = (v.userType() == QMetaType::UnknownType)
                    ? "an unknown type"
                    : QMetaType(v.userType()).name();

            const char *targetTypeName = propType.name();
            if (!targetTypeName)
                targetTypeName = "an unregistered type";

            QString error = QLatin1String("Cannot assign ") +
                            QLatin1String(valueType) +
                            QLatin1String(" to ") +
                            QLatin1String(targetTypeName);
            scope.engine->throwError(error);
            return;
        }
    }
}

ReturnedValue QObjectWrapper::wrap_slowPath(ExecutionEngine *engine, QObject *object)
{
    Q_ASSERT(!QQmlData::wasDeleted(object));

    QQmlData *ddata = QQmlData::get(object, true);
    if (!ddata)
        return Encode::undefined();

    Scope scope(engine);

    if (ddata->jsWrapper.isUndefined() &&
               (ddata->jsEngineId == engine->m_engineId || // We own the QObject
                ddata->jsEngineId == 0 ||    // No one owns the QObject
                !ddata->hasTaintedV4Object)) { // Someone else has used the QObject, but it isn't tainted

        ScopedValue rv(scope, create(engine, object));
        ddata->jsWrapper.set(scope.engine, rv);
        ddata->jsEngineId = engine->m_engineId;
        return rv->asReturnedValue();

    } else {
        // If this object is tainted, we have to check to see if it is in our
        // tainted object list
        ScopedObject alternateWrapper(scope, (Object *)nullptr);
        if (engine->m_multiplyWrappedQObjects && ddata->hasTaintedV4Object)
            alternateWrapper = engine->m_multiplyWrappedQObjects->value(object);

        // If our tainted handle doesn't exist or has been collected, and there isn't
        // a handle in the ddata, we can assume ownership of the ddata->jsWrapper
        if (ddata->jsWrapper.isUndefined() && !alternateWrapper) {
            ScopedValue result(scope, create(engine, object));
            ddata->jsWrapper.set(scope.engine, result);
            ddata->jsEngineId = engine->m_engineId;
            return result->asReturnedValue();
        }

        if (!alternateWrapper) {
            alternateWrapper = create(engine, object);
            if (!engine->m_multiplyWrappedQObjects)
                engine->m_multiplyWrappedQObjects = new MultiplyWrappedQObjectMap;
            engine->m_multiplyWrappedQObjects->insert(object, alternateWrapper->d());
            ddata->hasTaintedV4Object = true;
        }

        return alternateWrapper.asReturnedValue();
    }
}

void QObjectWrapper::markWrapper(QObject *object, MarkStack *markStack)
{
    if (QQmlData::wasDeleted(object))
        return;

    QQmlData *ddata = QQmlData::get(object);
    if (!ddata)
        return;

    const ExecutionEngine *engine = markStack->engine();
    if (ddata->jsEngineId == engine->m_engineId)
        ddata->jsWrapper.markOnce(markStack);
    else if (engine->m_multiplyWrappedQObjects && ddata->hasTaintedV4Object)
        engine->m_multiplyWrappedQObjects->mark(object, markStack);
}

void QObjectWrapper::setProperty(ExecutionEngine *engine, int propertyIndex, const Value &value)
{
    setProperty(engine, d()->object(), propertyIndex, value);
}

void QObjectWrapper::setProperty(ExecutionEngine *engine, QObject *object, int propertyIndex, const Value &value)
{
    Q_ASSERT(propertyIndex < 0xffff);
    Q_ASSERT(propertyIndex >= 0);

    if (QQmlData::wasDeleted(object))
        return;
    QQmlData *ddata = QQmlData::get(object, /*create*/false);
    if (!ddata)
        return;

    Q_ASSERT(ddata->propertyCache);
    QQmlPropertyData *property = ddata->propertyCache->property(propertyIndex);
    Q_ASSERT(property); // We resolved this property earlier, so it better exist!
    return setProperty(engine, object, property, value);
}

bool QObjectWrapper::virtualIsEqualTo(Managed *a, Managed *b)
{
    Q_ASSERT(a->as<QObjectWrapper>());
    QObjectWrapper *qobjectWrapper = static_cast<QObjectWrapper *>(a);
    Object *o = b->as<Object>();
    if (o) {
        if (QQmlTypeWrapper *qmlTypeWrapper = o->as<QQmlTypeWrapper>())
            return qmlTypeWrapper->toVariant().value<QObject*>() == qobjectWrapper->object();
    }

    return false;
}

ReturnedValue QObjectWrapper::create(ExecutionEngine *engine, QObject *object)
{
    if (QQmlRefPointer<QQmlPropertyCache> cache = QQmlData::ensurePropertyCache(object)) {
        ReturnedValue result = Encode::null();
        void *args[] = { &result, &engine };
        if (cache->callJSFactoryMethod(object, args))
            return result;
    }
    return (engine->memoryManager->allocate<QObjectWrapper>(object))->asReturnedValue();
}

ReturnedValue QObjectWrapper::virtualGet(const Managed *m, PropertyKey id, const Value *receiver, bool *hasProperty)
{
    if (!id.isString())
        return Object::virtualGet(m, id, receiver, hasProperty);

    const QObjectWrapper *that = static_cast<const QObjectWrapper*>(m);
    Scope scope(that);
    ScopedString n(scope, id.asStringOrSymbol());
    QQmlRefPointer<QQmlContextData> qmlContext = that->engine()->callingQmlContext();
    return that->getQmlProperty(qmlContext, n, IgnoreRevision, hasProperty, /*includeImports*/ true);
}

bool QObjectWrapper::virtualPut(Managed *m, PropertyKey id, const Value &value, Value *receiver)
{
    if (!id.isString())
        return Object::virtualPut(m, id, value, receiver);

    Scope scope(m);
    QObjectWrapper *that = static_cast<QObjectWrapper*>(m);
    ScopedString name(scope, id.asStringOrSymbol());

    if (that->internalClass()->isFrozen) {
        QString error = QLatin1String("Cannot assign to property \"") +
                        name->toQString() + QLatin1String("\" of read-only object");
        scope.engine->throwError(error);
        return false;
    }

    if (scope.hasException() || QQmlData::wasDeleted(that->d()->object()))
        return false;

    QQmlRefPointer<QQmlContextData> qmlContext = scope.engine->callingQmlContext();
    if (!setQmlProperty(scope.engine, qmlContext, that->d()->object(), name, QObjectWrapper::IgnoreRevision, value)) {
        QQmlData *ddata = QQmlData::get(that->d()->object());
        // Types created by QML are not extensible at run-time, but for other QObjects we can store them
        // as regular JavaScript properties, like on JavaScript objects.
        if (ddata && ddata->context) {
            QString error = QLatin1String("Cannot assign to non-existent property \"") +
                            name->toQString() + QLatin1Char('\"');
            scope.engine->throwError(error);
            return false;
        } else {
            return Object::virtualPut(m, id, value, receiver);
        }
    }

    return true;
}

PropertyAttributes QObjectWrapper::virtualGetOwnProperty(const Managed *m, PropertyKey id, Property *p)
{
    if (id.isString()) {
        const QObjectWrapper *that = static_cast<const QObjectWrapper*>(m);
        const QObject *thatObject = that->d()->object();
        if (!QQmlData::wasDeleted(thatObject)) {
            Scope scope(m);
            ScopedString n(scope, id.asStringOrSymbol());
            QQmlRefPointer<QQmlContextData> qmlContext = scope.engine->callingQmlContext();
            QQmlPropertyData local;
            if (that->findProperty(qmlContext, n, IgnoreRevision, &local)
                    || n->equals(scope.engine->id_destroy()) || n->equals(scope.engine->id_toString())) {
                if (p) {
                    // ### probably not the fastest implementation
                    bool hasProperty;
                    p->value = that->getQmlProperty(qmlContext, n, IgnoreRevision, &hasProperty, /*includeImports*/ true);
                }
                return Attr_Data;
            }
        }
    }

    return Object::virtualGetOwnProperty(m, id, p);
}

struct QObjectWrapperOwnPropertyKeyIterator : ObjectOwnPropertyKeyIterator
{
    int propertyIndex = 0;
    ~QObjectWrapperOwnPropertyKeyIterator() override = default;
    PropertyKey next(const Object *o, Property *pd = nullptr, PropertyAttributes *attrs = nullptr) override;

private:
    QSet<QByteArray> m_alreadySeen;
};

PropertyKey QObjectWrapperOwnPropertyKeyIterator::next(const Object *o, Property *pd, PropertyAttributes *attrs)
{
    // Used to block access to QObject::destroyed() and QObject::deleteLater() from QML
    static const int destroyedIdx1 = QObject::staticMetaObject.indexOfSignal("destroyed(QObject*)");
    static const int destroyedIdx2 = QObject::staticMetaObject.indexOfSignal("destroyed()");
    static const int deleteLaterIdx = QObject::staticMetaObject.indexOfSlot("deleteLater()");

    const QObjectWrapper *that = static_cast<const QObjectWrapper*>(o);

    QObject *thatObject = that->d()->object();
    if (thatObject && !QQmlData::wasDeleted(thatObject)) {
        const QMetaObject *mo = thatObject->metaObject();
        // These indices don't apply to gadgets, so don't block them.
        const bool preventDestruction = mo->superClass() || mo == &QObject::staticMetaObject;
        const int propertyCount = mo->propertyCount();
        if (propertyIndex < propertyCount) {
            ExecutionEngine *thatEngine = that->engine();
            Scope scope(thatEngine);
            const QMetaProperty property = mo->property(propertyIndex);
            ScopedString propName(scope, thatEngine->newString(QString::fromUtf8(property.name())));
            ++propertyIndex;
            if (attrs)
                *attrs= Attr_Data;
            if (pd) {
                QQmlPropertyData local;
                local.load(property);
                pd->value = that->getProperty(thatEngine, thatObject, &local);
            }
            return propName->toPropertyKey();
        }
        const int methodCount = mo->methodCount();
        while (propertyIndex < propertyCount + methodCount) {
            Q_ASSERT(propertyIndex >= propertyCount);
            int index = propertyIndex - propertyCount;
            const QMetaMethod method = mo->method(index);
            ++propertyIndex;
            if (method.access() == QMetaMethod::Private || (preventDestruction && (index == deleteLaterIdx || index == destroyedIdx1 || index == destroyedIdx2)))
                continue;
            // filter out duplicates due to overloads:
            if (m_alreadySeen.contains(method.name()))
                continue;
            else
                m_alreadySeen.insert(method.name());
            ExecutionEngine *thatEngine = that->engine();
            Scope scope(thatEngine);
            ScopedString methodName(scope, thatEngine->newString(QString::fromUtf8(method.name())));
            if (attrs)
                *attrs = Attr_Data;
            if (pd) {
                QQmlPropertyData local;
                local.load(method);
                pd->value = that->getProperty(thatEngine, thatObject, &local);
            }
            return methodName->toPropertyKey();
        }
    }

    return ObjectOwnPropertyKeyIterator::next(o, pd, attrs);
}

OwnPropertyKeyIterator *QObjectWrapper::virtualOwnPropertyKeys(const Object *m, Value *target)
{
    *target = *m;
    return new QObjectWrapperOwnPropertyKeyIterator;
}

ReturnedValue QObjectWrapper::virtualResolveLookupGetter(const Object *object, ExecutionEngine *engine, Lookup *lookup)
{
    // Keep this code in sync with ::getQmlProperty
    PropertyKey id = engine->identifierTable->asPropertyKey(engine->currentStackFrame->v4Function->compilationUnit->
                                                            runtimeStrings[lookup->nameIndex]);
    if (!id.isString())
        return Object::virtualResolveLookupGetter(object, engine, lookup);
    Scope scope(engine);

    const QObjectWrapper *This = static_cast<const QObjectWrapper *>(object);
    ScopedString name(scope, id.asStringOrSymbol());
    QQmlRefPointer<QQmlContextData> qmlContext = engine->callingQmlContext();

    QObject * const qobj = This->d()->object();

    if (QQmlData::wasDeleted(qobj))
        return Encode::undefined();

    if (auto methodValue = getDestroyOrToStringMethod(engine, name, qobj))
        return *methodValue;

    QQmlData *ddata = QQmlData::get(qobj, false);
    if (!ddata || !ddata->propertyCache) {
        QQmlPropertyData local;
        QQmlPropertyData *property = QQmlPropertyCache::property(qobj, name, qmlContext, &local);
        return property ? getProperty(engine, qobj, property) : Encode::undefined();
    }
    QQmlPropertyData *property = ddata->propertyCache->property(name.getPointer(), qobj, qmlContext);

    if (!property) {
        // Check for attached properties
        if (name->startsWithUpper()) {
            if (auto importProperty = getPropertyFromImports(engine, name, qmlContext, qobj))
                return *importProperty;
        }
        return Object::virtualResolveLookupGetter(object, engine, lookup);
    }

    setupQObjectLookup(lookup, ddata, property, This);
    lookup->getter = Lookup::getterQObject;
    return lookup->getter(lookup, engine, *object);
}

ReturnedValue QObjectWrapper::lookupAttached(
            Lookup *l, ExecutionEngine *engine, const Value &object)
{
    return Lookup::getterGeneric(l, engine, object);
}

bool QObjectWrapper::virtualResolveLookupSetter(Object *object, ExecutionEngine *engine, Lookup *lookup,
                                                const Value &value)
{
    return Object::virtualResolveLookupSetter(object, engine, lookup, value);
}

struct QObjectSlotDispatcher : public QtPrivate::QSlotObjectBase
{
    PersistentValue function;
    PersistentValue thisObject;
    QMetaMethod signal;

    QObjectSlotDispatcher()
        : QtPrivate::QSlotObjectBase(&impl)
    {}

    static void impl(int which, QSlotObjectBase *this_, QObject *receiver, void **metaArgs, bool *ret)
    {
        Q_UNUSED(receiver);
        switch (which) {
        case Destroy: {
            delete static_cast<QObjectSlotDispatcher*>(this_);
        }
        break;
        case Call: {
            QObjectSlotDispatcher *This = static_cast<QObjectSlotDispatcher*>(this_);
            ExecutionEngine *v4 = This->function.engine();
            // Might be that we're still connected to a signal that's emitted long
            // after the engine died. We don't track connections in a global list, so
            // we need this safeguard.
            if (!v4)
                break;

            QQmlMetaObject::ArgTypeStorage storage;
            QQmlMetaObject::methodParameterTypes(This->signal, &storage, nullptr);

            int argCount = storage.size();

            Scope scope(v4);
            ScopedFunctionObject f(scope, This->function.value());

            JSCallArguments jsCallData(scope, argCount);
            *jsCallData.thisObject = This->thisObject.isUndefined() ? v4->globalObject->asReturnedValue() : This->thisObject.value();
            for (int ii = 0; ii < argCount; ++ii) {
                QMetaType type = storage[ii];
                if (type == QMetaType::fromType<QVariant>()) {
                    jsCallData.args[ii] = v4->fromVariant(*((QVariant *)metaArgs[ii + 1]));
                } else {
                    jsCallData.args[ii] = v4->fromVariant(QVariant(type, metaArgs[ii + 1]));
                }
            }

            f->call(jsCallData);
            if (scope.hasException()) {
                QQmlError error = v4->catchExceptionAsQmlError();
                if (error.description().isEmpty()) {
                    ScopedString name(scope, f->name());
                    error.setDescription(QStringLiteral("Unknown exception occurred during evaluation of connected function: %1").arg(name->toQString()));
                }
                if (QQmlEngine *qmlEngine = v4->qmlEngine()) {
                    QQmlEnginePrivate::get(qmlEngine)->warning(error);
                } else {
                    QMessageLogger(error.url().toString().toLatin1().constData(),
                                   error.line(), nullptr).warning().noquote()
                            << error.toString();
                }
            }
        }
        break;
        case Compare: {
            QObjectSlotDispatcher *connection = static_cast<QObjectSlotDispatcher*>(this_);
            if (connection->function.isUndefined()) {
                *ret = false;
                return;
            }

            // This is tricky. Normally the metaArgs[0] pointer is a pointer to the _function_
            // for the new-style QObject::connect. Here we use the engine pointer as sentinel
            // to distinguish those type of QSlotObjectBase connections from our QML connections.
            ExecutionEngine *v4 = reinterpret_cast<ExecutionEngine*>(metaArgs[0]);
            if (v4 != connection->function.engine()) {
                *ret = false;
                return;
            }

            Scope scope(v4);
            ScopedValue function(scope, *reinterpret_cast<Value*>(metaArgs[1]));
            ScopedValue thisObject(scope, *reinterpret_cast<Value*>(metaArgs[2]));
            QObject *receiverToDisconnect = reinterpret_cast<QObject*>(metaArgs[3]);
            int slotIndexToDisconnect = *reinterpret_cast<int*>(metaArgs[4]);

            if (slotIndexToDisconnect != -1) {
                // This is a QObject function wrapper
                if (connection->thisObject.isUndefined() == thisObject->isUndefined() &&
                        (connection->thisObject.isUndefined() || RuntimeHelpers::strictEqual(*connection->thisObject.valueRef(), thisObject))) {

                    ScopedFunctionObject f(scope, connection->function.value());
                    QPair<QObject *, int> connectedFunctionData = QObjectMethod::extractQtMethod(f);
                    if (connectedFunctionData.first == receiverToDisconnect &&
                        connectedFunctionData.second == slotIndexToDisconnect) {
                        *ret = true;
                        return;
                    }
                }
            } else {
                // This is a normal JS function
                if (RuntimeHelpers::strictEqual(*connection->function.valueRef(), function) &&
                        connection->thisObject.isUndefined() == thisObject->isUndefined() &&
                        (connection->thisObject.isUndefined() || RuntimeHelpers::strictEqual(*connection->thisObject.valueRef(), thisObject))) {
                    *ret = true;
                    return;
                }
            }

            *ret = false;
        }
        break;
        case NumOperations:
        break;
        }
    };
};

ReturnedValue QObjectWrapper::method_connect(const FunctionObject *b, const Value *thisObject, const Value *argv, int argc)
{
    Scope scope(b);

    if (argc == 0)
        THROW_GENERIC_ERROR("Function.prototype.connect: no arguments given");

    QPair<QObject *, int> signalInfo = extractQtSignal(*thisObject);
    QObject *signalObject = signalInfo.first;
    int signalIndex = signalInfo.second; // in method range, not signal range!

    if (signalIndex < 0)
        THROW_GENERIC_ERROR("Function.prototype.connect: this object is not a signal");

    if (!signalObject)
        THROW_GENERIC_ERROR("Function.prototype.connect: cannot connect to deleted QObject");

    auto signalMetaMethod = signalObject->metaObject()->method(signalIndex);
    if (signalMetaMethod.methodType() != QMetaMethod::Signal)
        THROW_GENERIC_ERROR("Function.prototype.connect: this object is not a signal");

    ScopedFunctionObject f(scope);
    ScopedValue object (scope, Encode::undefined());

    if (argc == 1) {
        f = argv[0];
    } else if (argc >= 2) {
        object = argv[0];
        f = argv[1];
    }

    if (!f)
        THROW_GENERIC_ERROR("Function.prototype.connect: target is not a function");

    if (!object->isUndefined() && !object->isObject())
        THROW_GENERIC_ERROR("Function.prototype.connect: target this is not an object");

    QObjectSlotDispatcher *slot = new QObjectSlotDispatcher;
    slot->signal = signalMetaMethod;

    slot->thisObject.set(scope.engine, object);
    slot->function.set(scope.engine, f);

    if (QQmlData *ddata = QQmlData::get(signalObject)) {
        if (QQmlPropertyCache *propertyCache = ddata->propertyCache.data()) {
            QQmlPropertyPrivate::flushSignal(signalObject, propertyCache->methodIndexToSignalIndex(signalIndex));
        }
    }

    QPair<QObject *, int> functionData = QObjectMethod::extractQtMethod(f); // align with disconnect
    if (QObject *receiver = functionData.first) {
        QObjectPrivate::connect(signalObject, signalIndex, receiver, slot, Qt::AutoConnection);
    } else {
        qCInfo(lcObjectConnect,
               "Could not find receiver of the connection, using sender as receiver. Disconnect "
               "explicitly (or delete the sender) to make sure the connection is removed.");
        QObjectPrivate::connect(signalObject, signalIndex, signalObject, slot, Qt::AutoConnection);
    }

    RETURN_UNDEFINED();
}

ReturnedValue QObjectWrapper::method_disconnect(const FunctionObject *b, const Value *thisObject, const Value *argv, int argc)
{
    Scope scope(b);

    if (argc == 0)
        THROW_GENERIC_ERROR("Function.prototype.disconnect: no arguments given");

    QPair<QObject *, int> signalInfo = extractQtSignal(*thisObject);
    QObject *signalObject = signalInfo.first;
    int signalIndex = signalInfo.second;

    if (signalIndex == -1)
        THROW_GENERIC_ERROR("Function.prototype.disconnect: this object is not a signal");

    if (!signalObject)
        THROW_GENERIC_ERROR("Function.prototype.disconnect: cannot disconnect from deleted QObject");

    if (signalIndex < 0 || signalObject->metaObject()->method(signalIndex).methodType() != QMetaMethod::Signal)
        THROW_GENERIC_ERROR("Function.prototype.disconnect: this object is not a signal");

    ScopedFunctionObject functionValue(scope);
    ScopedValue functionThisValue(scope, Encode::undefined());

    if (argc == 1) {
        functionValue = argv[0];
    } else if (argc >= 2) {
        functionThisValue = argv[0];
        functionValue = argv[1];
    }

    if (!functionValue)
        THROW_GENERIC_ERROR("Function.prototype.disconnect: target is not a function");

    if (!functionThisValue->isUndefined() && !functionThisValue->isObject())
        THROW_GENERIC_ERROR("Function.prototype.disconnect: target this is not an object");

    QPair<QObject *, int> functionData = QObjectMethod::extractQtMethod(functionValue);

    void *a[] = {
        scope.engine,
        functionValue.ptr,
        functionThisValue.ptr,
        functionData.first,
        &functionData.second
    };

    if (QObject *receiver = functionData.first) {
        QObjectPrivate::disconnect(signalObject, signalIndex, receiver,
                                   reinterpret_cast<void **>(&a));
    } else {
        QObjectPrivate::disconnect(signalObject, signalIndex, signalObject,
                                   reinterpret_cast<void **>(&a));
    }

    RETURN_UNDEFINED();
}

static void markChildQObjectsRecursively(QObject *parent, MarkStack *markStack)
{
    const QObjectList &children = parent->children();
    for (int i = 0; i < children.count(); ++i) {
        QObject *child = children.at(i);
        if (!child)
            continue;
        QObjectWrapper::markWrapper(child, markStack);
        markChildQObjectsRecursively(child, markStack);
    }
}

void Heap::QObjectWrapper::markObjects(Heap::Base *that, MarkStack *markStack)
{
    QObjectWrapper *This = static_cast<QObjectWrapper *>(that);

    if (QObject *o = This->object()) {
        QQmlVMEMetaObject *vme = QQmlVMEMetaObject::get(o);
        if (vme)
            vme->mark(markStack);

        // Children usually don't need to be marked, the gc keeps them alive.
        // But in the rare case of a "floating" QObject without a parent that
        // _gets_ marked (we've been called here!) then we also need to
        // propagate the marking down to the children recursively.
        if (!o->parent())
            markChildQObjectsRecursively(o, markStack);
    }

    Object::markObjects(that, markStack);
}

void QObjectWrapper::destroyObject(bool lastCall)
{
    Heap::QObjectWrapper *h = d();
    Q_ASSERT(h->internalClass);

    if (h->object()) {
        QQmlData *ddata = QQmlData::get(h->object(), false);
        if (ddata) {
            if (!h->object()->parent() && !ddata->indestructible) {
                if (ddata && ddata->ownContext) {
                    Q_ASSERT(ddata->ownContext.data() == ddata->context);
                    ddata->ownContext->emitDestruction();
                    ddata->ownContext.reset();
                    ddata->context = nullptr;
                }
                // This object is notionally destroyed now
                ddata->isQueuedForDeletion = true;
                ddata->disconnectNotifiers();
                if (lastCall)
                    delete h->object();
                else
                    h->object()->deleteLater();
            } else {
                // If the object is C++-owned, we still have to release the weak reference we have
                // to it.
                ddata->jsWrapper.clear();
                if (lastCall && ddata->propertyCache)
                    ddata->propertyCache.reset();
            }
        }
    }

    h->destroy();
}


DEFINE_OBJECT_VTABLE(QObjectWrapper);

namespace {

template<typename A, typename B, typename C, typename D, typename E, typename F, typename G>
class MaxSizeOf7 {
    template<typename Z, typename X>
    struct SMax {
        char dummy[sizeof(Z) > sizeof(X) ? sizeof(Z) : sizeof(X)];
    };
public:
    static const size_t Size = sizeof(SMax<A, SMax<B, SMax<C, SMax<D, SMax<E, SMax<F, G> > > > > >);
};

struct CallArgument {
    Q_DISABLE_COPY_MOVE(CallArgument);

    CallArgument() = default;
    ~CallArgument() { cleanup(); }

    inline void *dataPtr();

    inline void initAsType(QMetaType type);
    inline bool fromValue(QMetaType type, ExecutionEngine *, const Value &);
    inline ReturnedValue toValue(ExecutionEngine *);

private:
    // QVariantWrappedType denotes that we're storing a QVariant, but we mean
    // the type inside the QVariant, not QVariant itself.
    enum { QVariantWrappedType = -1 };

    inline void cleanup();

    template <class T, class M>
    bool fromContainerValue(const Value &object, M CallArgument::*member);

    union {
        float floatValue;
        double doubleValue;
        quint32 intValue;
        bool boolValue;
        QObject *qobjectPtr;
        std::vector<int> *stdVectorIntPtr;
        std::vector<qreal> *stdVectorRealPtr;
        std::vector<bool> *stdVectorBoolPtr;
        std::vector<QString> *stdVectorQStringPtr;
        std::vector<QUrl> *stdVectorQUrlPtr;
#if QT_CONFIG(qml_itemmodel)
        std::vector<QModelIndex> *stdVectorQModelIndexPtr;
#endif

        char allocData[MaxSizeOf7<QVariant,
                                  QString,
                                  QList<QObject *>,
                                  QJSValue,
                                  QJsonArray,
                                  QJsonObject,
                                  QJsonValue>::Size];
        qint64 q_for_alignment;
    };

    // Pointers to allocData
    union {
        QString *qstringPtr;
        QByteArray *qbyteArrayPtr;
        QVariant *qvariantPtr;
        QList<QObject *> *qlistPtr;
        QJSValue *qjsValuePtr;
        QJsonArray *jsonArrayPtr;
        QJsonObject *jsonObjectPtr;
        QJsonValue *jsonValuePtr;
    };

    int type = QMetaType::UnknownType;
};
}

static ReturnedValue CallMethod(const QQmlObjectOrGadget &object, int index, QMetaType returnType, int argCount,
                                         const QMetaType *argTypes, ExecutionEngine *engine, CallData *callArgs,
                                         QMetaObject::Call callType = QMetaObject::InvokeMetaMethod)
{
    if (argCount > 0) {
        // Convert all arguments.
        QVarLengthArray<CallArgument, 9> args(argCount + 1);
        args[0].initAsType(returnType);
        for (int ii = 0; ii < argCount; ++ii) {
            if (!args[ii + 1].fromValue(argTypes[ii], engine,
                                        callArgs->args[ii].asValue<Value>())) {
                qWarning() << QString::fromLatin1("Could not convert argument %1 at").arg(ii);
                const StackTrace stack = engine->stackTrace();
                for (const StackFrame &frame : stack) {
                    qWarning() << "\t" << frame.function + QLatin1Char('@') + frame.source
                                    + (frame.line > 0
                                               ? (QLatin1Char(':') + QString::number(frame.line))
                                               : QString());

                }

                const bool is_signal =
                        object.metaObject()->method(index).methodType() == QMetaMethod::Signal;
                if (is_signal) {
                    qWarning() << "Passing incomatible arguments to signals is not supported.";
                } else {
                    return engine->throwTypeError(
                            QLatin1String("Passing incompatible arguments to C++ functions from "
                            "JavaScript is not allowed."));
                }
            }
        }
        QVarLengthArray<void *, 9> argData(args.count());
        for (int ii = 0; ii < args.count(); ++ii)
            argData[ii] = args[ii].dataPtr();

        object.metacall(callType, index, argData.data());

        return args[0].toValue(engine);

    } else if (returnType != QMetaType::fromType<void>()) {

        CallArgument arg;
        arg.initAsType(returnType);

        void *args[] = { arg.dataPtr() };

        object.metacall(callType, index, args);

        return arg.toValue(engine);

    } else {

        void *args[] = { nullptr };
        object.metacall(callType, index, args);
        return Encode::undefined();

    }
}

/*
    Returns the match score for converting \a actual to be of type \a conversionType.  A
    zero score means "perfect match" whereas a higher score is worse.

    The conversion table is copied out of the \l QScript::callQtMethod() function.
*/
static int MatchScore(const Value &actual, QMetaType conversionMetaType)
{
    const int conversionType = conversionMetaType.id();
    if (actual.isNumber()) {
        switch (conversionType) {
        case QMetaType::Double:
            return 0;
        case QMetaType::Float:
            return 1;
        case QMetaType::LongLong:
        case QMetaType::ULongLong:
            return 2;
        case QMetaType::Long:
        case QMetaType::ULong:
            return 3;
        case QMetaType::Int:
        case QMetaType::UInt:
            return 4;
        case QMetaType::Short:
        case QMetaType::UShort:
            return 5;
            break;
        case QMetaType::Char:
        case QMetaType::UChar:
            return 6;
        case QMetaType::QJsonValue:
            return 5;
        default:
            return 10;
        }
    } else if (actual.isString()) {
        switch (conversionType) {
        case QMetaType::QString:
            return 0;
        case QMetaType::QJsonValue:
            return 5;
        default:
            return 10;
        }
    } else if (actual.isBoolean()) {
        switch (conversionType) {
        case QMetaType::Bool:
            return 0;
        case QMetaType::QJsonValue:
            return 5;
        default:
            return 10;
        }
    } else if (actual.as<DateObject>()) {
        switch (conversionType) {
        case QMetaType::QDateTime:
            return 0;
        case QMetaType::QDate:
            return 1;
        case QMetaType::QTime:
            return 2;
        default:
            return 10;
        }
    } else if (actual.as<RegExpObject>()) {
        switch (conversionType) {
#if QT_CONFIG(regularexpression)
        case QMetaType::QRegularExpression:
            return 0;
#endif
        default:
            return 10;
        }
    } else if (actual.as<ArrayBuffer>()) {
        switch (conversionType) {
        case QMetaType::QByteArray:
            return 0;
        default:
            return 10;
        }
    } else if (actual.as<ArrayObject>()) {
        switch (conversionType) {
        case QMetaType::QJsonArray:
            return 3;
        case QMetaType::QStringList:
        case QMetaType::QVariantList:
            return 5;
        case QMetaType::QVector4D:
        case QMetaType::QMatrix4x4:
            return 6;
        case QMetaType::QVector3D:
            return 7;
        default:
            return 10;
        }
    } else if (actual.isNull()) {
        switch (conversionType) {
        case QMetaType::Nullptr:
        case QMetaType::VoidStar:
        case QMetaType::QObjectStar:
        case QMetaType::QJsonValue:
            return 0;
        default: {
            if (conversionMetaType.flags().testFlag(QMetaType::IsPointer))
                return 0;
            else
                return 10;
        }
        }
    } else if (const Object *obj = actual.as<Object>()) {
        if (obj->as<VariantObject>()) {
            if (conversionType == qMetaTypeId<QVariant>())
                return 0;
            if (obj->engine()->toVariant(actual, QMetaType {}).metaType() == conversionMetaType)
                return 0;
            else
                return 10;
        }

        if (obj->as<QObjectWrapper>()) {
            switch (conversionType) {
            case QMetaType::QObjectStar:
                return 0;
            default:
                return 10;
            }
        }

        if (auto sequenceMetaType = SequencePrototype::metaTypeForSequence(obj); sequenceMetaType.isValid()) {
            if (sequenceMetaType == conversionMetaType)
                return 1;
            else
                return 10;
        }

        if (obj->as<QQmlValueTypeWrapper>()) {
            const QVariant v = obj->engine()->toVariant(actual, QMetaType {});
            if (v.userType() == conversionType)
                return 0;
            else if (v.canConvert(conversionMetaType))
                return 5;
            return 10;
        }

        if (conversionType == QMetaType::QJsonObject)
            return 5;
        if (conversionType == qMetaTypeId<QJSValue>())
            return 0;
        if (conversionType == QMetaType::QVariantMap)
            return 5;
    }

    return 10;
}

static int numDefinedArguments(CallData *callArgs)
{
    int numDefinedArguments = callArgs->argc();
    while (numDefinedArguments > 0
           && callArgs->args[numDefinedArguments - 1].type() == StaticValue::Undefined_Type) {
        --numDefinedArguments;
    }
    return numDefinedArguments;
}

static ReturnedValue CallPrecise(const QQmlObjectOrGadget &object, const QQmlPropertyData &data,
                                      ExecutionEngine *engine, CallData *callArgs,
                                      QMetaObject::Call callType = QMetaObject::InvokeMetaMethod)
{
    QByteArray unknownTypeError;

    QMetaType returnType = object.methodReturnType(data, &unknownTypeError);

    if (!returnType.isValid()) {
        return engine->throwError(QLatin1String("Unknown method return type: ")
                                  + QLatin1String(unknownTypeError));
    }

    auto handleTooManyArguments = [&](int expectedArguments) {
        const QMetaObject *metaObject = object.metaObject();
        const int indexOfClassInfo = metaObject->indexOfClassInfo("QML.StrictArguments");
        if (indexOfClassInfo != -1
                && QString::fromUtf8(metaObject->classInfo(indexOfClassInfo).value())
                    == QStringLiteral("true")) {
            engine->throwError(QStringLiteral("Too many arguments"));
            return false;
        }

        const auto stackTrace = engine->stackTrace();
        if (stackTrace.isEmpty()) {
            qWarning().nospace().noquote()
                    << "When matching arguments for "
                    << object.className() << "::" << data.name(object.metaObject()) << "():";
        } else {
            const StackFrame frame = engine->stackTrace().first();
            qWarning().noquote() << frame.function + QLatin1Char('@') + frame.source
                            + (frame.line > 0 ? (QLatin1Char(':') + QString::number(frame.line))
                                              : QString());
        }

        qWarning().noquote() << QStringLiteral("Too many arguments, ignoring %1")
                                        .arg(callArgs->argc() - expectedArguments);
        return true;
    };

    const int definedArgumentCount = numDefinedArguments(callArgs);

    if (data.hasArguments()) {

        QQmlMetaObject::ArgTypeStorage storage;

        bool ok = false;
        if (data.isConstructor())
            ok = object.constructorParameterTypes(data.coreIndex(), &storage, &unknownTypeError);
        else
            ok = object.methodParameterTypes(data.coreIndex(), &storage, &unknownTypeError);

        if (!ok) {
            return engine->throwError(QLatin1String("Unknown method parameter type: ")
                                      + QLatin1String(unknownTypeError));
        }

        if (storage.size() > callArgs->argc()) {
            QString error = QLatin1String("Insufficient arguments");
            return engine->throwError(error);
        }

        if (storage.size() < definedArgumentCount) {
            if (!handleTooManyArguments(storage.size()))
                return Encode::undefined();

        }

        return CallMethod(object, data.coreIndex(), returnType, storage.size(), storage.constData(), engine, callArgs, callType);

    } else {
        if (definedArgumentCount > 0 && !handleTooManyArguments(0))
            return Encode::undefined();

        return CallMethod(object, data.coreIndex(), returnType, 0, nullptr, engine, callArgs, callType);
    }
}

/*
Resolve the overloaded method to call.  The algorithm works conceptually like this:
    1.  Resolve the set of overloads it is *possible* to call.
        Impossible overloads include those that have too many parameters or have parameters
        of unknown type.
    2.  Filter the set of overloads to only contain those with the closest number of
        parameters.
        For example, if we are called with 3 parameters and there are 2 overloads that
        take 2 parameters and one that takes 3, eliminate the 2 parameter overloads.
    3.  Find the best remaining overload based on its match score.
        If two or more overloads have the same match score, return the last one. The match
        score is constructed by adding the matchScore() result for each of the parameters.
*/
static const QQmlPropertyData *ResolveOverloaded(
            const QQmlObjectOrGadget &object, const QQmlPropertyData *methods, int methodCount,
            ExecutionEngine *engine, CallData *callArgs)
{
    const int argumentCount = callArgs->argc();
    const int definedArgumentCount = numDefinedArguments(callArgs);

    const QQmlPropertyData *best = nullptr;
    int bestParameterScore = INT_MAX;
    int bestMaxMatchScore = INT_MAX;
    int bestSumMatchScore = INT_MAX;

    Scope scope(engine);
    ScopedValue v(scope);

    for (int i = 0; i < methodCount; ++i) {
        const QQmlPropertyData *attempt = methods + i;

        // QQmlV4Function overrides anything that doesn't provide the exact number of arguments
        int methodParameterScore = 1;
        // QQmlV4Function overrides the "no idea" option, which is 10
        int maxMethodMatchScore = 9;
        // QQmlV4Function cannot provide a best sum of match scores as we don't match the arguments
        int sumMethodMatchScore = bestSumMatchScore;

        if (!attempt->isV4Function()) {
            QQmlMetaObject::ArgTypeStorage storage;
            int methodArgumentCount = 0;
            if (attempt->hasArguments()) {
                if (attempt->isConstructor()) {
                    if (!object.constructorParameterTypes(attempt->coreIndex(), &storage, nullptr))
                        continue;
                } else {
                    if (!object.methodParameterTypes(attempt->coreIndex(), &storage, nullptr))
                        continue;
                }
                methodArgumentCount = storage.size();
            }

            if (methodArgumentCount > argumentCount)
                continue; // We don't have sufficient arguments to call this method

            methodParameterScore = (definedArgumentCount == methodArgumentCount)
                    ? 0
                    : (definedArgumentCount - methodArgumentCount + 1);
            if (methodParameterScore > bestParameterScore)
                continue; // We already have a better option

            maxMethodMatchScore = 0;
            sumMethodMatchScore = 0;
            for (int ii = 0; ii < methodArgumentCount; ++ii) {
                const int score = MatchScore((v = Value::fromStaticValue(callArgs->args[ii])),
                                             storage[ii]);
                maxMethodMatchScore = qMax(maxMethodMatchScore, score);
                sumMethodMatchScore += score;
            }
        }

        if (bestParameterScore > methodParameterScore || bestMaxMatchScore > maxMethodMatchScore
                || (bestParameterScore == methodParameterScore
                    && bestMaxMatchScore == maxMethodMatchScore
                    && bestSumMatchScore > sumMethodMatchScore)) {
            best = attempt;
            bestParameterScore = methodParameterScore;
            bestMaxMatchScore = maxMethodMatchScore;
            bestSumMatchScore = sumMethodMatchScore;
        }

        if (bestParameterScore == 0 && bestMaxMatchScore == 0)
            break; // We can't get better than that

    };

    if (best && best->isValid()) {
        return best;
    } else {
        QString error = QLatin1String("Unable to determine callable overload.  Candidates are:");
        for (int i = 0; i < methodCount; ++i) {
            for (int i = 0; i < methodCount; ++i) {
                const QQmlPropertyData &candidate = methods[i];
                const QMetaMethod m = candidate.isConstructor()
                                      ? object.metaObject()->constructor(candidate.coreIndex())
                                      : object.metaObject()->method(candidate.coreIndex());
                error += u"\n    " + QString::fromUtf8(m.methodSignature());
            }
        }

        engine->throwError(error);
        return nullptr;
    }
}



void CallArgument::cleanup()
{
    switch (type) {
    case QMetaType::QString:
        qstringPtr->~QString();
        break;
    case QMetaType::QByteArray:
        qbyteArrayPtr->~QByteArray();
        break;
    case QMetaType::QVariant:
    case QVariantWrappedType:
        qvariantPtr->~QVariant();
        break;
    case QMetaType::QJsonArray:
        jsonArrayPtr->~QJsonArray();
        break;
    case QMetaType::QJsonObject:
        jsonObjectPtr->~QJsonObject();
        break;
    case QMetaType::QJsonValue:
        jsonValuePtr->~QJsonValue();
        break;
    default:
        if (type == qMetaTypeId<QJSValue>()) {
            qjsValuePtr->~QJSValue();
            break;
        }

        if (type == qMetaTypeId<QList<QObject *> >()) {
            qlistPtr->~QList<QObject *>();
            break;
        }

        // The sequence types need no cleanup because we don't own them.

        break;
    }
}

void *CallArgument::dataPtr()
{
    switch (type) {
    case QMetaType::UnknownType:
        return nullptr;
    case QVariantWrappedType:
        return qvariantPtr->data();
    default:
        if (type == qMetaTypeId<std::vector<int>>())
            return stdVectorIntPtr;
        if (type == qMetaTypeId<std::vector<qreal>>())
            return stdVectorRealPtr;
        if (type == qMetaTypeId<std::vector<bool>>())
            return stdVectorBoolPtr;
        if (type == qMetaTypeId<std::vector<QString>>())
            return stdVectorQStringPtr;
        if (type == qMetaTypeId<std::vector<QUrl>>())
            return stdVectorQUrlPtr;
#if QT_CONFIG(qml_itemmodel)
        if (type == qMetaTypeId<std::vector<QModelIndex>>())
            return stdVectorQModelIndexPtr;
#endif
        break;
    }

    return (void *)&allocData;
}

void CallArgument::initAsType(QMetaType metaType)
{
    if (type != QMetaType::UnknownType)
        cleanup();

    type = metaType.id();
    switch (type) {
    case QMetaType::Void:
        type = QMetaType::UnknownType;
        break;
    case QMetaType::UnknownType:
    case QMetaType::Int:
    case QMetaType::UInt:
    case QMetaType::Bool:
    case QMetaType::Double:
    case QMetaType::Float:
        break;
    case QMetaType::QObjectStar:
        qobjectPtr = nullptr;
        break;
    case QMetaType::QString:
        qstringPtr = new (&allocData) QString();
        break;
    case QMetaType::QVariant:
        qvariantPtr = new (&allocData) QVariant();
        break;
    case QMetaType::QJsonArray:
        jsonArrayPtr = new (&allocData) QJsonArray();
        break;
    case QMetaType::QJsonObject:
        jsonObjectPtr = new (&allocData) QJsonObject();
        break;
    case QMetaType::QJsonValue:
        jsonValuePtr = new (&allocData) QJsonValue();
        break;
    default: {
        if (metaType == QMetaType::fromType<QJSValue>()) {
            qjsValuePtr = new (&allocData) QJSValue();
            break;
        }

        if (metaType == QMetaType::fromType<QList<QObject *>>()) {
            qlistPtr = new (&allocData) QList<QObject *>();
            break;
        }

        type = QVariantWrappedType;
        qvariantPtr = new (&allocData) QVariant(metaType, (void *)nullptr);
        break;
    }
    }
}

template <class T, class M>
bool CallArgument::fromContainerValue(const Value &value, M CallArgument::*member)
{
    const Object *object = value.as<Object>();
    if (object && object->isListType()) {
        if (T* ptr = static_cast<T *>(SequencePrototype::getRawContainerPtr(
                                          object, QMetaType(type)))) {
            (this->*member) = ptr;
            return true;
        }
    }
    (this->*member) = nullptr;
    return false;
}

bool CallArgument::fromValue(QMetaType metaType, ExecutionEngine *engine, const Value &value)
{
    if (type != QMetaType::UnknownType)
        cleanup();

    type = metaType.id();

    switch (type) {
    case QMetaType::Int:
        intValue = quint32(value.toInt32());
        return true;
    case QMetaType::UInt:
        intValue = quint32(value.toUInt32());
        return true;
    case QMetaType::Bool:
        boolValue = value.toBoolean();
        return true;
    case QMetaType::Double:
        doubleValue = double(value.toNumber());
        return true;
    case QMetaType::Float:
        floatValue = float(value.toNumber());
        return true;
    case QMetaType::QString:
        if (value.isNullOrUndefined())
            qstringPtr = new (&allocData) QString();
        else
            qstringPtr = new (&allocData) QString(value.toQStringNoThrow());
        return true;
    case QMetaType::QObjectStar:
        if (const QObjectWrapper *qobjectWrapper = value.as<QObjectWrapper>()) {
            qobjectPtr = qobjectWrapper->object();
            return true;
        }

        if (const QQmlTypeWrapper *qmlTypeWrapper = value.as<QQmlTypeWrapper>()) {
            if (qmlTypeWrapper->isSingleton()) {
                // Convert via QVariant below.
                // TODO: Can't we just do qobjectPtr = qmlTypeWrapper->object() instead?
                break;
            }

            // If this is a plain type wrapper without an instance,
            // then indeed it's an undefined parameter.
            // (although we might interpret that as const QMetaObject *).
            // TODO: But what if it's an attached object?
            type = QMetaType::UnknownType;
            return true;
        }

        qobjectPtr = nullptr;
        return value.isNullOrUndefined(); // null and undefined are nullptr
    case QMetaType::QVariant:
        qvariantPtr = new (&allocData) QVariant(engine->toVariant(value, QMetaType {}));
        return true;
    case QMetaType::QJsonArray: {
        Scope scope(engine);
        ScopedArrayObject a(scope, value);
        jsonArrayPtr = new (&allocData) QJsonArray(JsonObject::toJsonArray(a));
        return true;
    }
    case  QMetaType::QJsonObject: {
        Scope scope(engine);
        ScopedObject o(scope, value);
        jsonObjectPtr = new (&allocData) QJsonObject(JsonObject::toJsonObject(o));
        return true;
    }
    case QMetaType::QJsonValue:
        jsonValuePtr = new (&allocData) QJsonValue(JsonObject::toJsonValue(value));
        return true;
    case QMetaType::Void:
        type = QMetaType::UnknownType;
        // TODO: This only doesn't leak because a default constructed QVariant doesn't allocate.
        *qvariantPtr = QVariant();
        return true;
    default:
        if (type == qMetaTypeId<QJSValue>()) {
            qjsValuePtr = new (&allocData) QJSValue;
            QJSValuePrivate::setValue(qjsValuePtr, value.asReturnedValue());
            return true;
        }

        if (type == qMetaTypeId<QList<QObject*> >()) {
            qlistPtr = new (&allocData) QList<QObject *>();
            Scope scope(engine);
            ScopedArrayObject array(scope, value);
            if (array) {
                Scoped<QObjectWrapper> qobjectWrapper(scope);

                uint length = array->getLength();
                for (uint ii = 0; ii < length; ++ii)  {
                    QObject *o = nullptr;
                    qobjectWrapper = array->get(ii);
                    if (!!qobjectWrapper)
                        o = qobjectWrapper->object();
                    qlistPtr->append(o);
                }
                return true;
            }

            if (const QObjectWrapper *qobjectWrapper = value.as<QObjectWrapper>()) {
                qlistPtr->append(qobjectWrapper->object());
                return true;
            }

            qlistPtr->append(nullptr);
            return value.isNullOrUndefined();
        }

        if (metaType.flags() & (QMetaType::PointerToQObject | QMetaType::PointerToGadget)) {
            // You can assign null or undefined to any pointer. The result is a nullptr.
            if (value.isNullOrUndefined()) {
                qvariantPtr = new (&allocData) QVariant(metaType, nullptr);
                return true;
            }
            break;
        }

        if (type == qMetaTypeId<std::vector<int>>()) {
            if (fromContainerValue<std::vector<int>>(value, &CallArgument::stdVectorIntPtr))
                return true;
        } else if (type == qMetaTypeId<std::vector<qreal>>()) {
            if (fromContainerValue<std::vector<qreal>>(value, &CallArgument::stdVectorRealPtr))
                return true;
        } else if (type == qMetaTypeId<std::vector<bool>>()) {
            if (fromContainerValue<std::vector<bool>>(value, &CallArgument::stdVectorBoolPtr))
                return true;
        } else if (type == qMetaTypeId<std::vector<QString>>()) {
            if (fromContainerValue<std::vector<QString>>(value, &CallArgument::stdVectorQStringPtr))
                return true;
        } else if (type == qMetaTypeId<std::vector<QUrl>>()) {
            if (fromContainerValue<std::vector<QUrl>>(value, &CallArgument::stdVectorQUrlPtr))
                return true;
#if QT_CONFIG(qml_itemmodel)
        } else if (type == qMetaTypeId<std::vector<QModelIndex>>()) {
            if (fromContainerValue<std::vector<QModelIndex>>(
                        value, &CallArgument::stdVectorQModelIndexPtr)) {
                return true;
            }
#endif
        }
        break;
    }

    // Convert via QVariant through the QML engine.
    qvariantPtr = new (&allocData) QVariant();
    type = QVariantWrappedType;

    QVariant v = engine->toVariant(value, metaType);

    if (v.metaType() == metaType) {
        *qvariantPtr = std::move(v);
        return true;
    }

    if (v.canConvert(metaType)) {
        *qvariantPtr = std::move(v);
        qvariantPtr->convert(metaType);
        return true;
    }

    const QQmlMetaObject mo = QQmlMetaType::rawMetaObjectForType(metaType);
    if (!mo.isNull()) {
        QObject *obj = QQmlMetaType::toQObject(v);

        if (obj != nullptr && !QQmlMetaObject::canConvert(obj, mo)) {
            *qvariantPtr = QVariant(metaType, nullptr);
            return false;
        }

        *qvariantPtr = QVariant(metaType, &obj);
        return true;
    }

    *qvariantPtr = QVariant(metaType, (void *)nullptr);
    return false;
}

ReturnedValue CallArgument::toValue(ExecutionEngine *engine)
{
    switch (type) {
    case QMetaType::Int:
        return Encode(int(intValue));
    case QMetaType::UInt:
        return Encode((uint)intValue);
    case QMetaType::Bool:
        return Encode(boolValue);
    case QMetaType::Double:
        return Encode(doubleValue);
    case QMetaType::Float:
        return Encode(floatValue);
    case QMetaType::QString:
        return Encode(engine->newString(*qstringPtr));
    case QMetaType::QByteArray:
        return Encode(engine->newArrayBuffer(*qbyteArrayPtr));
    case QMetaType::QObjectStar:
        if (qobjectPtr)
            QQmlData::get(qobjectPtr, true)->setImplicitDestructible();
        return QObjectWrapper::wrap(engine, qobjectPtr);
    case QMetaType::QJsonArray:
        return JsonObject::fromJsonArray(engine, *jsonArrayPtr);
    case QMetaType::QJsonObject:
        return JsonObject::fromJsonObject(engine, *jsonObjectPtr);
    case QMetaType::QJsonValue:
        return JsonObject::fromJsonValue(engine, *jsonValuePtr);
    case QMetaType::QVariant:
    case QVariantWrappedType: {
        Scope scope(engine);
        ScopedValue rv(scope, scope.engine->fromVariant(*qvariantPtr));
        Scoped<QObjectWrapper> qobjectWrapper(scope, rv);
        if (!!qobjectWrapper) {
            if (QObject *object = qobjectWrapper->object())
                QQmlData::get(object, true)->setImplicitDestructible();
        }
        return rv->asReturnedValue();
    }
    default:
        break;
    }

    if (type == qMetaTypeId<QJSValue>()) {
        // The QJSValue can be passed around via dataPtr()
        QJSValuePrivate::manageStringOnV4Heap(engine, qjsValuePtr);
        return QJSValuePrivate::asReturnedValue(qjsValuePtr);
    }

    if (type == qMetaTypeId<QList<QObject *> >()) {
        // XXX Can this be made more by using Array as a prototype and implementing
        // directly against QList<QObject*>?
        QList<QObject *> &list = *qlistPtr;
        Scope scope(engine);
        ScopedArrayObject array(scope, engine->newArrayObject());
        array->arrayReserve(list.count());
        ScopedValue v(scope);
        for (int ii = 0; ii < list.count(); ++ii)
            array->arrayPut(ii, (v = QObjectWrapper::wrap(engine, list.at(ii))));
        array->setArrayLengthUnchecked(list.count());
        return array.asReturnedValue();
    }

    return Encode::undefined();
}

ReturnedValue QObjectMethod::create(ExecutionContext *scope, QObject *object, int index)
{
    Scope valueScope(scope);
    Scoped<QObjectMethod> method(valueScope, valueScope.engine->memoryManager->allocate<QObjectMethod>(scope));
    method->d()->setObject(object);

    method->d()->index = index;
    return method.asReturnedValue();
}

ReturnedValue QObjectMethod::create(ExecutionContext *scope, Heap::QQmlValueTypeWrapper *valueType, int index)
{
    Scope valueScope(scope);
    Scoped<QObjectMethod> method(valueScope, valueScope.engine->memoryManager->allocate<QObjectMethod>(scope));
    method->d()->index = index;
    method->d()->valueTypeWrapper.set(valueScope.engine, valueType);
    return method.asReturnedValue();
}

void Heap::QObjectMethod::init(QV4::ExecutionContext *scope)
{
    Heap::FunctionObject::init(scope);
}

const QMetaObject *Heap::QObjectMethod::metaObject()
{
    if (valueTypeWrapper)
        return valueTypeWrapper->metaObject();
    return object()->metaObject();
}

void Heap::QObjectMethod::ensureMethodsCache()
{
    if (methods)
        return;
    const QMetaObject *mo = metaObject();
    int methodOffset = mo->methodOffset();
    while (methodOffset > index) {
        mo = mo->superClass();
        methodOffset -= QMetaObjectPrivate::get(mo)->methodCount;
    }
    QVarLengthArray<QQmlPropertyData, 9> resolvedMethods;
    QQmlPropertyData dummy;
    QMetaMethod method = mo->method(index);
    dummy.load(method);
    resolvedMethods.append(dummy);
    // Look for overloaded methods
    QByteArray methodName = method.name();
    for (int ii = index - 1; ii >= methodOffset; --ii) {
        if (methodName == mo->method(ii).name()) {
            method = mo->method(ii);
            dummy.load(method);
            resolvedMethods.append(dummy);
        }
    }
    if (resolvedMethods.size() > 1) {
        methods = new QQmlPropertyData[resolvedMethods.size()];
        memcpy(methods, resolvedMethods.data(), resolvedMethods.size()*sizeof(QQmlPropertyData));
        methodCount = resolvedMethods.size();
    } else {
        methods = reinterpret_cast<QQmlPropertyData *>(&_singleMethod);
        *methods = resolvedMethods.at(0);
        methodCount = 1;
    }
}

ReturnedValue QObjectMethod::method_toString(ExecutionEngine *engine) const
{
    QString result;
    if (const QMetaObject *metaObject = d()->metaObject()) {

        result += QString::fromUtf8(metaObject->className()) +
                QLatin1String("(0x") + QString::number((quintptr)d()->object(),16);

        if (d()->object()) {
            QString objectName = d()->object()->objectName();
            if (!objectName.isEmpty())
                result += QLatin1String(", \"") + objectName + QLatin1Char('\"');
        }

        result += QLatin1Char(')');
    } else {
        result = QLatin1String("null");
    }

    return engine->newString(result)->asReturnedValue();
}

ReturnedValue QObjectMethod::method_destroy(ExecutionEngine *engine, const Value *args, int argc) const
{
    if (!d()->object())
        return Encode::undefined();
    if (QQmlData::keepAliveDuringGarbageCollection(d()->object()))
        return engine->throwError(QStringLiteral("Invalid attempt to destroy() an indestructible object"));

    int delay = 0;
    if (argc > 0)
        delay = args[0].toUInt32();

    if (delay > 0)
        QTimer::singleShot(delay, d()->object(), SLOT(deleteLater()));
    else
        d()->object()->deleteLater();

    return Encode::undefined();
}

ReturnedValue QObjectMethod::virtualCall(const FunctionObject *m, const Value *thisObject, const Value *argv, int argc)
{
    const QObjectMethod *This = static_cast<const QObjectMethod*>(m);
    return This->callInternal(thisObject, argv, argc);
}

ReturnedValue QObjectMethod::callInternal(const Value *thisObject, const Value *argv, int argc) const
{
    ExecutionEngine *v4 = engine();
    if (d()->index == DestroyMethod)
        return method_destroy(v4, argv, argc);
    else if (d()->index == ToStringMethod)
        return method_toString(v4);

    d()->ensureMethodsCache();

    Scope scope(v4);
    QQmlObjectOrGadget object(d()->object());

    if (!d()->object()) {
        if (!d()->valueTypeWrapper)
            return Encode::undefined();

        object = QQmlObjectOrGadget(d()->metaObject(), d()->valueTypeWrapper->gadgetPtr());
    }

    JSCallData cData(thisObject, argv, argc);
    CallData *callData = cData.callData(scope);

    const QQmlPropertyData *method = d()->methods;

    // If we call the method, we have to write back any value type references afterwards.
    // The method might change the value.
    const auto doCall = [&](const auto &call) {
        if (!method->isConstant()) {
            Scoped<QQmlValueTypeReference> valueTypeReference(
                        scope, d()->valueTypeWrapper.get());
            if (valueTypeReference) {
                ScopedValue rv(scope, call());
                valueTypeReference->d()->writeBack();
                return rv->asReturnedValue();
            }
        }

        return call();
    };

    if (d()->methodCount != 1) {
        method = ResolveOverloaded(object, d()->methods, d()->methodCount, v4, callData);
        if (method == nullptr)
            return Encode::undefined();
    }

    if (method->isV4Function()) {
        return doCall([&]() {
            ScopedValue rv(scope, Value::undefinedValue());
            QQmlV4Function func(callData, rv, v4);
            QQmlV4Function *funcptr = &func;

            void *args[] = { nullptr, &funcptr };
            object.metacall(QMetaObject::InvokeMetaMethod, method->coreIndex(), args);

            return rv->asReturnedValue();
        });
    }

    return doCall([&]() { return CallPrecise(object, *method, v4, callData); });
}

DEFINE_OBJECT_VTABLE(QObjectMethod);


void Heap::QMetaObjectWrapper::init(const QMetaObject *metaObject)
{
    FunctionObject::init();
    this->metaObject = metaObject;
    constructors = nullptr;
    constructorCount = 0;
}

void Heap::QMetaObjectWrapper::destroy()
{
    delete[] constructors;
}

void Heap::QMetaObjectWrapper::ensureConstructorsCache() {

    const int count = metaObject->constructorCount();
    if (constructorCount != count) {
        delete[] constructors;
        constructorCount = count;
        if (count == 0) {
            constructors = nullptr;
            return;
        }
        constructors = new QQmlPropertyData[count];

        for (int i = 0; i < count; ++i) {
            QMetaMethod method = metaObject->constructor(i);
            QQmlPropertyData &d = constructors[i];
            d.load(method);
            d.setCoreIndex(i);
        }
    }
}


ReturnedValue QMetaObjectWrapper::create(ExecutionEngine *engine, const QMetaObject* metaObject) {

     Scope scope(engine);
     Scoped<QMetaObjectWrapper> mo(scope, engine->memoryManager->allocate<QMetaObjectWrapper>(metaObject)->asReturnedValue());
     mo->init(engine);
     return mo->asReturnedValue();
}

void QMetaObjectWrapper::init(ExecutionEngine *) {
    const QMetaObject & mo = *d()->metaObject;

    for (int i = 0; i < mo.enumeratorCount(); i++) {
        QMetaEnum Enum = mo.enumerator(i);
        for (int k = 0; k < Enum.keyCount(); k++) {
            const char* key = Enum.key(k);
            const int value = Enum.value(k);
            defineReadonlyProperty(QLatin1String(key), Value::fromInt32(value));
        }
    }
}

ReturnedValue QMetaObjectWrapper::virtualCallAsConstructor(const FunctionObject *f, const Value *argv, int argc, const Value *)
{
    const QMetaObjectWrapper *This = static_cast<const QMetaObjectWrapper*>(f);
    return This->constructInternal(argv, argc);
}

ReturnedValue QMetaObjectWrapper::constructInternal(const Value *argv, int argc) const
{

    d()->ensureConstructorsCache();

    ExecutionEngine *v4 = engine();
    const QMetaObject* mo = d()->metaObject;
    if (d()->constructorCount == 0) {
        return v4->throwTypeError(QLatin1String(mo->className())
                                  + QLatin1String(" has no invokable constructor"));
    }

    Scope scope(v4);
    Scoped<QObjectWrapper> object(scope);
    JSCallData cData(nullptr, argv, argc);
    CallData *callData = cData.callData(scope);

    const QQmlObjectOrGadget objectOrGadget(mo);

    if (d()->constructorCount == 1) {
        object = CallPrecise(objectOrGadget, d()->constructors[0], v4, callData, QMetaObject::CreateInstance);
    } else if (const QQmlPropertyData *ctor = ResolveOverloaded(
                    objectOrGadget, d()->constructors, d()->constructorCount, v4, callData)) {
        object = CallPrecise(objectOrGadget, *ctor, v4, callData, QMetaObject::CreateInstance);
    }
    Scoped<QMetaObjectWrapper> metaObject(scope, this);
    object->defineDefaultProperty(v4->id_constructor(), metaObject);
    object->setPrototypeOf(const_cast<QMetaObjectWrapper*>(this));
    return object.asReturnedValue();

}

bool QMetaObjectWrapper::virtualIsEqualTo(Managed *a, Managed *b)
{
    Q_ASSERT(a->as<QMetaObjectWrapper>());
    QMetaObjectWrapper *aMetaObject = a->as<QMetaObjectWrapper>();
    QMetaObjectWrapper *bMetaObject = b->as<QMetaObjectWrapper>();
    if (!bMetaObject)
        return true;
    return aMetaObject->metaObject() == bMetaObject->metaObject();
}

DEFINE_OBJECT_VTABLE(QMetaObjectWrapper);




void Heap::QmlSignalHandler::init(QObject *object, int signalIndex)
{
    Object::init();
    this->signalIndex = signalIndex;
    setObject(object);
}

DEFINE_OBJECT_VTABLE(QmlSignalHandler);

void QmlSignalHandler::initProto(ExecutionEngine *engine)
{
    if (engine->signalHandlerPrototype()->d_unchecked())
        return;

    Scope scope(engine);
    ScopedObject o(scope, engine->newObject());
    ScopedString connect(scope, engine->newIdentifier(QStringLiteral("connect")));
    ScopedString disconnect(scope, engine->newIdentifier(QStringLiteral("disconnect")));
    o->put(connect, ScopedValue(scope, engine->functionPrototype()->get(connect)));
    o->put(disconnect, ScopedValue(scope, engine->functionPrototype()->get(disconnect)));

    engine->jsObjects[ExecutionEngine::SignalHandlerProto] = o->d();
}

void MultiplyWrappedQObjectMap::insert(QObject *key, Heap::Object *value)
{
    QHash<QObject*, WeakValue>::operator[](key).set(value->internalClass->engine, value);
    connect(key, SIGNAL(destroyed(QObject*)), this, SLOT(removeDestroyedObject(QObject*)));
}



MultiplyWrappedQObjectMap::Iterator MultiplyWrappedQObjectMap::erase(MultiplyWrappedQObjectMap::Iterator it)
{
    disconnect(it.key(), SIGNAL(destroyed(QObject*)), this, SLOT(removeDestroyedObject(QObject*)));
    return QHash<QObject*, WeakValue>::erase(it);
}

void MultiplyWrappedQObjectMap::remove(QObject *key)
{
    Iterator it = find(key);
    if (it == end())
        return;
    erase(it);
}

void MultiplyWrappedQObjectMap::mark(QObject *key, MarkStack *markStack)
{
    Iterator it = find(key);
    if (it == end())
        return;
    it->markOnce(markStack);
}

void MultiplyWrappedQObjectMap::removeDestroyedObject(QObject *object)
{
    QHash<QObject*, WeakValue>::remove(object);
}

} // namespace QV4

QT_END_NAMESPACE

#include "moc_qv4qobjectwrapper_p.cpp"
