/****************************************************************************
**
** Copyright (C) 2017 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of the Qt Labs Platform module of the Qt Toolkit.
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

#ifndef QQUICKLABSPLATFORMDIALOG_P_H
#define QQUICKLABSPLATFORMDIALOG_P_H

//
//  W A R N I N G
//  -------------
//
// This file is not part of the Qt API.  It exists purely as an
// implementation detail.  This header file may change from version to
// version without notice, or even be removed.
//
// We mean it.
//

#include <QtCore/qobject.h>
#include <QtGui/qpa/qplatformtheme.h>
#include <QtGui/qpa/qplatformdialoghelper.h>
#include <QtQml/qqmlparserstatus.h>
#include <QtQml/qqmllist.h>
#include <QtQml/qqml.h>

QT_BEGIN_NAMESPACE

class QWindow;
class QPlatformDialogHelper;

class QQuickLabsPlatformDialog : public QObject, public QQmlParserStatus
{
    Q_OBJECT
    Q_INTERFACES(QQmlParserStatus)
    QML_NAMED_ELEMENT(Dialog)
    QML_UNCREATABLE("Dialog is an abstract base class")
    Q_PROPERTY(QQmlListProperty<QObject> data READ data FINAL)
    Q_PROPERTY(QWindow *parentWindow READ parentWindow WRITE setParentWindow NOTIFY parentWindowChanged FINAL)
    Q_PROPERTY(QString title READ title WRITE setTitle NOTIFY titleChanged FINAL)
    Q_PROPERTY(Qt::WindowFlags flags READ flags WRITE setFlags NOTIFY flagsChanged FINAL)
    Q_PROPERTY(Qt::WindowModality modality READ modality WRITE setModality NOTIFY modalityChanged FINAL)
    Q_PROPERTY(bool visible READ isVisible WRITE setVisible NOTIFY visibleChanged FINAL)
    Q_PROPERTY(int result READ result WRITE setResult NOTIFY resultChanged FINAL)
    Q_CLASSINFO("DefaultProperty", "data")

public:
    explicit QQuickLabsPlatformDialog(QPlatformTheme::DialogType type, QObject *parent = nullptr);
    ~QQuickLabsPlatformDialog();

    QPlatformDialogHelper *handle() const;

    QQmlListProperty<QObject> data();

    QWindow *parentWindow() const;
    void setParentWindow(QWindow *window);

    QString title() const;
    void setTitle(const QString &title);

    Qt::WindowFlags flags() const;
    void setFlags(Qt::WindowFlags flags);

    Qt::WindowModality modality() const;
    void setModality(Qt::WindowModality modality);

    bool isVisible() const;
    void setVisible(bool visible);

    enum StandardCode { Rejected, Accepted };
    Q_ENUM(StandardCode)

    int result() const;
    void setResult(int result);

public Q_SLOTS:
    void open();
    void close();
    virtual void accept();
    virtual void reject();
    virtual void done(int result);

Q_SIGNALS:
    void accepted();
    void rejected();
    void parentWindowChanged();
    void titleChanged();
    void flagsChanged();
    void modalityChanged();
    void visibleChanged();
    void resultChanged();

protected:
    void classBegin() override;
    void componentComplete() override;

    bool create();
    void destroy();

    virtual bool useNativeDialog() const;
    virtual void onCreate(QPlatformDialogHelper *dialog);
    virtual void onShow(QPlatformDialogHelper *dialog);
    virtual void onHide(QPlatformDialogHelper *dialog);

    QWindow *findParentWindow() const;

private:
    bool m_visible;
    bool m_complete;
    int m_result;
    QWindow *m_parentWindow;
    QString m_title;
    Qt::WindowFlags m_flags;
    Qt::WindowModality m_modality;
    QPlatformTheme::DialogType m_type;
    QList<QObject *> m_data;
    QPlatformDialogHelper *m_handle;
};

class QPlatformDialogHelperForeign
{
    QML_FOREIGN(QPlatformDialogHelper)
    QML_NAMED_ELEMENT(StandardButton)
    QML_UNCREATABLE("Cannot create an instance of StandardButton")
};

QT_END_NAMESPACE

QML_DECLARE_TYPE(QQuickLabsPlatformDialog)

#endif // QQUICKLABSPLATFORMDIALOG_P_H
