cmake_minimum_required(VERSION 3.16)
project(qbitfusion_ltspike CXX)

# P1 feasibility spike (CORE-002). Links only libtorrent; deliberately independent of qBittorrent.
find_package(LibtorrentRasterbar 2.1 REQUIRED CONFIG)

add_executable(ltspike ltspike.cpp)
target_compile_features(ltspike PRIVATE cxx_std_20)
target_link_libraries(ltspike PRIVATE LibtorrentRasterbar::torrent-rasterbar)
if (MSVC)
    target_compile_options(ltspike PRIVATE /W4 /permissive- /utf-8)
    target_compile_definitions(ltspike PRIVATE _WIN32_WINNT=0x0A00 NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
