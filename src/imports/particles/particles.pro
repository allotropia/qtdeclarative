CXX_MODULE = qml
TARGET  = particlesplugin
TARGETPATH = QtQuick/Particles.2
IMPORT_VERSION = 2.15

SOURCES += \
    plugin.cpp

QT += quick-private quickparticles-private qml-private

load(qml_plugin)
