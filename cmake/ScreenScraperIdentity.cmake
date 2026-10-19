# Optional developer credentials are supplied in a private, untracked header.
# Never put their values in compile definitions, CMake cache entries or logs.
set(XENIA_SCREENSCRAPER_IDENTITY_HEADER "" CACHE FILEPATH
    "Private header defining XENIA_SCREENSCRAPER_DEV_ID and DEV_PASSWORD")

function(xenia_screenscraper_identity target)
  if(XENIA_SCREENSCRAPER_IDENTITY_HEADER)
    if(NOT EXISTS "${XENIA_SCREENSCRAPER_IDENTITY_HEADER}")
      message(FATAL_ERROR "The private ScreenScraper identity header is missing")
    endif()
    set(identity_dir "${CMAKE_CURRENT_BINARY_DIR}/private-identity")
    file(MAKE_DIRECTORY "${identity_dir}/xenia")
    configure_file("${XENIA_SCREENSCRAPER_IDENTITY_HEADER}"
      "${identity_dir}/xenia/screenscraper_build_config.h" COPYONLY)
    # Only this translation unit needs access to the private header. The header
    # is not a deployment asset and does not enter the package source archive.
    set_property(SOURCE "${PROJECT_SOURCE_DIR}/src/xenia/app/screenscraper_build_identity.cc"
      APPEND PROPERTY INCLUDE_DIRECTORIES "${identity_dir}")
  endif()
endfunction()
