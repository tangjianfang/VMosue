add_library(vmosue_ui STATIC
  OverlayWindow.h
  OverlayWindow.cpp
)
target_include_directories(vmosue_ui PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(vmosue_ui PUBLIC vmosue_util d2d1 dwrite)
