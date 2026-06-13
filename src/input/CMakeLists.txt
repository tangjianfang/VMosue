add_library(vmosue_input STATIC
  InputInjector.h
  InputInjector.cpp
)
target_include_directories(vmosue_input PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(vmosue_input PUBLIC vmosue_util)