add_library(vmosue_util STATIC
  Result.h
  Logger.h
  OneEuroFilter.h
  Kalman1D.h
  I18n.h
  I18n.cpp
)
target_include_directories(vmosue_util PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(vmosue_util PUBLIC spdlog::spdlog nlohmann_json::nlohmann_json)