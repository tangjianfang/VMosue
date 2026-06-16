add_library(vmosue_capture STATIC
  Frame.h
  CameraCapture.h
  CameraCapture.cpp
)
target_include_directories(vmosue_capture PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(vmosue_capture PUBLIC
  vmosue_util
  mfreadwrite
  Mfplat     # D2D1 / source reader runtime
  Mf         # MFCreateDeviceSource / MFEnumDeviceSources (in this
            # SDK they live in Mf.lib, not in Mfplat.lib)
  mfuuid
)
