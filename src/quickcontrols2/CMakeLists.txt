#####################################################################
## QuickControls2 Module:
#####################################################################

qt_internal_add_qml_module(QuickControls2
    URI "QtQuick.Controls"
    VERSION "${PROJECT_VERSION}"
    PAST_MAJOR_VERSIONS 2
    DESIGNER_SUPPORTED
    CLASS_NAME QtQuickControls2Plugin
    PLUGIN_TARGET qtquickcontrols2plugin
    IMPORTS
        QtQuick.Controls.impl/auto
    OPTIONAL_IMPORTS
        QtQuick.Controls.Basic/auto
        QtQuick.Controls.Fusion/auto
        QtQuick.Controls.Material/auto
        QtQuick.Controls.Imagine/auto
        QtQuick.Controls.Universal/auto
        QtQuick.Controls.Windows/auto
        QtQuick.Controls.macOS/auto
    NO_PLUGIN_OPTIONAL
    NO_GENERATE_PLUGIN_SOURCE
    SOURCES
        qquickstyle.cpp qquickstyle.h qquickstyle_p.h
        qquickstyleplugin.cpp qquickstyleplugin_p.h
        qtquickcontrols2global.h
    DEFINES
        QT_NO_CAST_FROM_ASCII
        QT_NO_CAST_TO_ASCII
    LIBRARIES
        Qt::CorePrivate
        Qt::GuiPrivate
        Qt::QmlPrivate
        Qt::QuickPrivate
        Qt::QuickTemplates2
        Qt::QuickTemplates2Private
    PUBLIC_LIBRARIES
        Qt::Core
        Qt::Gui
        Qt::Quick
    GENERATE_CPP_EXPORTS
)

qt_internal_extend_target(qtquickcontrols2plugin
    SOURCES
        qtquickcontrols2plugin.cpp
    LIBRARIES
        Qt::QuickControls2ImplPrivate
        Qt::QuickControls2Private
        Qt::QuickTemplates2Private
)

if(QT_FEATURE_quick_designer)
    add_subdirectory(designer)
endif()

add_subdirectory(basic)
add_subdirectory(fusion)
add_subdirectory(imagine)
add_subdirectory(material)
add_subdirectory(universal)

if(MACOS)
    add_subdirectory(macos)
endif()

if(WIN32)
    add_subdirectory(windows)
endif()

qt_internal_add_docs(QuickControls2
    doc/qtquickcontrols.qdocconf
)
