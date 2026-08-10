cmake_minimum_required(VERSION 3.16...3.30)
project(ChatDB VERSION 2.1.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(WIN32)
    add_definitions(-DWIN32_LEAN_AND_MEAN -DNOMINMAX)
    set(PLATFORM_LIBS ws2_32)
else()
    set(PLATFORM_LIBS pthread dl)
endif()

find_package(Threads REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(OpenSSL REQUIRED)

# hiredis: try system first, fallback to FetchContent
set(HIREDIS_LIBS "")
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(HIREDIS hiredis)
    if(HIREDIS_FOUND)
        include_directories(${HIREDIS_INCLUDE_DIRS})
        link_directories(${HIREDIS_LIBRARY_DIRS})
        set(HIREDIS_LIBS ${HIREDIS_LIBRARIES})
        message(STATUS "Found hiredis via pkg-config")
    endif()
endif()

if(NOT HIREDIS_LIBS)
    find_library(HIREDIS_LIB hiredis PATHS /usr/local/lib /usr/lib)
    find_path(HIREDIS_INCLUDE hiredis/hiredis.h PATHS /usr/local/include /usr/include)
    if(HIREDIS_LIB AND HIREDIS_INCLUDE)
        include_directories(${HIREDIS_INCLUDE})
        set(HIREDIS_LIBS ${HIREDIS_LIB})
        message(STATUS "Found hiredis: ${HIREDIS_LIB}")
    else()
        message(STATUS "hiredis not found system-wide, fetching from source...")
        include(FetchContent)
        FetchContent_Declare(hiredis
            GIT_REPOSITORY https://github.com/redis/hiredis.git
            GIT_TAG v1.2.0
            GIT_SHALLOW TRUE
        )
        FetchContent_MakeAvailable(hiredis)
        set(HIREDIS_LIBS hiredis)
        include_directories(${hiredis_SOURCE_DIR})
    endif()
endif()

include_directories(
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party
    ${SQLite3_INCLUDE_DIRS}
)

include(FetchContent)

FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
    GIT_SHALLOW TRUE
)
FetchContent_Declare(fmt
    GIT_REPOSITORY https://github.com/fmtlib/fmt.git
    GIT_TAG 10.2.1
    GIT_SHALLOW TRUE
)
FetchContent_Declare(spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG v1.13.0
    GIT_SHALLOW TRUE
)
FetchContent_Declare(httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG v0.15.3
    GIT_SHALLOW TRUE
)
FetchContent_Declare(ixwebsocket
    GIT_REPOSITORY https://github.com/machinezone/ixwebsocket.git
    GIT_TAG v11.4.5
    GIT_SHALLOW TRUE
)

FetchContent_MakeAvailable(nlohmann_json fmt spdlog httplib ixwebsocket)

set(CHATDB_SOURCES
    src/config.cpp
    src/sqlite_storage.cpp
    src/sqlite_storage_backup.cpp
    src/redis_client.cpp
    src/ollama_client.cpp
    src/message_processor.cpp
    src/query_engine.cpp
    src/chat_database.cpp
    src/port_detector.cpp
    src/http_server.cpp
    src/onebot_v11_client.cpp
    src/embedding_provider.cpp
    src/memory_summarizer.cpp
    src/process_guard.cpp
    src/main.cpp
)

add_executable(chatdb ${CHATDB_SOURCES})

target_link_libraries(chatdb PRIVATE
    SQLite::SQLite3
    nlohmann_json::nlohmann_json
    fmt::fmt
    spdlog::spdlog
    ${HIREDIS_LIBS}
    Threads::Threads
    ${PLATFORM_LIBS}
    OpenSSL::SSL
    OpenSSL::Crypto
)

if(TARGET httplib)
    target_link_libraries(chatdb PRIVATE httplib)
    target_include_directories(chatdb PRIVATE ${httplib_SOURCE_DIR})
endif()

if(TARGET ixwebsocket)
    target_link_libraries(chatdb PRIVATE ixwebsocket)
endif()

if(MSVC)
    target_compile_options(chatdb PRIVATE /W4 /WX-)
else()
    target_compile_options(chatdb PRIVATE -Wall -Wextra -Wpedantic -O2)
endif()

install(TARGETS chatdb DESTINATION bin)
install(FILES chatdb.conf DESTINATION . RENAME chatdb.conf.example)
