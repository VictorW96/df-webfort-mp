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
)

set(HDRS
    webfort.hpp
    server.hpp
    config.hpp
    input.hpp
)
set_source_files_properties(${HDRS} PROPERTIES HEADER_FILE_ONLY TRUE)
list(APPEND SRC ${HDRS})

# Linux only for now.
if(NOT UNIX)
    message(FATAL_ERROR "webfort only builds on Linux in this port.")
endif()

# Boost + Asio for vendored websocketpp.
set(Boost_USE_STATIC_LIBS ON)
find_package(Boost 1.39.0 REQUIRED COMPONENTS system thread regex)

if (isPlugin)
    DFHACK_PLUGIN(webfort ${SRC}
        LINK_LIBRARIES ${Boost_LIBRARIES} pthread
    )
    target_include_directories(webfort PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/websocketpp
        ${Boost_INCLUDE_DIRS}
        ${SDL2_INCLUDE_DIRS}
    )
    target_compile_definitions(webfort PRIVATE
        LINUX_BUILD
        _WEBSOCKETPP_CPP11_STL_
    )
    # websocketpp 0.8.x emits warnings that -Werror (inherited from DFHack)
    # would otherwise turn into failures.
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
    message(FATAL_ERROR "webfort must be built as a DFHack plugin.")
endif()

# Install static frontend assets alongside the plugin so the embedded HTTP
# handler can serve them from <hack>/webfort/static/.
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/../static/"
    DESTINATION "${DFHACK_DATA_DESTINATION}/webfort/static"
)
