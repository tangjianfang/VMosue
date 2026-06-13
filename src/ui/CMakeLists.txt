add_library(vmosue_ui STATIC
  DebugWindow.h
  DebugWindow.cpp
  OverlayWindow.h
  OverlayWindow.cpp
  SettingsWindow.h
  SettingsWindow.cpp
  TutorialWindow.h
  TutorialWindow.cpp
  TrayIcon.h
  TrayIcon.cpp
)
target_include_directories(vmosue_ui PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(vmosue_ui PUBLIC vmosue_util vmosue_config d2d1 dwrite)
