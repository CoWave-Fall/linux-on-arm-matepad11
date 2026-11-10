# SPDX-License-Identifier: GPL-3.0-or-later
#
# 构建系统设置的内嵌 KCM 插件。
# QML 由 kcmutils_add_qml_kcm 打包进 QRC，随插件一起安装。
cmake_minimum_required(VERSION 3.16)

project(loa-tablet-kcm VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
# kcoreaddons_add_plugin 在 BUILD_SHARED_LIBS 关闭时构建静态库，
# KCM 必须作为可加载的 MODULE 插件安装。
set(BUILD_SHARED_LIBS ON)
# kcoreaddons_add_plugin 要求显式设置库输出目录。
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

find_package(ECM 6.0 REQUIRED NO_MODULE)
set(CMAKE_MODULE_PATH ${ECM_MODULE_PATH} ${CMAKE_MODULE_PATH})

include(KDEInstallDirs)

find_package(Qt6 REQUIRED COMPONENTS Core Qml)
find_package(KF6 REQUIRED COMPONENTS CoreAddons KCMUtils)

kcmutils_add_qml_kcm(kcm_loa_tablet SOURCES kcm_loa_tablet.cpp)

# KCM 的 QRC 前缀与插件文件名必须都是 kcm_loa_tablet，
# KQuickConfigModule 按插件 Id 查找 qrc:/kcm/<id>/ui/main.qml。
set_target_properties(kcm_loa_tablet PROPERTIES PREFIX "")

target_link_libraries(kcm_loa_tablet PRIVATE
    Qt6::Core
    Qt6::Qml
    KF6::CoreAddons
    KF6::KCMUtilsCore
    KF6::KCMUtilsQuick
)
