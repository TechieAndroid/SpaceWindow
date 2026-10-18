# Unit tests for the core library (as CoreTests.vcxproj).  The drive tests,
# the NTFS benchmark and the comparison (VolumeTests.cpp, Bench.cpp,
# Compare.cpp) read NTFS drives directly, and are built on Windows only;
# BenchPosix.cpp is the benchmark here (CoreTests --bench <folder> [threads...]).
add_executable(CoreTests
  main.cpp
  BackdropTests.cpp
  BenchPosix.cpp
  CommandTests.cpp
  FormatTests.cpp
  LabelFitTests.cpp
  LayoutTests.cpp
  NtfsTests.cpp
  PieLayoutTests.cpp
  PresenterTests.cpp     # src/Presenter, which only the Linux build has for now
  ScanTests.cpp
  SettingsTests.cpp
  ThemeTests.cpp
  TreeTests.cpp
  UtilityTests.cpp
  ViewHistoryTests.cpp)
target_precompile_headers(CoreTests PRIVATE pch.h)
target_compile_definitions(CoreTests PRIVATE SPACEWINDOW_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
target_link_libraries(CoreTests PRIVATE SpaceWindowPresenter spacewindow_options)

add_test(NAME CoreTests COMMAND CoreTests)
set_tests_properties(CoreTests PROPERTIES TIMEOUT 300)
# The benchmark, on a small folder that is always there: so that it keeps working.
add_test(NAME CoreBench COMMAND CoreTests --bench ${PROJECT_SOURCE_DIR}/src 1 2)
set_tests_properties(CoreBench PROPERTIES TIMEOUT 120)
