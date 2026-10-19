add_library(xenia-base STATIC)
xe_platform_sources(xenia-base ${CMAKE_CURRENT_SOURCE_DIR})
# Exclude console_app_main and main_init files
file(GLOB _base_exclude   "${CMAKE_CURRENT_SOURCE_DIR}/console_app_main_*.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/main_init_*.cc"
)
get_target_property(_base_srcs xenia-base SOURCES)
if(_base_exclude AND _base_srcs)
  list(REMOVE_ITEM _base_srcs ${_base_exclude})
  set_target_properties(xenia-base PROPERTIES SOURCES "${_base_srcs}")
endif()
target_link_libraries(xenia-base PUBLIC fmt libcurl)
xe_target_defaults(xenia-base)

if(XENIA_BUILD_UWP)
  # Native Windows memory APIs preserve upstream's single-address JIT mapping.
  set_property(TARGET xenia-base PROPERTY VS_GLOBAL__NoWinAPIFamilyApp true)
  set(_ca_bundle "${PROJECT_SOURCE_DIR}/third_party/cacert/cacert.pem")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_ca_bundle}")
  file(READ "${_ca_bundle}" _ca_bundle_hex HEX)
  string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _ca_bundle_bytes "${_ca_bundle_hex}")
  string(REPLACE "0x0a," "0x0a,\n" _ca_bundle_bytes "${_ca_bundle_bytes}")
  file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/xenia_ca_bundle.inc"
    CONTENT "${_ca_bundle_bytes}\n")
  target_include_directories(xenia-base PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
endif()

if(XENIA_BUILD_TESTS)
  set(CMAKE_FOLDER "tests")
  add_subdirectory(testing)
endif()
