get_directory_property(isPlugin PARENT_DIRECTORY)

project(webfort)

# ----------------------------------------------------------------------------
# Phase 2 port: WebSocket server + direct screen capture. Input injection
# (input.hpp) is still stubbed — finalized in Phase 3.
# ----------------------------------------------------------------------------

set(SRC
    webfort.cpp
    server.cpp
    config.cpp
    ${CMAKE_SOURCE_DIR}/depends/lodepng/lodepng.cpp
)

set(HDRS
    webfort.hpp
    server.hpp
    config.hpp
    input.hpp
)
set_source_files_properties(${HDRS} PROPERTIES HEADER_FILE_ONLY TRUE)
list(APPEND SRC ${HDRS})

# Use vendored standalone Asio (header-only, no Boost required).
# The asio/ subdirectory contains asio.hpp + asio/ extracted from
# https://github.com/chriskohlhoff/asio (tag asio-1-28-0).

if (isPlugin)
    # Platform-specific link libraries for Asio networking.
    if (WIN32)
        set(WF_PLATFORM_LIBS ws2_32 mswsock)
    else()
        set(WF_PLATFORM_LIBS pthread)
    endif()

    DFHACK_PLUGIN(webfort ${SRC}
        LINK_LIBRARIES ${WF_PLATFORM_LIBS}
    )
    target_include_directories(webfort PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/websocketpp
        ${CMAKE_CURRENT_SOURCE_DIR}/asio      # standalone Asio headers
        ${SDL2_INCLUDE_DIRS}
        ${CMAKE_SOURCE_DIR}/depends/lodepng   # lodepng single-file PNG encoder
    )
    target_compile_definitions(webfort PRIVATE
        ASIO_STANDALONE                # websocketpp: use standalone Asio, not Boost
        _WEBSOCKETPP_CPP11_STL_
    )
    # Standalone Asio on Windows needs a minimum Windows version.
    if (WIN32)
        target_compile_definitions(webfort PRIVATE
            _WIN32_WINNT=0x0601   # Windows 7+
            NOMINMAX              # prevent windows.h min/max macros
        )
    endif()
    # websocketpp 0.8.x emits warnings that -Werror (inherited from DFHack)
    # would otherwise turn into failures.
    if (NOT MSVC)
        target_compile_options(webfort PRIVATE
            -Wno-error=deprecated-declarations
            -Wno-error=unused-result
            -Wno-error=unused-variable
            -Wno-error=unused-but-set-variable
            -Wno-error=sign-compare
            -Wno-error=parentheses
            -Wno-error=uninitialized
            -Wno-error=maybe-uninitialized
        )
    else()
        # Suppress equivalent MSVC warnings from websocketpp headers and lodepng.
        target_compile_options(webfort PRIVATE
            /wd4267   # conversion from size_t to smaller type
            /wd4244   # conversion from larger to smaller integer type
            /wd4996   # deprecated function use
            /wd4127   # conditional expression is constant
            /wd4334   # 32-bit shift implicitly converted to 64 bits (lodepng)
        )
    endif()
else()
    message(FATAL_ERROR "webfort must be built as a DFHack plugin.")
endif()

# Install static frontend assets alongside the plugin so the embedded HTTP
# handler can serve them from <hack>/webfort/static/.
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/../static/"
    DESTINATION "${DFHACK_DATA_DESTINATION}/webfort/static"
)
