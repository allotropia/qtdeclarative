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

#include "qv4qmlcontext_p.h"

#include <private/qjsvalue_p.h>
#include <private/qqmlcontext_p.h>
#include <private/qqmlengine_p.h>
#include <private/qqmlglobal_p.h>
#include <private/qqmljavascriptexpression_p.h>
#include <private/qqmllistwrapper_p.h>
#include <private/qqmltypewrapper_p.h>
#include <private/qv4compileddata_p.h>
#include <private/qv4engine_p.h>
#include <private/qv4function_p.h>
#include <private/qv4identifiertable_p.h>
#include <private/qv4lookup_p.h>
#include <private/qv4mm_p.h>
#include <private/qv4module_p.h>
#include <private/qv4objectproto_p.h>
#include <private/qv4qobjectwrapper_p.h>
#include <private/qv4value_p.h>

#include <QtCore/qloggingcategory.h>

QT_BEGIN_NAMESPACE

Q_LOGGING_CATEGORY(lcQmlContext, "qt.qml.context");

using namespace QV4;

DEFINE_OBJECT_VTABLE(QQmlContextWrapper);
DEFINE_MANAGED_VTABLE(QmlContext);

void Heap::QQmlContextWrapper::init(QQmlRefPointer<QQmlContextData> context, QObject *scopeObject)
{
    Object::init();
    this->context = context.take();
    this->scopeObject.init(scopeObject);
}

void Heap::QQmlContextWrapper::destroy()
{
    context->release();
    context = nullptr;
    scopeObject.destroy();
    Object::destroy();
}

static OptionalReturnedValue searchContextProperties(
        QV4::ExecutionEngine *v4, const QQmlRefPointer<QQmlContextData> &context, String *name,
        bool *hasProperty, Value *base, QV4::Lookup *lookup, QV4::Lookup *originalLookup,
        QQmlEnginePrivate *ep)
{
    const int propertyIdx = context->propertyIndex(name);

    if (propertyIdx == -1)
        return OptionalReturnedValue();

    if (propertyIdx < context->numIdValues()) {
        if (hasProperty)
            *hasProperty = true;

        if (lookup) {
            lookup->qmlContextIdObjectLookup.objectId = propertyIdx;
            lookup->qmlContextPropertyGetter = QQmlContextWrapper::lookupIdObject;
            return OptionalReturnedValue(lookup->qmlContextPropertyGetter(lookup, v4, base));
        } else if (originalLookup) {
            originalLookup->qmlContextPropertyGetter = QQmlContextWrapper::lookupInParentContextHierarchy;
        }

        if (ep->propertyCapture)
            ep->propertyCapture->captureProperty(context->idValueBindings(propertyIdx));
        return OptionalReturnedValue(QV4::QObjectWrapper::wrap(v4, context->idValue(propertyIdx)));
    }

    QQmlContextPrivate *cp = context->asQQmlContextPrivate();

    if (ep->propertyCapture)
        ep->propertyCapture->captureProperty(context->asQQmlContext(), -1, propertyIdx + cp->notifyIndex());

    const QVariant &value = cp->propertyValue(propertyIdx);
    if (hasProperty)
        *hasProperty = true;
    if (value.userType() == qMetaTypeId<QList<QObject*> >()) {
        QQmlListProperty<QObject> prop(context->asQQmlContext(), (void*) qintptr(propertyIdx),
                                       QQmlContextPrivate::context_count,
                                       QQmlContextPrivate::context_at);
        return OptionalReturnedValue(QmlListWrapper::create(v4, prop, QMetaType::fromType<QQmlListProperty<QObject> >()));
    }
    return OptionalReturnedValue(v4->fromVariant(cp->propertyValue(propertyIdx)));
}

ReturnedValue QQmlContextWrapper::getPropertyAndBase(const QQmlContextWrapper *resource, PropertyKey id, const Value *receiver, bool *hasProperty, Value *base, Lookup *lookup)
{
    if (!id.isString())
        return Object::virtualGet(resource, id, receiver, hasProperty);

    QV4::ExecutionEngine *v4 = resource->engine();
    QV4::Scope scope(v4);

    if (v4->callingQmlContext().data() != resource->d()->context) {
        if (resource->d()->module) {
            Scoped<Module> module(scope, resource->d()->module);
            bool hasProp = false;
            ScopedValue value(scope, module->get(id, receiver, &hasProp));
            if (hasProp) {
                if (hasProperty)
                    *hasProperty = hasProp;
                return value->asReturnedValue();
            }
        }

        return Object::virtualGet(resource, id, receiver, hasProperty);
    }

    bool hasProp = false;
    ScopedValue result(scope, Object::virtualGet(resource, id, receiver, &hasProp));
    if (hasProp) {
        if (hasProperty)
            *hasProperty = hasProp;
        return result->asReturnedValue();
    }

    // It's possible we could delay the calculation of the "actual" context (in the case
    // of sub contexts) until it is definitely needed.
    QQmlRefPointer<QQmlContextData> context = resource->getContext();
    QQmlRefPointer<QQmlContextData> expressionContext = context;

    if (!context) {
        if (hasProperty)
            *hasProperty = true;
        return result->asReturnedValue();
    }

    // Search type (attached property/enum/imported scripts) names
    // while (context) {
    //     Search context properties
    //     Search scope object
    //     Search context object
    //     context = context->parent
    // }

    QObject *scopeObject = resource->getScopeObject();

    ScopedString name(scope, id.asStringOrSymbol());

    const auto performGobalLookUp = [&result, v4, &name, hasProperty]() {
        bool hasProp = false;
        result = v4->globalObject->get(name, &hasProp);
        if (hasProp) {
            if (hasProperty)
                *hasProperty = hasProp;
            return true;
        }
        return false;
    };

    // If the scope object is a QAbstractDynamicMetaObject, then QMetaObject::indexOfProperty
    // will call createProperty() on the QADMO and implicitly create the property. While that
    // is questionable behavior, there are two use-cases that we support in the light of this:
    //
    //    (1) The implicit creation of properties is necessary because it will also result in
    //        a recorded capture, which will allow a re-evaluation of bindings when the value
    //        is populated later. See QTBUG-35233 and the test-case in tst_qqmlpropertymap.
    //
    //    (1) Looking up "console" in order to place a console.log() call for example must
    //        find the console instead of creating a new property. Therefore we prioritize the
    //        lookup in the global object here.
    //
    // Note: The scope object is only a QADMO for example when somebody registers a QQmlPropertyMap
    // sub-class as QML type and then instantiates it in .qml.
    const QMetaObjectPrivate *metaObjectPrivate = scopeObject
            ? reinterpret_cast<const QMetaObjectPrivate *>(scopeObject->metaObject()->d.data)
            : nullptr;
    if (metaObjectPrivate && metaObjectPrivate->flags & DynamicMetaObject) {
        // all bets are off, so don't try to optimize any lookups
        lookup = nullptr;
        if (performGobalLookUp())
            return result->asReturnedValue();
    }

    if (context->imports() && name->startsWithUpper()) {
        // Search for attached properties, enums and imported scripts
        QQmlTypeNameCache::Result r = context->imports()->query<QQmlImport::AllowRecursion>(name);

        if (r.isValid()) {
            if (hasProperty)
                *hasProperty = true;
            if (r.scriptIndex != -1) {
                if (lookup) {
                    lookup->qmlContextScriptLookup.scriptIndex = r.scriptIndex;
                    lookup->qmlContextPropertyGetter = QQmlContextWrapper::lookupScript;
                    return lookup->qmlContextPropertyGetter(lookup, v4, base);
                }
                QV4::ScopedObject scripts(scope, context->importedScripts().valueRef());
                if (scripts)
                    return scripts->get(r.scriptIndex);
                return QV4::Encode::null();
            } else if (r.type.isValid()) {
                if (lookup) {
                    bool isValueSingleton = false;
                    if (r.type.isSingleton()) {
                        QQmlEnginePrivate *e = QQmlEnginePrivate::get(v4->qmlEngine());
                        if (r.type.isQObjectSingleton() || r.type.isCompositeSingleton()) {
                            e->singletonInstance<QObject*>(r.type);
                            lookup->qmlContextSingletonLookup.singletonObject =
                                        Value::fromReturnedValue(
                                            QQmlTypeWrapper::create(v4, nullptr, r.type)
                                        ).heapObject();
                        } else {
                            QJSValue singleton = e->singletonInstance<QJSValue>(r.type);

                            // QSrting values should already have been put on the engine heap at this point
                            // to manage their memory. We later assume this has already happened.
                            Q_ASSERT(!QJSValuePrivate::asQString(&singleton));

                            if (QV4::Value *val = QJSValuePrivate::takeManagedValue(&singleton)) {
                                lookup->qmlContextSingletonLookup.singletonObject = val->heapObject();
                            } else {
                                lookup->qmlContextSingletonLookup.singletonValue = QJSValuePrivate::asReturnedValue(&singleton);
                                isValueSingleton = true;
                            }
                        }
                        lookup->qmlContextPropertyGetter = isValueSingleton ? QQmlContextWrapper::lookupValueSingleton
                                                                            : QQmlContextWrapper::lookupSingleton;
                        return lookup->qmlContextPropertyGetter(lookup, v4, base);
                    }
                }
                result = QQmlTypeWrapper::create(v4, scopeObject, r.type);
            } else if (r.importNamespace) {
                result = QQmlTypeWrapper::create(v4, scopeObject, context->imports(), r.importNamespace);
            }
            if (lookup) {
                lookup->qmlTypeLookup.qmlTypeWrapper = static_cast<Heap::Object*>(result->heapObject());
                lookup->qmlContextPropertyGetter = QQmlContextWrapper::lookupType;
            }
            return result->asReturnedValue();
        }

        // Fall through
    }

    QQmlEnginePrivate *ep = QQmlEnginePrivate::get(v4->qmlEngine());
    Lookup * const originalLookup = lookup;

    decltype(lookup->qmlContextPropertyGetter) contextGetterFunction = QQmlContextWrapper::lookupContextObjectProperty;

    // minor optimization so we don't potentially try two property lookups on the same object
    if (scopeObject == context->contextObject()) {
        scopeObject = nullptr;
        contextGetterFunction = QQmlContextWrapper::lookupScopeObjectProperty;
    }

    while (context) {
        if (auto property = searchContextProperties(v4, context, name, hasProperty, base, lookup, originalLookup, ep))
            return *property;

        // Search scope object
        if (scopeObject) {
            bool hasProp = false;

            QQmlPropertyData *propertyData = nullptr;
            QV4::ScopedValue result(scope, QV4::QObjectWrapper::getQmlProperty(v4, context, scopeObject,
                                                                               name, QV4::QObjectWrapper::CheckRevision, &hasProp, &propertyData));
            if (hasProp) {
                if (hasProperty)
                    *hasProperty = true;
                if (base)
                    *base = QV4::QObjectWrapper::wrap(v4, scopeObject);

                if (lookup && propertyData) {
                    QQmlData *ddata = QQmlData::get(scopeObject, false);
                    if (ddata && ddata->propertyCache) {
                        ScopedValue val(scope, base ? *base : Value::fromReturnedValue(QV4::QObjectWrapper::wrap(v4, scopeObject)));
                        QV4::setupQObjectLookup(lookup, ddata, propertyData, val->objectValue());
                        lookup->qmlContextPropertyGetter = QQmlContextWrapper::lookupScopeObjectProperty;
                    }
                }

                return result->asReturnedValue();
            }
        }
        scopeObject = nullptr;


        // Search context object
        if (QObject *contextObject = context->contextObject()) {
            bool hasProp = false;
            QQmlPropertyData *propertyData = nullptr;
            result = QV4::QObjectWrapper::getQmlProperty(v4, context, contextObject,
                                                         name, QV4::QObjectWrapper::CheckRevision,
                                                         &hasProp, &propertyData);
            if (hasProp) {
                if (hasProperty)
                    *hasProperty = true;
                if (base)
                    *base = QV4::QObjectWrapper::wrap(v4, contextObject);

                if (propertyData) {
                    if (lookup) {
                        QQmlData *ddata = QQmlData::get(contextObject, false);
                        if (ddata && ddata->propertyCache
                                && lookup->qmlContextPropertyGetter != contextGetterFunction) {
                            ScopedValue val(scope, base ? *base
                                                        : Value::fromReturnedValue(QV4::QObjectWrapper::wrap(v4, contextObject)));
                            QV4::setupQObjectLookup(lookup, ddata, propertyData,
                                                    val->objectValue());
                            lookup->qmlContextPropertyGetter = contextGetterFunction;
                        }
                    } else if (originalLookup) {
                        originalLookup->qmlContextPropertyGetter = lookupInParentContextHierarchy;
                    }
                }

                return result->asReturnedValue();
            }
        }

        context = context->parent();

        // As the hierarchy of contexts is not stable, we can't do accelerated lookups beyond
        // the immediate QML context (of the .qml file).
        lookup = nullptr;
    }

    // Do a lookup in the global object here to avoid expressionContext->unresolvedNames becoming
    // true if we access properties of the global object.
    if (originalLookup) {
        // Try a lookup in the global object. It's theoretically possible to first find a property
        // in the global object and then later a context property with the same name is added, but that
        // never really worked as we used to detect access to global properties at type compile time anyway.
        lookup = originalLookup;
        result = lookup->resolveGlobalGetter(v4);
        if (lookup->globalGetter != Lookup::globalGetterGeneric) {
            if (hasProperty)
                *hasProperty = true;
            lookup->qmlContextGlobalLookup.getterTrampoline = lookup->globalGetter;
            lookup->qmlContextPropertyGetter = QQmlContextWrapper::lookupInGlobalObject;
            return result->asReturnedValue();
        }
        lookup->qmlContextPropertyGetter = QQmlContextWrapper::resolveQmlContextPropertyLookupGetter;
    } else {
        if (performGobalLookUp())
            return result->asReturnedValue();
    }

    expressionContext->setUnresolvedNames(true);

    return Encode::undefined();
}

ReturnedValue QQmlContextWrapper::virtualGet(const Managed *m, PropertyKey id, const Value *receiver, bool *hasProperty)
{
    Q_ASSERT(m->as<QQmlContextWrapper>());
    const QQmlContextWrapper *This = static_cast<const QQmlContextWrapper *>(m);
    return getPropertyAndBase(This, id, receiver, hasProperty, /*base*/nullptr);
}

bool QQmlContextWrapper::virtualPut(Managed *m, PropertyKey id, const Value &value, Value *receiver)
{
    Q_ASSERT(m->as<QQmlContextWrapper>());

    if (id.isSymbol() || id.isArrayIndex())
        return Object::virtualPut(m, id, value, receiver);

    QQmlContextWrapper *resource = static_cast<QQmlContextWrapper *>(m);
    ExecutionEngine *v4 = resource->engine();
    QV4::Scope scope(v4);
    if (scope.hasException())
        return false;
    QV4::Scoped<QQmlContextWrapper> wrapper(scope, resource);

    auto member = wrapper->internalClass()->findValueOrSetter(id);
    if (member.index < UINT_MAX)
        return wrapper->putValue(member.index, member.attrs, value);

    // It's possible we could delay the calculation of the "actual" context (in the case
    // of sub contexts) until it is definitely needed.
    QQmlRefPointer<QQmlContextData> context = wrapper->getContext();
    QQmlRefPointer<QQmlContextData> expressionContext = context;

    if (!context)
        return false;

    // See QV8ContextWrapper::Getter for resolution order

    QObject *scopeObject = wrapper->getScopeObject();
    ScopedString name(scope, id.asStringOrSymbol());

    while (context) {
        // Search context properties
        if (const int propertyIndex = context->propertyIndex(name); propertyIndex != -1) {
            if (propertyIndex < context->numIdValues()) {
                v4->throwError(QLatin1String("left-hand side of assignment operator is not an lvalue"));
                return false;
            }
            return false;
        }

        // Search scope object
        if (scopeObject &&
            QV4::QObjectWrapper::setQmlProperty(v4, context, scopeObject, name, QV4::QObjectWrapper::CheckRevision, value))
            return true;
        scopeObject = nullptr;

        // Search context object
        if (context->contextObject() &&
            QV4::QObjectWrapper::setQmlProperty(v4, context, context->contextObject(), name,
                                                QV4::QObjectWrapper::CheckRevision, value))
            return true;

        context = context->parent();
    }

    expressionContext->setUnresolvedNames(true);

    QString error = QLatin1String("Invalid write to global property \"") + name->toQString() +
            QLatin1Char('"');
    v4->throwError(error);
    return false;
}

ReturnedValue QQmlContextWrapper::resolveQmlContextPropertyLookupGetter(Lookup *l, ExecutionEngine *engine, Value *base)
{
    Scope scope(engine);
    auto *func = engine->currentStackFrame->v4Function;
    PropertyKey name =engine->identifierTable->asPropertyKey(
                func->compilationUnit->runtimeStrings[l->nameIndex]);

    // Special hack for bounded signal expressions, where the parameters of signals are injected
    // into the handler expression through the locals of the call context. So for onClicked: { ... }
    // the parameters of the clicked signal are injected and we must allow for them to be found here
    // before any other property from the QML context.
    for (Heap::ExecutionContext *ctx = engine->currentContext()->d(); ctx; ctx = ctx->outer) {
        if (ctx->type == Heap::ExecutionContext::Type_CallContext) {
            const uint index = ctx->internalClass->indexOfValueOrGetter(name);
            if (index < std::numeric_limits<uint>::max()) {
                if (!func->detectedInjectedParameters) {
                    const auto location = func->sourceLocation();
                    qCWarning(lcQmlContext).nospace().noquote()
                            << location.sourceFile << ":" << location.line << ":" << location.column
                            << " Parameter \"" << name.toQString() << "\" is not declared."
                            << " Injection of parameters into signal handlers is deprecated."
                            << " Use JavaScript functions with formal parameters instead.";

                    // Don't warn over and over for the same function
                    func->detectedInjectedParameters = true;
                }

                return static_cast<Heap::CallContext *>(ctx)->locals[index].asReturnedValue();
            }
        }

        // Skip only block and call contexts.
        // Other contexts need a regular QML property lookup. See below.
        if (ctx->type != Heap::ExecutionContext::Type_BlockContext && ctx->type != Heap::ExecutionContext::Type_CallContext)
            break;
    }

    bool hasProperty = false;
    ScopedValue result(scope);

    Scoped<QmlContext> callingQmlContext(scope, engine->qmlContext());
    if (callingQmlContext) {
        Scoped<QQmlContextWrapper> qmlContextWrapper(scope, callingQmlContext->d()->qml());
        result = QQmlContextWrapper::getPropertyAndBase(qmlContextWrapper, name, /*receiver*/nullptr, &hasProperty,
                                                        base, l);
    } else {
        // Code path typical to worker scripts, compiled with lookups but no qml context.
        result = l->resolveGlobalGetter(engine);
        if (l->globalGetter != Lookup::globalGetterGeneric) {
            hasProperty = true;
            l->qmlContextGlobalLookup.getterTrampoline = l->globalGetter;
            l->qmlContextPropertyGetter = QQmlContextWrapper::lookupInGlobalObject;
        }
    }
    if (!hasProperty)
        return engine->throwReferenceError(name.toQString());
    return result->asReturnedValue();
}

ReturnedValue QQmlContextWrapper::lookupScript(Lookup *l, ExecutionEngine *engine, Value *base)
{
    Q_UNUSED(base);
    Scope scope(engine);
    Scoped<QmlContext> qmlContext(scope, engine->qmlContext());
    if (!qmlContext)
        return QV4::Encode::null();

    QQmlRefPointer<QQmlContextData> context = qmlContext->qmlContext();
    if (!context)
        return QV4::Encode::null();

    QV4::ScopedObject scripts(scope, context->importedScripts().valueRef());
    if (!scripts)
        return QV4::Encode::null();
    return scripts->get(l->qmlContextScriptLookup.scriptIndex);
}

ReturnedValue QQmlContextWrapper::lookupSingleton(Lookup *l, ExecutionEngine *engine, Value *base)
{
    Q_UNUSED(engine);
    Q_UNUSED(base);

    return l->qmlContextSingletonLookup.singletonObject->asReturnedValue();
}

ReturnedValue QQmlContextWrapper::lookupValueSingleton(Lookup *l, ExecutionEngine *engine, Value *base)
{
    Q_UNUSED(engine);
    Q_UNUSED(base);

    Q_ASSERT(l->qmlContextSingletonLookup.singletonObject == nullptr);
    return l->qmlContextSingletonLookup.singletonValue;
}

ReturnedValue QQmlContextWrapper::lookupIdObject(Lookup *l, ExecutionEngine *engine, Value *base)
{
    Q_UNUSED(base);
    Scope scope(engine);
    Scoped<QmlContext> qmlContext(scope, engine->qmlContext());
    if (!qmlContext)
        return QV4::Encode::null();

    QQmlRefPointer<QQmlContextData> context = qmlContext->qmlContext();
    if (!context)
        return QV4::Encode::null();

    QQmlEnginePrivate *qmlEngine = QQmlEnginePrivate::get(engine->qmlEngine());
    const int objectId = l->qmlContextIdObjectLookup.objectId;

    if (qmlEngine->propertyCapture)
        qmlEngine->propertyCapture->captureProperty(context->idValueBindings(objectId));

    return QV4::QObjectWrapper::wrap(engine, context->idValue(objectId));
}

ReturnedValue QQmlContextWrapper::lookupIdObjectInParentContext(
        Lookup *l, ExecutionEngine *engine, Value *base)
{
    return QQmlContextWrapper::resolveQmlContextPropertyLookupGetter(l, engine, base);
}

ReturnedValue QQmlContextWrapper::lookupScopeObjectProperty(Lookup *l, ExecutionEngine *engine, Value *base)
{
    Scope scope(engine);
    Scoped<QmlContext> qmlContext(scope, engine->qmlContext());
    if (!qmlContext)
        return QV4::Encode::undefined();

    QObject *scopeObject = qmlContext->qmlScope();
    if (!scopeObject)
        return QV4::Encode::undefined();

    if (QQmlData::wasDeleted(scopeObject))
        return QV4::Encode::undefined();

    const auto revertLookup = [l, engine, base]() {
        l->qobjectLookup.propertyCache->release();
        l->qobjectLookup.propertyCache = nullptr;
        l->qmlContextPropertyGetter = QQmlContextWrapper::resolveQmlContextPropertyLookupGetter;
        return QQmlContextWrapper::resolveQmlContextPropertyLookupGetter(l, engine, base);
    };

    ScopedValue obj(scope, QV4::QObjectWrapper::wrap(engine, scopeObject));

    if (base)
        *base = obj;

    return QObjectWrapper::lookupGetterImpl(l, engine, obj, /*useOriginalProperty*/ true, revertLookup);
}

ReturnedValue QQmlContextWrapper::lookupContextObjectProperty(Lookup *l, ExecutionEngine *engine, Value *base)
{
    Scope scope(engine);
    Scoped<QmlContext> qmlContext(scope, engine->qmlContext());
    if (!qmlContext)
        return QV4::Encode::undefined();

    QQmlRefPointer<QQmlContextData> context = qmlContext->qmlContext();
    if (!context)
        return QV4::Encode::undefined();

    QObject *contextObject = context->contextObject();
    if (!contextObject)
        return QV4::Encode::undefined();

    if (QQmlData::wasDeleted(contextObject))
        return QV4::Encode::undefined();

    const auto revertLookup = [l, engine, base]() {
        l->qobjectLookup.propertyCache->release();
        l->qobjectLookup.propertyCache = nullptr;
        l->qmlContextPropertyGetter = QQmlContextWrapper::resolveQmlContextPropertyLookupGetter;
        return QQmlContextWrapper::resolveQmlContextPropertyLookupGetter(l, engine, base);
    };

    ScopedValue obj(scope, QV4::QObjectWrapper::wrap(engine, contextObject));

    if (base)
        *base = obj;

    return QObjectWrapper::lookupGetterImpl(l, engine, obj, /*useOriginalProperty*/ true, revertLookup);
}

ReturnedValue QQmlContextWrapper::lookupInGlobalObject(Lookup *l, ExecutionEngine *engine, Value *base)
{
    Q_UNUSED(base);
    ReturnedValue result = l->qmlContextGlobalLookup.getterTrampoline(l, engine);
    // In the unlikely event of mutation of the global object, update the trampoline.
    if (l->qmlContextPropertyGetter != lookupInGlobalObject) {
        l->qmlContextGlobalLookup.getterTrampoline = l->globalGetter;
        l->qmlContextPropertyGetter = QQmlContextWrapper::lookupInGlobalObject;
    }
    return result;
}

ReturnedValue QQmlContextWrapper::lookupInParentContextHierarchy(Lookup *l, ExecutionEngine *engine, Value *base)
{
    Scope scope(engine);
    Scoped<QmlContext> qmlContext(scope, engine->qmlContext());
    if (!qmlContext)
        return QV4::Encode::undefined();

    QQmlRefPointer<QQmlContextData> context = qmlContext->qmlContext();
    if (!context)
        return QV4::Encode::undefined();

    QQmlRefPointer<QQmlContextData> expressionContext = context;

    QQmlEnginePrivate *ep = QQmlEnginePrivate::get(engine->qmlEngine());

    PropertyKey id =engine->identifierTable->asPropertyKey(engine->currentStackFrame->v4Function->compilationUnit->
                                                           runtimeStrings[l->nameIndex]);
    ScopedString name(scope, id.asStringOrSymbol());

    ScopedValue result(scope);

    for (context = context->parent(); context; context = context->parent()) {
        if (auto property = searchContextProperties(engine, context, name, nullptr, base, nullptr, nullptr, ep))
            return *property;

        // Search context object
        if (QObject *contextObject = context->contextObject()) {
            bool hasProp = false;
            result = QV4::QObjectWrapper::getQmlProperty(
                        engine, context, contextObject, name,
                        QV4::QObjectWrapper::CheckRevision, &hasProp);
            if (hasProp) {
                if (base)
                    *base = QV4::QObjectWrapper::wrap(engine, contextObject);

                return result->asReturnedValue();
            }
        }
    }

    bool hasProp = false;
    result = engine->globalObject->get(name, &hasProp);
    if (hasProp)
        return result->asReturnedValue();

    expressionContext->setUnresolvedNames(true);

    return Encode::undefined();
}

ReturnedValue QQmlContextWrapper::lookupType(Lookup *l, ExecutionEngine *engine, Value *base)
{
    Scope scope(engine);
    Scoped<QmlContext> qmlContext(scope, engine->qmlContext());
    if (!qmlContext)
        return QV4::Encode::undefined();

    QObject *scopeObject = qmlContext->qmlScope();
    if (scopeObject && QQmlData::wasDeleted(scopeObject))
        return QV4::Encode::undefined();

    Heap::Object *heapObject = l->qmlTypeLookup.qmlTypeWrapper;
    if (static_cast<Heap::QQmlTypeWrapper *>(heapObject)->object != scopeObject) {
        l->qmlTypeLookup.qmlTypeWrapper = nullptr;
        l->qmlContextPropertyGetter = QQmlContextWrapper::resolveQmlContextPropertyLookupGetter;
        return QQmlContextWrapper::resolveQmlContextPropertyLookupGetter(l, engine, base);
    }

    return Value::fromHeapObject(heapObject).asReturnedValue();
}

void Heap::QmlContext::init(QV4::ExecutionContext *outerContext, QV4::QQmlContextWrapper *qml)
{
    Heap::ExecutionContext::init(Heap::ExecutionContext::Type_QmlContext);
    outer.set(internalClass->engine, outerContext->d());

    this->activation.set(internalClass->engine, qml->d());
}

Heap::QmlContext *QmlContext::create(
        ExecutionContext *parent, QQmlRefPointer<QQmlContextData> context,
        QObject *scopeObject)
{
    Scope scope(parent);

    Scoped<QQmlContextWrapper> qml(
                scope, scope.engine->memoryManager->allocate<QQmlContextWrapper>(
                    std::move(context), scopeObject));
    Heap::QmlContext *c = scope.engine->memoryManager->alloc<QmlContext>(parent, qml);
    Q_ASSERT(c->vtable() == staticVTable());
    return c;
}

QT_END_NAMESPACE
