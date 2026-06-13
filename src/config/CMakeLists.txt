add_library(vmosue_config STATIC
  Calibration.h
  Calibration.cpp
  Config.h
  Config.cpp
)
target_include_directories(vmosue_config PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(vmosue_config PUBLIC vmosue_util nlohmann_json::nlohmann_json)
