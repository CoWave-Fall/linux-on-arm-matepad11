# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 3.16)
project(kwin_ambient_color LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_INCLUDE_CURRENT_DIR ON)

# 不使用 find_package(KWin)：Arch 的 KWin 配置会连带查找 Vulkan 头文件，
# 而构建本插件只需要头文件与 libkwin.so。
find_package(Qt6 REQUIRED COMPONENTS Core Gui DBus Quick)

add_library(ambientcolor MODULE
    ambientcolor.cpp
    ambientcolor.h
    resources.qrc
)

# KWin 按 metadata.json 中的 Id 查找效果，文件名保持与 Id 一致便于排查。
set_target_properties(ambientcolor PROPERTIES PREFIX "")

target_include_directories(ambientcolor PRIVATE
    /usr/include/kwin
    /usr/include/KF6/KCoreAddons
    /usr/include/KF6/KConfigCore
    /usr/include/KF6/KConfig
)

target_link_libraries(ambientcolor PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::DBus
    Qt6::Quick
    kwin
)

install(TARGETS ambientcolor
    DESTINATION /usr/lib/qt6/plugins/kwin/effects/plugins
)
