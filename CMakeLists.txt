# CMakeLists.txt --- CMake project settings
##############################################################################

# CMake minimum version
cmake_minimum_required(VERSION 3.5)

# project name and languages
project(PicoPico CXX RC)

##############################################################################

# PicoPico.exe
add_executable(PicoPico WIN32
    PicoPico.cpp
    SaveBitmapToFile.cpp
    PicoPico_res.rc)

target_link_libraries(PicoPico shlwapi comctl32 winmm)

##############################################################################
