# SPDX-License-Identifier: GPL-2.0-or-later
cmake_minimum_required(VERSION 3.22.1)
project(chameleon_presenter CXX)

add_library(chameleon_presenter SHARED presenter.cpp)
target_include_directories(chameleon_presenter PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../common)
target_compile_options(chameleon_presenter PRIVATE -Wall -Wextra)
target_link_libraries(chameleon_presenter android nativewindow log EGL GLESv3)
