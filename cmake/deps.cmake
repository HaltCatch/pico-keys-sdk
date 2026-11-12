include(FetchContent)

option(ENABLE_EDDSA "Enable/disable EdDSA support" OFF)
configure_bool_option(
    ENABLE_EDDSA
    ""
    "EdDSA support:\t\t enabled"
    "EdDSA support:\t\t disabled"
)

set(MBEDTLS_PATH "${CMAKE_CURRENT_LIST_DIR}/../third-party/mbedtls")
set(TINYCBOR_PATH "${CMAKE_CURRENT_LIST_DIR}/../third-party/tinycbor")
set(CJSON_PATH "${CMAKE_CURRENT_LIST_DIR}/../third-party/cjson")
set(MLKEM_PATH "${CMAKE_CURRENT_LIST_DIR}/../third-party/mlkem")
set(MLDSA_PATH "${CMAKE_CURRENT_LIST_DIR}/../third-party/mldsa")
set(LIBCVC_PATH "${CMAKE_CURRENT_LIST_DIR}/../third-party/libcvc")

set(PICOKEYS_MBEDTLS_EDDSA_REPO "https://github.com/polhenarejos/mbedtls.git")
set(PICOKEYS_MBEDTLS_EDDSA_REF "v3.6.7-eddsa")

set(PICOKEYS_TINYCBOR_REPO "https://github.com/intel/tinycbor.git")
set(PICOKEYS_TINYCBOR_REF "v7.1")
set(PICOKEYS_CJSON_REPO "https://github.com/DaveGamble/cJSON.git")
set(PICOKEYS_CJSON_REF "v1.7.19")
set(PICOKEYS_MLKEM_REPO "https://github.com/pq-code-package/mlkem-native.git")
set(PICOKEYS_MLKEM_REF "v1.2.0")
set(PICOKEYS_MLDSA_REPO "https://github.com/pq-code-package/mldsa-native.git")
set(PICOKEYS_MLDSA_REF "v1.0.0-beta2")
set(PICOKEYS_LIBCVC_REPO "https://github.com/polhenarejos/libcvc.git")
set(PICOKEYS_LIBCVC_REF "main")

option(PICOKEYS_FETCH_DEPS_ON_DEMAND "Fetch missing optional dependencies" ON)
function(picokeys_sync_dep name repo ref dest)
    set(marker CMakeLists.txt)
    if(name STREQUAL "mlkem_dep")
        set(marker mlkem/src/kem.c)
    elseif(name STREQUAL "mldsa_dep")
        set(marker mldsa/src/sign.c)
    endif()
    if(NOT EXISTS "${dest}/${marker}")
        if(name MATCHES "^(cjson|mlkem|mldsa)_dep$" AND PICOKEYS_FETCH_DEPS_ON_DEMAND)
            FetchContent_Populate(${name}
                GIT_REPOSITORY "${repo}" GIT_TAG "${ref}" GIT_SHALLOW TRUE
                UPDATE_DISCONNECTED TRUE SOURCE_DIR "${dest}"
                BINARY_DIR "${CMAKE_BINARY_DIR}/_deps/${name}-build"
                SUBBUILD_DIR "${CMAKE_BINARY_DIR}/_deps/${name}-subbuild")
        else()
            message(FATAL_ERROR "Missing dependency ${name} at ${dest}: initialize SDK submodules; optional dependencies require PICOKEYS_FETCH_DEPS_ON_DEMAND=ON or pre-populated sources")
        endif()
    endif()
    message(STATUS "[deps] ${name}: pinned source at ${dest}")
endfunction()

picokeys_sync_dep(mbedtls_dep "${PICOKEYS_MBEDTLS_EDDSA_REPO}" "${PICOKEYS_MBEDTLS_EDDSA_REF}" "${MBEDTLS_PATH}")
if(ENABLE_EDDSA)
    if(NOT EXISTS "${MBEDTLS_PATH}/library/eddsa.c")
        message(FATAL_ERROR "EdDSA requires the pinned Pico Keys Mbed TLS source. Run tools/checkout-deps.sh.")
    endif()
    add_compile_definitions(
        MBEDTLS_ECP_DP_ED25519_ENABLED=1
        MBEDTLS_ECP_DP_ED448_ENABLED=1
        MBEDTLS_EDDSA_C=1
        MBEDTLS_SHA3_C=1
    )
elseif(EXISTS "${MBEDTLS_PATH}/library/eddsa.c")
    # This Mbed TLS release references its Edwards helper even without EdDSA.
    add_compile_definitions(MBEDTLS_ECP_DP_ED25519_ENABLED=1)
endif()

if(USB_ITF_HID)
    picokeys_sync_dep(tinycbor_dep "${PICOKEYS_TINYCBOR_REPO}" "${PICOKEYS_TINYCBOR_REF}" "${TINYCBOR_PATH}")
endif()
if(USB_ITF_LWIP)
    picokeys_sync_dep(cjson_dep "${PICOKEYS_CJSON_REPO}" "${PICOKEYS_CJSON_REF}" "${CJSON_PATH}")
endif()
if(ENABLE_PQC)
    picokeys_sync_dep(mlkem_dep "${PICOKEYS_MLKEM_REPO}" "${PICOKEYS_MLKEM_REF}" "${MLKEM_PATH}")
    picokeys_sync_dep(mldsa_dep "${PICOKEYS_MLDSA_REPO}" "${PICOKEYS_MLDSA_REF}" "${MLDSA_PATH}")
endif()
if(ENABLE_LIBCVC)
    picokeys_sync_dep(libcvc_dep "${PICOKEYS_LIBCVC_REPO}" "${PICOKEYS_LIBCVC_REF}" "${LIBCVC_PATH}")
endif()
