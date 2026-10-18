# The window's logic without a toolkit: what the map shows and how it moves
# (MapView), the theme shown and the background animation's clock
# (Presentation).  The Qt app uses it; the WinUI app still has its own copy.
add_library(SpaceWindowPresenter STATIC MapView.cpp Presentation.cpp)
target_include_directories(SpaceWindowPresenter PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(SpaceWindowPresenter PUBLIC SpaceWindowCore PRIVATE spacewindow_options)
