/****************************************************************************
**
** Copyright (C) 2016 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of the QtQuick module of the Qt Toolkit.
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

#include "qquickrendercontrol.h"
#include "qquickrendercontrol_p.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QTime>
#include <QtQuick/private/qquickanimatorcontroller_p.h>
#include <QtQuick/private/qsgdefaultrendercontext_p.h>
#include <QtQuick/private/qsgrhisupport_p.h>

#include <private/qsgrhishadereffectnode_p.h>

#include <QtGui/private/qguiapplication_p.h>
#include <qpa/qplatformintegration.h>
#include <QtGui/qoffscreensurface.h>

#include <QtQml/private/qqmlglobal_p.h>

#include <QtQuick/QQuickWindow>
#include <QtQuick/QQuickRenderTarget>
#include <QtQuick/private/qquickwindow_p.h>
#include <QtQuick/private/qquickitem_p.h>
#include <QtQuick/private/qsgsoftwarerenderer_p.h>
#include <QtCore/private/qobject_p.h>

#include <QtQuick/private/qquickwindow_p.h>
#include <QtGui/private/qrhi_p.h>

QT_BEGIN_NAMESPACE

/*!
  \class QQuickRenderControl

  \brief The QQuickRenderControl class provides a mechanism for rendering the Qt
  Quick scenegraph onto an offscreen render target in a fully
  application-controlled manner.

  \since 5.4

  QQuickWindow and QQuickView and their associated internal render loops render
  the Qt Quick scene onto a native window. In some cases, for example when
  integrating with 3rd party OpenGL, Vulkan, Metal, or Direct 3D renderers, it
  can be useful to get the scene into a texture that can then be used in
  arbitrary ways by the external rendering engine. Such a mechanism is also
  essential when integrating with a VR framework. QQuickRenderControl makes this
  possible in a hardware accelerated manner, unlike the performance-wise limited
  alternative of using QQuickWindow::grabWindow()

  When using a QQuickRenderControl, the QQuickWindow must not be
  \l{QWindow::show()}{shown} (it will not be visible on-screen) and there will
  not be an underlying native window for it. Instead, the QQuickWindow instance
  is associated with the render control object, using the overload of the
  QQuickWindow constructor, and a texture or image object specified via
  QQuickWindow::setRenderTarget(). The QQuickWindow object is still essential,
  because it represents the Qt Quick scene and provides the bulk of the scene
  management and event delivery mechanisms. It does not however act as a real
  on-screen window from the windowing system's perspective.

  Management of the graphics devices, contexts, image and texture objects is up
  to the application. The device or context that will be used by Qt Quick must
  be created before calling initialize(). The creation of the the texture object
  can be deferred, see below. Qt 5.4 introduces the ability for QOpenGLContext
  to adopt existing native contexts. Together with QQuickRenderControl this
  makes it possible to create a QOpenGLContext that shares with an external
  rendering engine's existing context. This new QOpenGLContext can then be used
  to render the Qt Quick scene into a texture that is accessible by the other
  engine's context too. For Vulkan, Metal, and Direct 3D there are no
  Qt-provided wrappers for device objects, so existing ones can be passed as-is
  via QQuickWindow::setGraphicsDevice().

  Loading and instantiation of the QML components happen by using a
  QQmlEngine. Once the root object is created, it will need to be parented to
  the QQuickWindow's contentItem().

  Applications will usually have to connect to 4 important signals:

  \list

  \li QQuickWindow::sceneGraphInitialized() Emitted at some point after calling
  QQuickRenderControl::initialize(). Upon this signal, the application is
  expected to create its framebuffer object and associate it with the
  QQuickWindow.

  \li QQuickWindow::sceneGraphInvalidated() When the scenegraph resources are
  released, the framebuffer object can be destroyed too.

  \li QQuickRenderControl::renderRequested() Indicates that the scene has to be
  rendered by calling render(). After making the context current, applications
  are expected to call render().

  \li QQuickRenderControl::sceneChanged() Indicates that the scene has changed
  meaning that, before rendering, polishing and synchronizing is also necessary.

  \endlist

  To send events, for example mouse or keyboard events, to the scene, use
  QCoreApplication::sendEvent() with the QQuickWindow instance as the receiver.

  For key events it may be also necessary to set the focus manually on the
  desired item. In practice this involves calling
  \l{QQuickItem::forceActiveFocus()}{forceActiveFocus()} on the desired item,
  for example the scene's root item, once it is associated with the scene (the
  QQuickWindow).

  \note In general QQuickRenderControl is supported in combination with all Qt
  Quick backends. However, some functionality, in particular grab(), may not be
  available in all cases.

  \inmodule QtQuick
*/

QSGContext *QQuickRenderControlPrivate::sg = nullptr;

QQuickRenderControlPrivate::QQuickRenderControlPrivate(QQuickRenderControl *renderControl)
    : q(renderControl),
      initialized(false),
      window(nullptr),
      rhi(nullptr),
      ownRhi(true),
      cb(nullptr),
      offscreenSurface(nullptr),
      sampleCount(1)
{
    if (!sg) {
        qAddPostRoutine(cleanup);
        sg = QSGContext::createDefaultContext();
    }
    rc = sg->createRenderContext();
}

void QQuickRenderControlPrivate::cleanup()
{
    delete sg;
    sg = nullptr;
}

/*!
   Constructs a QQuickRenderControl object, with parent
   object \a parent.
*/
QQuickRenderControl::QQuickRenderControl(QObject *parent)
    : QObject(*(new QQuickRenderControlPrivate(this)), parent)
{
}

/*!
 \internal
*/
QQuickRenderControl::QQuickRenderControl(QQuickRenderControlPrivate &dd, QObject * parent)
    : QObject(dd, parent)
{
}

/*!
  Destroys the instance. Releases all scenegraph resources.

  \sa invalidate()
 */
QQuickRenderControl::~QQuickRenderControl()
{
    Q_D(QQuickRenderControl);

    invalidate();

    if (d->window)
        QQuickWindowPrivate::get(d->window)->renderControl = nullptr;

    // It is likely that the cleanup in windowDestroyed() is not called since
    // the standard pattern is to destroy the rendercontrol before the QQuickWindow.
    // Do it here.
    d->windowDestroyed();

    delete d->rc;

    d->resetRhi();
}

void QQuickRenderControlPrivate::windowDestroyed()
{
    if (window) {
        QQuickWindowPrivate *cd = QQuickWindowPrivate::get(window);
        cd->cleanupNodesOnShutdown();

        rc->invalidate();

        QQuickWindowPrivate::get(window)->animationController.reset();
#if QT_CONFIG(quick_shadereffect)
        QSGRhiShaderEffectNode::cleanupMaterialTypeCache();
#endif
        window = nullptr;
    }
}

/*!
  Prepares rendering the Qt Quick scene outside the GUI thread.

  \a targetThread specifies the thread on which synchronization and
  rendering will happen. There is no need to call this function in a
  single threaded scenario.
 */
void QQuickRenderControl::prepareThread(QThread *targetThread)
{
    Q_D(QQuickRenderControl);
    d->rc->moveToThread(targetThread);
    QQuickWindowPrivate::get(d->window)->animationController->moveToThread(targetThread);
}

/*!
    Sets the number of samples to use for multisampling. When \a sampleCount is
    0 or 1, multisampling is disabled.

    \note This function is always used in combination with a multisample render
    target, which means \a sampleCount must match the sample count passed to
    QQuickRenderTarget::fromNativeTexture(), which in turn must match the
    sample count of the native texture.

    \since 6.0

    \sa initialize(), QQuickRenderTarget
 */
void QQuickRenderControl::setSamples(int sampleCount)
{
    Q_D(QQuickRenderControl);
    d->sampleCount = qMax(1, sampleCount);
}

/*!
    \return the current sample count. 1 or 0 means no multisampling.

    \since 6.0
 */
int QQuickRenderControl::samples() const
{
    Q_D(const QQuickRenderControl);
    return d->sampleCount;
}

/*!
    Initializes the scene graph resources. When using a graphics API, such as
    Vulkan, Metal, OpenGL, or Direct3D, for Qt Quick rendering,
    QQuickRenderControl will set up an appropriate rendering engine when this
    function is called. This rendering infrastructure exists as long as the
    QQuickRenderControl exists.

    To control what graphics API Qt Quick uses, call
    QQuickWindow::setGraphicsApi() with one of the
    QSGRendererInterface:GraphicsApi constants. That must be done before
    calling this function.

    To prevent the scenegraph from creating its own device and context objects,
    specify an appropriate QQuickGraphicsDevice, wrapping existing graphics
    objects, by calling QQuickWindow::setGraphicsDevice().

    To configure which device extensions to enable (for example, for Vulkan),
    call QQuickWindow::setGraphicsConfiguration() before this function.

    \note When using Vulkan, QQuickRenderControl does not create a QVulkanInstance
    automatically. Rather, it is the application's responsibility to create a
    suitable QVulkanInstance and \l{QWindow::setVulkanInstance()}{associate it} with
    the QQuickWindow. Before initializing the QVulkanInstance, it is strongly
    encouraged to query the list of Qt Quick's desired instance extensions by calling
    the static function QQuickGraphicsConfiguration::preferredInstanceExtensions()
    and to pass the returned list to QVulkanInstance::setExtensions().

    Returns \c true on success, \c false otherwise.

    \note This function does not need to be, and must not be, called when using
    the \c software adaptation of Qt Quick.

    \since 6.0

    \sa QQuickRenderTarget, QQuickGraphicsDevice, QQuickGraphicsConfiguration::preferredInstanceExtensions()
 */
bool QQuickRenderControl::initialize()
{
    Q_D(QQuickRenderControl);
    if (!d->window) {
        qWarning("QQuickRenderControl::initialize called with no associated window");
        return false;
    }

    if (!d->initRhi())
        return false;

    QQuickWindowPrivate *wd = QQuickWindowPrivate::get(d->window);
    wd->rhi = d->rhi;

    QSGDefaultRenderContext *renderContext = qobject_cast<QSGDefaultRenderContext *>(d->rc);
    if (renderContext) {
        QSGDefaultRenderContext::InitParams params;
        params.rhi = d->rhi;
        params.sampleCount = d->sampleCount;
        params.initialSurfacePixelSize = d->window->size() * d->window->effectiveDevicePixelRatio();
        params.maybeSurface = d->window;
        renderContext->initialize(&params);
    } else {
        qWarning("QRhi is only compatible with default adaptation");
        return false;
    }
    return true;
}

/*!
  This function should be called as late as possible before
  sync(). In a threaded scenario, rendering can happen in parallel
  with this function.
 */
void QQuickRenderControl::polishItems()
{
    Q_D(QQuickRenderControl);
    if (!d->window)
        return;

    QQuickWindowPrivate *cd = QQuickWindowPrivate::get(d->window);
    cd->flushFrameSynchronousEvents();
    if (!d->window)
        return;
    cd->polishItems();
    emit d->window->afterAnimating();
}

/*!
  This function is used to synchronize the QML scene with the rendering scene
  graph.

  If a dedicated render thread is used, the GUI thread should be blocked for the
  duration of this call.

  \return \e true if the synchronization changed the scene graph.
 */
bool QQuickRenderControl::sync()
{
    Q_D(QQuickRenderControl);
    if (!d->window)
        return false;

    QQuickWindowPrivate *cd = QQuickWindowPrivate::get(d->window);
    // we may not have a d->rhi (software backend) hence the check is important
    if (d->rhi) {
        if (!d->rhi->isRecordingFrame()) {
            qWarning("QQuickRenderControl can only sync when beginFrame() has been called");
            return false;
        }
        if (!d->cb) {
            qWarning("QQuickRenderControl cannot be used with QRhi when no QRhiCommandBuffer is provided");
            return false;
        }
        cd->setCustomCommandBuffer(d->cb);
    }

    cd->syncSceneGraph();
    d->rc->endSync();

    return true;
}

/*!
  Stop rendering and release resources.

  This is the equivalent of the cleanup operations that happen with a
  real QQuickWindow when the window becomes hidden.

  This function is called from the destructor. Therefore there will
  typically be no need to call it directly.

  Once invalidate() has been called, it is possible to reuse the
  QQuickRenderControl instance by calling initialize() again.

  \note This function does not take
  QQuickWindow::persistentSceneGraph() or
  QQuickWindow::persistentGraphics() into account. This means
  that context-specific resources are always released.
 */
void QQuickRenderControl::invalidate()
{
    Q_D(QQuickRenderControl);
    if (!d->window)
        return;

    QQuickWindowPrivate *cd = QQuickWindowPrivate::get(d->window);
    cd->fireAboutToStop();
    cd->cleanupNodesOnShutdown();

    if (!d->initialized)
        return;

    // We must invalidate since the context can potentially be destroyed by the
    // application right after returning from this function. Invalidating is
    // also essential to allow a subsequent initialize() to succeed.
    d->rc->invalidate();

    d->initialized = false;
}

/*!
  Renders the scenegraph using the current context.
 */
void QQuickRenderControl::render()
{
    Q_D(QQuickRenderControl);
    if (!d->window)
        return;

    QQuickWindowPrivate *cd = QQuickWindowPrivate::get(d->window);
    // we may not have a d->rhi (software backend) hence the check is important
    if (d->rhi) {
        if (!d->rhi->isRecordingFrame()) {
            qWarning("QQuickRenderControl can only render when beginFrame() has been called");
            return;
        }
        if (!d->cb) {
            qWarning("QQuickRenderControl cannot be used with QRhi when no QRhiCommandBuffer is provided");
            return;
        }
        cd->setCustomCommandBuffer(d->cb);
    }

    cd->renderSceneGraph(d->window->size());
}

/*!
    \fn void QQuickRenderControl::renderRequested()

    This signal is emitted when the scene graph needs to be rendered. It is not necessary to call sync().

    \note Avoid triggering rendering directly when this signal is
    emitted. Instead, prefer deferring it by using a timer for example. This
    will lead to better performance.
*/

/*!
    \fn void QQuickRenderControl::sceneChanged()

    This signal is emitted when the scene graph is updated, meaning that
    polishItems() and sync() needs to be called. If sync() returns
    true, then render() needs to be called.

    \note Avoid triggering polishing, synchronization and rendering directly
    when this signal is emitted. Instead, prefer deferring it by using a timer
    for example. This will lead to better performance.
*/

QImage QQuickRenderControlPrivate::grab()
{
    if (!window)
        return QImage();

    QImage grabContent;

    if (rhi) {

        // As documented by QQuickWindow::grabWindow(): Nothing to do here, we
        // do not support "grabbing" with an application-provided render target
        // in Qt 6. (with the exception of the software backend because that
        // does not support custom render targets, so the grab implementation
        // here is still valuable)

#if QT_CONFIG(thread)
    } else if (window->rendererInterface()->graphicsApi() == QSGRendererInterface::Software) {
        QQuickWindowPrivate *cd = QQuickWindowPrivate::get(window);
        cd->polishItems();
        cd->syncSceneGraph();
        QSGSoftwareRenderer *softwareRenderer = static_cast<QSGSoftwareRenderer *>(cd->renderer);
        if (softwareRenderer) {
            const qreal dpr = window->effectiveDevicePixelRatio();
            const QSize imageSize = window->size() * dpr;
            grabContent = QImage(imageSize, QImage::Format_ARGB32_Premultiplied);
            grabContent.setDevicePixelRatio(dpr);
            QPaintDevice *prevDev = softwareRenderer->currentPaintDevice();
            softwareRenderer->setCurrentPaintDevice(&grabContent);
            softwareRenderer->markDirty();
            rc->endSync();
            q->render();
            softwareRenderer->setCurrentPaintDevice(prevDev);
        }
#endif
    } else {
        qWarning("QQuickRenderControl: grabs are not supported with the current Qt Quick backend");
    }

    return grabContent;
}

void QQuickRenderControlPrivate::update()
{
    Q_Q(QQuickRenderControl);
    emit q->renderRequested();
}

void QQuickRenderControlPrivate::maybeUpdate()
{
    Q_Q(QQuickRenderControl);
    emit q->sceneChanged();
}

/*!
  \fn QWindow *QQuickRenderControl::renderWindow(QPoint *offset)

  Reimplemented in subclasses to return the real window this render control
  is rendering into.

  If \a offset in non-null, it is set to the offset of the control
  inside the window.

  \note While not mandatory, reimplementing this function becomes essential for
  supporting multiple screens with different device pixel ratios and properly positioning
  popup windows opened from QML. Therefore providing it in subclasses is highly
  recommended.
*/

/*!
  Returns the real window that \a win is being rendered to, if any.

  If \a offset in non-null, it is set to the offset of the rendering
  inside its window.

 */
QWindow *QQuickRenderControl::renderWindowFor(QQuickWindow *win, QPoint *offset)
{
    if (!win)
        return nullptr;
    QQuickRenderControl *rc = QQuickWindowPrivate::get(win)->renderControl;
    if (rc)
        return rc->renderWindow(offset);
    return nullptr;
}

bool QQuickRenderControlPrivate::isRenderWindowFor(QQuickWindow *quickWin, const QWindow *renderWin)
{
    QQuickRenderControl *rc = QQuickWindowPrivate::get(quickWin)->renderControl;
    if (rc)
        return QQuickRenderControlPrivate::get(rc)->isRenderWindow(renderWin);
    return false;
}

/*!
    \return the QQuickWindow this QQuickRenderControl is associated with.

    \note A QQuickRenderControl gets associated with a QQuickWindow when
    constructing the QQuickWindow. The return value from this function is null
    before that point.

    \since 6.0
 */
QQuickWindow *QQuickRenderControl::window() const
{
    Q_D(const QQuickRenderControl);
    return d->window;
}

/*!
    Specifies the start of a graphics frame. Calls to sync() or render() must
    be enclosed by calls to beginFrame() and endFrame().

    Unlike the earlier OpenGL-only world of Qt 5, rendering with other graphics
    APIs requires more well-defined points of starting and ending a frame. When
    manually driving the rendering loop via QQuickRenderControl, it now falls
    to the user of QQuickRenderControl to specify these points.

    A typical update step, including initialization of rendering into an
    existing texture, could like like the following. The example snippet
    assumes Direct3D 11 but the same concepts apply other graphics APIs as
    well.

    \badcode
        if (!m_quickInitialized) {
            m_quickWindow->setGraphicsDevice(QQuickGraphicsDevice::fromDeviceAndContext(m_engine->device(), m_engine->context()));

            if (!m_renderControl->initialize())
                qWarning("Failed to initialize redirected Qt Quick rendering");

            m_quickWindow->setRenderTarget(QQuickRenderTarget::fromNativeTexture({ quint64(m_res.texture), 0 },
                                                                                 QSize(QML_WIDTH, QML_HEIGHT),
                                                                                 SAMPLE_COUNT));

            m_quickInitialized = true;
        }

        m_renderControl->polishItems();

        m_renderControl->beginFrame();
        m_renderControl->sync();
        m_renderControl->render();
        m_renderControl->endFrame(); // Qt Quick's rendering commands are submitted to the device context here
    \endcode

    \since 6.0

    \sa endFrame(), initialize(), sync(), render(), QQuickGraphicsDevice, QQuickRenderTarget
 */
void QQuickRenderControl::beginFrame()
{
    Q_D(QQuickRenderControl);
    if (!d->rhi || d->rhi->isRecordingFrame())
        return;

    emit d->window->beforeFrameBegin();

    d->rhi->beginOffscreenFrame(&d->cb);
}

/*!
    Specifies the end of a graphics frame. Calls to sync() or render() must be
    enclosed by calls to beginFrame() and endFrame().

    When this function is called, any graphics commands enqueued by the
    scenegraph are submitted to the context or command queue, whichever is
    applicable.

    \since 6.0

    \sa beginFrame(), initialize(), sync(), render(), QQuickGraphicsDevice, QQuickRenderTarget
 */
void QQuickRenderControl::endFrame()
{
    Q_D(QQuickRenderControl);
    if (!d->rhi || !d->rhi->isRecordingFrame())
        return;

    d->rhi->endOffscreenFrame();
    d->cb = nullptr;

    emit d->window->afterFrameEnd();
}

bool QQuickRenderControlPrivate::initRhi()
{
    // initialize() - invalidate() - initialize() uses the QRhi the first
    // initialize() created, so if already exists, we are done
    if (rhi)
        return true;

    QSGRhiSupport *rhiSupport = QSGRhiSupport::instance();

    // sanity check for Vulkan
#if QT_CONFIG(vulkan)
    if (rhiSupport->rhiBackend() == QRhi::Vulkan && !window->vulkanInstance()) {
        qWarning("QQuickRenderControl: No QVulkanInstance set for QQuickWindow, cannot initialize");
        return false;
    }
#endif

    // for OpenGL
    if (!offscreenSurface)
        offscreenSurface = rhiSupport->maybeCreateOffscreenSurface(window);

    QSGRhiSupport::RhiCreateResult result = rhiSupport->createRhi(window, offscreenSurface);
    if (!result.rhi) {
        qWarning("QQuickRenderControl: Failed to initialize QRhi");
        return false;
    }

    rhi = result.rhi;
    ownRhi = result.own;

    return true;
}

void QQuickRenderControlPrivate::resetRhi()
{
    if (ownRhi)
        QSGRhiSupport::instance()->destroyRhi(rhi);

    rhi = nullptr;

    delete offscreenSurface;
    offscreenSurface = nullptr;
}

QT_END_NAMESPACE

#include "moc_qquickrendercontrol.cpp"
