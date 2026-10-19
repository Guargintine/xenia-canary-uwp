# Apply RSA buffer bounds checks without modifying the pinned submodule.
function(xenia_configure_uwp_wolfssl target source_dir overlay_dir)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  get_filename_component(_support_root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/.." ABSOLUTE)
  set(_patch "${_support_root}/third_party/wolfssl-xenia/patches/rsa-size-bounds.patch")
  set(_script "${_support_root}/tools/build/apply_wolfssl_overlay.py")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${_patch}" "${_script}"
    "${source_dir}/wolfcrypt/src/rsa.c"
    "${source_dir}/wolfssl/wolfcrypt/sp_int.h")
  execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${_script}"
      --source "${source_dir}" --output "${overlay_dir}" --patch "${_patch}"
    RESULT_VARIABLE _patch_result
    OUTPUT_VARIABLE _patch_stdout
    ERROR_VARIABLE _patch_stderr)
  if(NOT _patch_result EQUAL 0)
    message(FATAL_ERROR "Unable to prepare the pinned wolfSSL RSA backport:\n${_patch_stdout}${_patch_stderr}")
  endif()

  get_target_property(_sources "${target}" SOURCES)
  set(_patched_sources)
  set(_rsa_replaced FALSE)
  foreach(_source IN LISTS _sources)
    if(_source STREQUAL "wolfcrypt/src/rsa.c" OR
       _source STREQUAL "${source_dir}/wolfcrypt/src/rsa.c")
      list(APPEND _patched_sources "${overlay_dir}/wolfcrypt/src/rsa.c")
      set(_rsa_replaced TRUE)
    else()
      list(APPEND _patched_sources "${_source}")
    endif()
  endforeach()
  if(NOT _rsa_replaced)
    message(FATAL_ERROR "The wolfSSL target no longer contains the expected rsa.c source")
  endif()
  set_property(TARGET "${target}" PROPERTY SOURCES "${_patched_sources}")
  # PUBLIC also places the overlay and ABI-defining integer size ahead of
  # vendor headers for libcurl and any other wolfSSL consumer.
  target_include_directories("${target}" BEFORE PUBLIC "$<BUILD_INTERFACE:${overlay_dir}>")
  target_compile_definitions("${target}" PUBLIC
    NO_WOLFSSL_DIR WOLFSSL_SP_NO_DYN_STACK SP_INT_BITS=4096)
endfunction()
