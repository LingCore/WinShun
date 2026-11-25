# libarchive (BSD-2-Clause) for the archive search plugin (src/archive-plugin):
# reads zip, 7z, rar, tar, cab, iso and more through one interface. Built from
# its release tarball, with the two libraries the formats cannot do without:
# zlib (gzip, deflate) and liblzma from XZ Utils (0BSD; a 7z's list of files
# is LZMA-compressed, and .tar.xz). Static libraries with the C runtime
# linked in, as the plugin's program has it; everything else libarchive can
# use (bzip2, zstd, OpenSSL, libxml2, iconv...) is left out.
#
# The three are built as external projects, when the plugin is built, each
# into ${CMAKE_BINARY_DIR}/_deps/libarchive (their tarballs are downloaded
# into ${CMAKE_BINARY_DIR}/_deps with a pinned SHA256, through the GitHub
# mirrors; a tarball already there is not downloaded again).
#
#   winshun_libarchive()   # defines the target `wsarchive_deps` to link to

include(ExternalProject)

set(WS_ZLIB_VERSION 1.4)
set(WS_ZLIB_SHA256 4f11c512053d7bab8d9e9b89bde3575add3e4c50a383b42ef6cb837035fc5a7c)
set(WS_XZ_VERSION 5.8.4)
set(WS_XZ_SHA256 4ce24038fd4221e0d13bc1a2de7a4db56e90b92b3bf75321f6c14be73f65de4b)
set(WS_LIBARCHIVE_VERSION 3.8.11)
set(WS_LIBARCHIVE_SHA256 a3dc0872b1b39da34dc18ef1b944f0f11ffd37425d70ae04be679279f1cf43d2)

function(winshun_libarchive)
    if(TARGET wsarchive_deps)
        return()
    endif()
    set(downloads "${CMAKE_BINARY_DIR}/_deps")
    set(root "${downloads}/libarchive")
    set(prefix "${root}/prefix")
    file(MAKE_DIRECTORY "${prefix}/include") # an imported target's include folder has to exist at configure time

    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(runtime MultiThreadedDebug)
        set(zlib_name libzsd)
    else()
        set(runtime MultiThreaded)
        set(zlib_name libzs)
    endif()
    set(common
        -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
        -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
        -DCMAKE_INSTALL_PREFIX=${prefix}
        -DCMAKE_PREFIX_PATH=${prefix}
        -DCMAKE_POLICY_DEFAULT_CMP0091=NEW
        -DCMAKE_MSVC_RUNTIME_LIBRARY=${runtime}
        -DBUILD_SHARED_LIBS=OFF
        -DBUILD_TESTING=OFF)
    set(zlib_library "${prefix}/lib/${zlib_name}.lib")
    set(lzma_library "${prefix}/lib/lzma.lib")
    set(archive_library "${prefix}/lib/archive.lib")

    winshun_download_sources(
        "https://github.com/madler/zlib/releases/download/v${WS_ZLIB_VERSION}/zlib-${WS_ZLIB_VERSION}.tar.xz" zlib_urls)
    ExternalProject_Add(wsarchive_zlib
        URL ${zlib_urls}
        URL_HASH SHA256=${WS_ZLIB_SHA256}
        DOWNLOAD_DIR "${downloads}"
        PREFIX "${root}/zlib" SOURCE_DIR "${root}/zlib/s" BINARY_DIR "${root}/zlib/b"
        CMAKE_ARGS ${common} -DZLIB_BUILD_SHARED=OFF -DZLIB_BUILD_TESTING=OFF -DZLIB_BUILD_MINIZIP=OFF
        BUILD_BYPRODUCTS "${zlib_library}"
        EXCLUDE_FROM_ALL TRUE)

    winshun_download_sources(
        "https://github.com/tukaani-project/xz/releases/download/v${WS_XZ_VERSION}/xz-${WS_XZ_VERSION}.tar.xz" xz_urls)
    ExternalProject_Add(wsarchive_xz
        URL ${xz_urls}
        URL_HASH SHA256=${WS_XZ_SHA256}
        DOWNLOAD_DIR "${downloads}"
        PREFIX "${root}/xz" SOURCE_DIR "${root}/xz/s" BINARY_DIR "${root}/xz/b"
        CMAKE_ARGS ${common} -DXZ_NLS=OFF -DXZ_DOC=OFF -DXZ_TOOL_XZ=OFF -DXZ_TOOL_XZDEC=OFF -DXZ_TOOL_LZMADEC=OFF
            -DXZ_TOOL_LZMAINFO=OFF -DXZ_TOOL_SCRIPTS=OFF -DXZ_THREADS=no
        BUILD_BYPRODUCTS "${lzma_library}"
        EXCLUDE_FROM_ALL TRUE)

    winshun_download_sources(
        "https://github.com/libarchive/libarchive/releases/download/v${WS_LIBARCHIVE_VERSION}/libarchive-${WS_LIBARCHIVE_VERSION}.tar.xz"
        archive_urls)
    ExternalProject_Add(wsarchive_libarchive
        URL ${archive_urls}
        URL_HASH SHA256=${WS_LIBARCHIVE_SHA256}
        DOWNLOAD_DIR "${downloads}"
        PREFIX "${root}/archive" SOURCE_DIR "${root}/archive/s" BINARY_DIR "${root}/archive/b"
        CMAKE_ARGS ${common} -DMSVC_USE_STATIC_CRT=ON -DENABLE_WERROR=OFF -DENABLE_TEST=OFF -DENABLE_TAR=OFF
            -DENABLE_CPIO=OFF -DENABLE_CAT=OFF -DENABLE_UNZIP=OFF -DENABLE_ACL=OFF -DENABLE_XATTR=OFF
            -DENABLE_ICONV=OFF -DENABLE_OPENSSL=OFF -DENABLE_MBEDTLS=OFF -DENABLE_NETTLE=OFF -DENABLE_LIBB2=OFF
            -DENABLE_LZ4=OFF -DENABLE_LZO=OFF -DENABLE_ZSTD=OFF -DENABLE_BZip2=OFF -DENABLE_LIBXML2=OFF
            -DENABLE_EXPAT=OFF -DENABLE_BSDXML=OFF -DENABLE_WIN32_XMLLITE=OFF -DENABLE_PCREPOSIX=OFF -DENABLE_PCRE2POSIX=OFF
            -DENABLE_LIBGCC=OFF -DPOSIX_REGEX_LIB=NONE -DENABLE_ZLIB=ON -DENABLE_LZMA=ON
            -DZLIB_INCLUDE_DIR=${prefix}/include -DZLIB_LIBRARY=${zlib_library}
            -DLIBLZMA_INCLUDE_DIR=${prefix}/include -DLIBLZMA_LIBRARY=${lzma_library}
        DEPENDS wsarchive_zlib wsarchive_xz
        BUILD_BYPRODUCTS "${archive_library}"
        EXCLUDE_FROM_ALL TRUE)

    add_library(wsarchive_deps INTERFACE)
    add_dependencies(wsarchive_deps wsarchive_libarchive)
    target_include_directories(wsarchive_deps SYSTEM INTERFACE "${prefix}/include")
    target_compile_definitions(wsarchive_deps INTERFACE LIBARCHIVE_STATIC)
    # bcrypt: the AES of encrypted zip and 7z entries (CNG, Windows' own).
    target_link_libraries(wsarchive_deps INTERFACE "${archive_library}" "${lzma_library}" "${zlib_library}" bcrypt)
    set(WS_LIBARCHIVE_LICENSES
        "${root}/archive/s/COPYING"
        "${root}/xz/s/COPYING.0BSD"
        "${root}/zlib/s/LICENSE"
        PARENT_SCOPE)
endfunction()
