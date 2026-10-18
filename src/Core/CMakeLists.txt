# The core library: scanner, layouts, settings, text, colours (as Core.vcxproj).
add_library(SpaceWindowCore STATIC
  Backdrop.cpp
  ChangeJournal.cpp      # the journal reader is Windows-only; applying changes is not
  Commands.cpp
  FolderTree.cpp
  Format.cpp
  LabelFit.cpp
  Lang.cpp
  MapItem.cpp
  Ntfs.cpp               # the volume reader is Windows-only; the record parser is not
  Palette.cpp
  PieLayout.cpp
  Settings.cpp
  Text.cpp
  Theme.cpp
  TreemapLayout.cpp
  ViewHistory.cpp
  # Linux only (Windows has FolderWatch.cpp, IoCancellation.cpp, Parallel.cpp):
  FolderTreePosix.cpp
  FolderWatchPosix.cpp
  IoCancellationPosix.cpp
  ParallelPosix.cpp
  Posix.cpp
  TrashPosix.cpp)
target_include_directories(SpaceWindowCore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_precompile_headers(SpaceWindowCore PRIVATE pch.h)
target_link_libraries(SpaceWindowCore PUBLIC Threads::Threads PRIVATE spacewindow_options)
