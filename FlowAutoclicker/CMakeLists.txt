cmake_minimum_required(VERSION 3.24)
project(FlowAutoclicker LANGUAGES CXX RC)

add_executable(FlowAutoclicker WIN32
  src/main.cpp
  src/app.rc
)

target_compile_features(FlowAutoclicker PRIVATE cxx_std_20)
target_compile_definitions(FlowAutoclicker PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(FlowAutoclicker PRIVATE comctl32 dwmapi gdiplus uxtheme winmm)

if(MSVC)
  set_property(TARGET FlowAutoclicker PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  target_compile_options(FlowAutoclicker PRIVATE /O2 /GL /Oi /Gy /Gw /utf-8 /permissive- /EHsc)
  target_link_options(FlowAutoclicker PRIVATE /LTCG /OPT:REF /OPT:ICF /SUBSYSTEM:WINDOWS)
endif()
