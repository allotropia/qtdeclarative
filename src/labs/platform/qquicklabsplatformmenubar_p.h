/****************************************************************************
**
** Copyright (C) 2020 The Qt Company Ltd.
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

#ifndef QQUICKLABSPLATFORMMENUBAR_P_H
#define QQUICKLABSPLATFORMMENUBAR_P_H

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
#include <QtQml/qqmlparserstatus.h>
#include <QtQml/qqmllist.h>
#include <QtQml/qqml.h>

QT_BEGIN_NAMESPACE

class QWindow;
class QPlatformMenuBar;
class QQuickLabsPlatformMenu;

class QQuickLabsPlatformMenuBar : public QObject, public QQmlParserStatus
{
    Q_OBJECT
    QML_NAMED_ELEMENT(MenuBar)
    Q_INTERFACES(QQmlParserStatus)
    Q_PROPERTY(QQmlListProperty<QObject> data READ data FINAL)
    Q_PROPERTY(QQmlListProperty<QQuickLabsPlatformMenu> menus READ menus NOTIFY menusChanged FINAL)
    Q_PROPERTY(QWindow *window READ window WRITE setWindow NOTIFY windowChanged FINAL)
    Q_CLASSINFO("DefaultProperty", "data")

public:
    explicit QQuickLabsPlatformMenuBar(QObject *parent = nullptr);
    ~QQuickLabsPlatformMenuBar();

    QPlatformMenuBar *handle() const;

    QQmlListProperty<QObject> data();
    QQmlListProperty<QQuickLabsPlatformMenu> menus();

    QWindow *window() const;
    void setWindow(QWindow *window);

    Q_INVOKABLE void addMenu(QQuickLabsPlatformMenu *menu);
    Q_INVOKABLE void insertMenu(int index, QQuickLabsPlatformMenu *menu);
    Q_INVOKABLE void removeMenu(QQuickLabsPlatformMenu *menu);
    Q_INVOKABLE void clear();

Q_SIGNALS:
    void menusChanged();
    void windowChanged();

protected:
    void classBegin() override;
    void componentComplete() override;

    QWindow *findWindow() const;

    static void data_append(QQmlListProperty<QObject> *property, QObject *object);
    static qsizetype data_count(QQmlListProperty<QObject> *property);
    static QObject *data_at(QQmlListProperty<QObject> *property, qsizetype index);
    static void data_clear(QQmlListProperty<QObject> *property);

    static void menus_append(QQmlListProperty<QQuickLabsPlatformMenu> *property, QQuickLabsPlatformMenu *menu);
    static qsizetype menus_count(QQmlListProperty<QQuickLabsPlatformMenu> *property);
    static QQuickLabsPlatformMenu *menus_at(QQmlListProperty<QQuickLabsPlatformMenu> *property, qsizetype index);
    static void menus_clear(QQmlListProperty<QQuickLabsPlatformMenu> *property);

private:
    bool m_complete;
    QWindow *m_window;
    QList<QObject *> m_data;
    QList<QQuickLabsPlatformMenu *> m_menus;
    QPlatformMenuBar *m_handle;
};

QT_END_NAMESPACE

QML_DECLARE_TYPE(QQuickLabsPlatformMenuBar)

#endif // QQUICKLABSPLATFORMMENUBAR_P_H
