# Native C++/WinRT entry point and the shared upstream ImGui application.
add_executable(xenia-uwp WIN32
  App.cpp
  UWPUtil.cpp
  WinRTKeyboard.cpp
  XeniaUWP.cpp
  surface_uwp.cpp
  window_uwp.cc
  windowed_app_context_uwp.cc
  file_picker_uwp.cc
  menu_item_uwp.cc
  Package.appxmanifest
  ${PROJECT_SOURCE_DIR}/src/xenia/app/xenia_main.cc
)

# Keep all upstream app dialogs, including netplay, in the packaged app.
xe_platform_sources(xenia-uwp ${PROJECT_SOURCE_DIR}/src/xenia/app)
target_include_directories(xenia-uwp PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(xenia-uwp PRIVATE
  XBYAK_NO_OP_NAMES XBYAK_ENABLE_OMITTED_OPERAND CURL_STATICLIB MINIUPNP_STATICLIB
)
target_compile_options(xenia-uwp PRIVATE /bigobj)
target_link_libraries(xenia-uwp PRIVATE
  xenia-apu xenia-apu-nop xenia-apu-xaudio2
  xenia-base xenia-core xenia-cpu xenia-cpu-backend-x64
  xenia-gpu xenia-gpu-d3d12
  xenia-hid xenia-hid-nop xenia-hid-xinput
  xenia-kernel xenia-patcher xenia-ui xenia-ui-d3d12 xenia-vfs
  xenia-debug-ui xenia-app-discord
  aes_128 capstone fmt dxbc glslang-spirv imgui libavcodec libavutil mspack snappy xxhash
  libcurl miniupnp
  windowsapp runtimeobject
)

set_target_properties(xenia-uwp PROPERTIES
  OUTPUT_NAME xeniacanary
  VS_WINRT_COMPONENT FALSE
  VS_GLOBAL_RootNamespace xenia_canary_uwp
  VS_GLOBAL_MinimalCoreWin true
  VS_GLOBAL_DefaultLanguage en-US
  VS_GLOBAL_GenerateWindowsMetadata false
  VS_GLOBAL_AppxPackageSigningEnabled false
  VS_GLOBAL_AppxAutoIncrementPackageRevision false
  VS_GLOBAL_AppxBundle Never
)

# Relative deployment locations preserve the exact paths the Blades UI loads.
file(GLOB_RECURSE _uwp_assets CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/Assets/*")
foreach(asset IN LISTS _uwp_assets)
  file(RELATIVE_PATH relative_asset "${CMAKE_CURRENT_SOURCE_DIR}" "${asset}")
  get_filename_component(asset_directory "${relative_asset}" DIRECTORY)
  set_source_files_properties("${asset}" PROPERTIES
    VS_DEPLOYMENT_CONTENT 1
    VS_DEPLOYMENT_LOCATION "${asset_directory}"
  )
endforeach()
target_sources(xenia-uwp PRIVATE ${_uwp_assets})
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${_uwp_assets})

# Ship the license for the TLS dependency used by the UWP package.
set(_uwp_tls_license "${PROJECT_SOURCE_DIR}/third_party/wolfssl/COPYING")
set_source_files_properties("${_uwp_tls_license}" PROPERTIES
  VS_DEPLOYMENT_CONTENT 1
  VS_DEPLOYMENT_LOCATION "Licenses/wolfSSL"
)
target_sources(xenia-uwp PRIVATE "${_uwp_tls_license}")
