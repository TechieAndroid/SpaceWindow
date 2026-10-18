# SpaceWindow's Linux build: the core library, its unit tests, and the Qt app.
#
# Windows builds with build.ps1 and SpaceWindow.slnx (MSBuild): the WinUI app
# has no CMake route.  Keep the source lists here in step with the .vcxproj
# files; the files only one side builds are marked.
#
#   cmake --preset linux-release && cmake --build --preset linux-release && ctest --preset linux-release
cmake_minimum_required(VERSION 3.25)
file(STRINGS VERSION SPACEWINDOW_VERSION LIMIT_COUNT 1)  # as build.ps1 reads it
project(SpaceWindow VERSION ${SPACEWINDOW_VERSION} DESCRIPTION "See where your disk space goes" LANGUAGES CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "This CMake build is for Linux.  On Windows, run build.ps1 (see README.md).")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)  # for clang-tidy and editors
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(SPACEWINDOW_WERROR "Treat compiler warnings as errors, as the Windows build does" ON)
option(SPACEWINDOW_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(SPACEWINDOW_APP "Build the Qt app (needs Qt 6.4 or later: qt6-base-dev)" ON)
option(SPACEWINDOW_QT_BASELINE "Build the Qt app with only what Qt 6.4 has, whatever Qt is installed" OFF)

find_package(Threads REQUIRED)

# Settings every SpaceWindow target compiles with (the Linux side of
# build/SpaceWindow.Cpp.props): strict warnings and a hardened binary.
add_library(spacewindow_options INTERFACE)
target_compile_options(spacewindow_options INTERFACE
  -Wall -Wextra -Wpedantic -Wnon-virtual-dtor -Wimplicit-fallthrough
  -Wno-unknown-pragmas  # MSVC's (#pragma warning), as tools/Run-ClangTidy.ps1 has it
  $<$<BOOL:${SPACEWINDOW_WERROR}>:-Werror>
  -fstack-protector-strong -fstack-clash-protection
  # Some compilers set a level of their own (Ubuntu 22.04's: 2): replaced, not redefined.
  $<$<CONFIG:Release,RelWithDebInfo>:-U_FORTIFY_SOURCE>
  $<$<CONFIG:Release,RelWithDebInfo>:-D_FORTIFY_SOURCE=3>
  $<$<CONFIG:Debug>:-D_GLIBCXX_ASSERTIONS>)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
  target_compile_options(spacewindow_options INTERFACE -fcf-protection=full)  # CET, as on Windows
endif()
target_link_options(spacewindow_options INTERFACE -Wl,-z,relro,-z,now -Wl,-z,noexecstack)
if(SPACEWINDOW_SANITIZE)
  target_compile_options(spacewindow_options INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer
                                                       -fno-sanitize-recover=all)
  target_link_options(spacewindow_options INTERFACE -fsanitize=address,undefined)
endif()
# Position-independent executables, for address space layout randomization (as
# Windows' /DYNAMICBASE), whatever the compiler's own default.
include(CheckPIESupported)
check_pie_supported()
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

add_subdirectory(src/Core)
add_subdirectory(src/Presenter)
if(SPACEWINDOW_APP)
  find_package(Qt6 6.4 QUIET COMPONENTS Widgets DBus)
  if(Qt6_FOUND)
    add_subdirectory(src/AppQt)
  else()
    message(WARNING "Qt 6.4 or later was not found: building the core library and its tests only "
                    "(install qt6-base-dev, or pass -DSPACEWINDOW_APP=OFF to silence this).")
  endif()
endif()

# Distribution packages of what cmake --install installs: cpack -G DEB or
# cpack -G RPM in the build folder.  Their dependencies are those of the
# system they are built on, so build each on the distribution it is for.
set(CPACK_PACKAGE_NAME spacewindow)
set(CPACK_PACKAGE_VENDOR recompiler)
set(CPACK_PACKAGE_CONTACT "recompiler (https://github.com/TechieAndroid/SpaceWindow/issues)")
set(CPACK_PACKAGE_HOMEPAGE_URL https://github.com/TechieAndroid/SpaceWindow)
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "See where your disk space goes: a treemap and pie charts of a drive or folder")
set(CPACK_RESOURCE_FILE_LICENSE ${PROJECT_SOURCE_DIR}/LICENSE.txt)
set(CPACK_PACKAGING_INSTALL_PREFIX /usr)
# Whatever the umask of the one packaging: the folders a package creates are 755.
set(CPACK_INSTALL_DEFAULT_DIRECTORY_PERMISSIONS
    OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_PACKAGE_SECTION utils)
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_PACKAGE_DEPENDS qt6-qpa-plugins)  # Qt's X11 plugin, which no library links to
set(CPACK_DEBIAN_PACKAGE_RECOMMENDS qt6-wayland)   # and its Wayland one
set(CPACK_RPM_FILE_NAME RPM-DEFAULT)
set(CPACK_RPM_PACKAGE_RELEASE_DIST ON)  # 1.fc44: the release it is for
set(CPACK_RPM_PACKAGE_LICENSE MIT)
set(CPACK_RPM_PACKAGE_GROUP Applications/System)
set(CPACK_RPM_PACKAGE_REQUIRES hicolor-icon-theme)
set(CPACK_RPM_PACKAGE_RECOMMENDS qt6-qtwayland)
# The folders the system's packages own (filesystem, hicolor-icon-theme): an
# RPM that claims them too conflicts with them.
set(CPACK_RPM_EXCLUDE_FROM_AUTO_FILELIST_ADDITION
    /usr/share/applications /usr/share/icons /usr/share/icons/hicolor /usr/share/metainfo)
file(GLOB icon_sizes RELATIVE ${PROJECT_SOURCE_DIR}/packaging/linux/icons ${PROJECT_SOURCE_DIR}/packaging/linux/icons/*)
foreach(size IN LISTS icon_sizes)
  list(APPEND CPACK_RPM_EXCLUDE_FROM_AUTO_FILELIST_ADDITION
       /usr/share/icons/hicolor/${size} /usr/share/icons/hicolor/${size}/apps)
endforeach()
include(CPack)

include(CTest)
if(BUILD_TESTING)
  add_subdirectory(tests/CoreTests)
  # The two builds in step: the version everywhere it is stated, and the same shared sources.
  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_Interpreter_FOUND)
    add_test(NAME VersionsAgree COMMAND ${Python3_EXECUTABLE} -I ${PROJECT_SOURCE_DIR}/tools/check_versions.py
                                        ${PROJECT_SOURCE_DIR})
    add_test(NAME SourceListsAgree COMMAND ${Python3_EXECUTABLE} -I ${PROJECT_SOURCE_DIR}/tools/check_sources.py
                                           ${PROJECT_SOURCE_DIR})
  endif()
  if(TARGET SpaceWindowQt)
    find_package(Qt6 QUIET COMPONENTS Test)
    if(Qt6Test_FOUND)
      add_subdirectory(tests/AppQt)
    endif()
  endif()
endif()
