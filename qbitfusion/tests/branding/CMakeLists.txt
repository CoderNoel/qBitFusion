cmake_minimum_required(VERSION 3.22)
project(qbitfusion_branding_checks LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
find_package(Qt6 6.10 REQUIRED COMPONENTS Widgets Svg)
qt_add_executable(branding_checks main.cpp)
qt_add_resources(branding_checks branding
    PREFIX "/qbitfusion"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/../../../src/gui/modern"
    FILES
        "../../../src/gui/modern/brand/qbitfusion.png"
        "../../../src/gui/modern/brand/tray-light.png"
        "../../../src/gui/modern/brand/tray-dark.png"
)
target_link_libraries(branding_checks PRIVATE Qt6::Widgets Qt6::Svg)
if(WIN32)
    target_link_libraries(branding_checks PRIVATE user32 shell32)
endif()
