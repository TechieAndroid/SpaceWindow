# A smoke test of the Qt app, driving its real window (QTest) on Qt's offscreen
# platform: at 100 %, and again at 125 % and 200 %, where the map's pixel
# arithmetic has rounding to get right.
add_executable(AppQtSmokeTest SmokeTest.cpp SmokeTest.h)
set_target_properties(AppQtSmokeTest PROPERTIES AUTOMOC ON)
target_link_libraries(AppQtSmokeTest PRIVATE SpaceWindowQt Qt6::Test spacewindow_options)
foreach(scale 1 1.25 2)
  if(scale STREQUAL "1")
    set(name AppQtSmokeTest)
  else()
    set(name AppQtSmokeTest@${scale}x)
  endif()
  add_test(NAME ${name} COMMAND AppQtSmokeTest)
  set_tests_properties(${name} PROPERTIES TIMEOUT 120 ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=${scale}")
endforeach()
