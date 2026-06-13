add_library(vmosue_capture STATIC
  Frame.h
  CameraCapture.h
  CameraCapture.cpp
)
target_include_directories(vmosue_capture PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(vmosue_capture PUBLIC
  vmosue_util
  mfreadwrite
  mfplat
  mfuuid
  mfuuid
)
