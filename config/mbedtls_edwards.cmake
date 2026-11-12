# The pinned EdDSA fork rejects valid seeds whose most significant byte is zero.
# RFC 8032 seeds are fixed-length byte strings, not minimum-length integers.
# Keep the upstream checkout intact and compile the same corrected source on
# desktop, RP2350 and ESP32. Fail configuration if upstream changes these checks.
set(_edwards_input "${CMAKE_CURRENT_LIST_DIR}/../third-party/mbedtls/library/ecp.c")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_edwards_input}")
file(READ "${_edwards_input}" _edwards_source)
foreach(_seed_bytes IN ITEMS 32 57)
    set(_before "if (mbedtls_mpi_size(d) != ${_seed_bytes})")
    string(FIND "${_edwards_source}" "${_before}" _check_offset)
    if(_check_offset EQUAL -1)
        message(FATAL_ERROR "Re-audit the EdDSA fixed-width seed correction for this MbedTLS revision")
    endif()
    string(REPLACE "${_before}" "if (mbedtls_mpi_size(d) > ${_seed_bytes})" _edwards_source "${_edwards_source}")
endforeach()
set(PICOKEYS_ECP_SOURCE "${CMAKE_BINARY_DIR}/picokeys-crypto/ecp.c")
set(_edwards_existing "")
if(EXISTS "${PICOKEYS_ECP_SOURCE}")
    file(READ "${PICOKEYS_ECP_SOURCE}" _edwards_existing)
endif()
if(NOT _edwards_source STREQUAL _edwards_existing)
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/picokeys-crypto")
    file(WRITE "${PICOKEYS_ECP_SOURCE}" "${_edwards_source}")
endif()
