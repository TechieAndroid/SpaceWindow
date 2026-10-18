# The Qt 6 Widgets app: the Linux counterpart of the WinUI app in src/App.
# Its windows are a library, so that tests/AppQt can drive them.
set(SPACEWINDOW_APP_ID io.github.techieandroid.SpaceWindow)  # as the files in packaging/linux are named
configure_file(Version.h.in ${CMAKE_CURRENT_BINARY_DIR}/Version.h @ONLY)

add_library(SpaceWindowQt STATIC
  Desktop.cpp
  Dialogs.cpp
  MainWindow.cpp
  MapRenderer.cpp
  MapRendererBackdrop.cpp
  MapWidget.cpp
  QtText.cpp
  Shell.cpp)
set_target_properties(SpaceWindowQt PROPERTIES AUTOMOC ON)
target_include_directories(SpaceWindowQt PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})
target_compile_definitions(SpaceWindowQt PUBLIC QT_NO_KEYWORDS QT_NO_CAST_FROM_ASCII QT_NO_CAST_TO_ASCII
                                                $<$<BOOL:${SPACEWINDOW_QT_BASELINE}>:SPACEWINDOW_QT_BASELINE>)
target_link_libraries(SpaceWindowQt PUBLIC SpaceWindowPresenter Qt6::Widgets Qt6::DBus PRIVATE spacewindow_options)

add_executable(SpaceWindow main.cpp SpaceWindow.qrc)
set_target_properties(SpaceWindow PROPERTIES OUTPUT_NAME spacewindow AUTORCC ON)
target_link_libraries(SpaceWindow PRIVATE SpaceWindowQt spacewindow_options)

# Where desktops look: the program, its menu entry, its icon in every size, and
# its description for software centres (packaging/linux); and the notices that
# go with every copy, as the Windows zip has them.
include(GNUInstallDirs)
install(TARGETS SpaceWindow RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES ${PROJECT_SOURCE_DIR}/packaging/linux/${SPACEWINDOW_APP_ID}.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES ${PROJECT_SOURCE_DIR}/packaging/linux/${SPACEWINDOW_APP_ID}.metainfo.xml
        DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)
install(DIRECTORY ${PROJECT_SOURCE_DIR}/packaging/linux/icons/ DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor)
install(FILES ${PROJECT_SOURCE_DIR}/LICENSE.txt ${PROJECT_SOURCE_DIR}/THIRD-PARTY-NOTICES.md
        DESTINATION ${CMAKE_INSTALL_DOCDIR})
