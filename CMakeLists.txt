
cmake_minimum_required(VERSION 3.5...3.31)

project(qore-pgsql-module)

set (VERSION_MAJOR 3)
set (VERSION_MINOR 5)
set (VERSION_PATCH 0)

set(PROJECT_VERSION "${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_PATCH}")

option(enable-scu "build as a single compilation unit" ON)
option(server-includes "build with pgsql server includes" OFF)

include(CheckCXXCompilerFlag)
include(CheckCXXSourceCompiles)
include(CheckCXXSymbolExists)
include(CheckIncludeFileCXX)

# Check for C++11.
CHECK_CXX_COMPILER_FLAG("-std=c++11" COMPILER_SUPPORTS_CXX11)
if(COMPILER_SUPPORTS_CXX11)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11")
else()
    message(FATAL_ERROR "The compiler ${CMAKE_CXX_COMPILER} has no C++11 support. Please use a different C++ compiler.")
endif()

find_package(Qore 0.9 REQUIRED)
qore_find_pthreads()

if (DEFINED ENV{PostgreSQL_INCLUDE_DIR})
    set(PostgreSQL_INCLUDE_DIR $ENV{PostgreSQL_INCLUDE_DIR})
endif()
if (DEFINED ENV{PostgreSQL_TYPE_INCLUDE_DIR})
    set(PostgreSQL_TYPE_INCLUDE_DIR $ENV{PostgreSQL_TYPE_INCLUDE_DIR})
endif()
if (DEFINED ENV{PostgreSQL_LIBRARY})
    set(PostgreSQL_LIBRARY $ENV{PostgreSQL_LIBRARY})
endif()

find_package(PostgreSQL 8.0.0 REQUIRED)

# check if the client library has PQLibVersion
set(CMAKE_REQUIRED_INCLUDES ${PostgreSQL_INCLUDE_DIRS})
set(CMAKE_REQUIRED_LIBRARIES ${PostgreSQL_LIBRARY})
check_cxx_symbol_exists(PQlibVersion libpq-fe.h HAVE_PQLIBVERSION)
unset(CMAKE_REQUIRED_INCLUDES)
unset(CMAKE_REQUIRED_LIBRARIES)

check_include_file_cxx(arpa/inet.h HAVE_ARPA_INET_H)
check_include_file_cxx(netinet/in.h HAVE_NETINET_IN_H)

include_directories(${CMAKE_SOURCE_DIR}/src)
include_directories(${PostgreSQL_INCLUDE_DIRS})
add_definitions(-DQORE_MODULE_PACKAGE_VERSION="${PROJECT_VERSION}")

check_cxx_compiler_flag(-fvisibility=hidden HAVE_GCC_VISIBILITY)

configure_file(${CMAKE_SOURCE_DIR}/cmake/config.h.cmake
               ${CMAKE_BINARY_DIR}/config.h)

set(QPP_SRC src/ql_pgsql.qpp)

set(CPP_SRC
    src/pgsql.cpp
    src/QorePGConnection.cpp
    src/QorePGMapper.cpp
)

qore_wrap_qpp_value(QPP_SOURCES METALIST QPP_META_SRC DOXLIST QPP_DOX ${QPP_SRC})
add_custom_target(QPP_GENERATED_FILES DEPENDS ${QPP_SOURCES})

set(module_name "pgsql")

set(QORE_DOX_TMPL_SRC
  docs/mainpage.dox.tmpl
)

if(enable-scu)
    add_library(${module_name} MODULE src/single-compilation-unit.cpp)
    add_dependencies(${module_name} QPP_GENERATED_FILES)
else(enable-scu)
    add_library(${module_name} MODULE ${CPP_SRC} ${QPP_SOURCES})
endif(enable-scu)

if(WIN32 OR MINGW OR MSYS)
    target_compile_definitions(${module_name} PUBLIC BUILDING_DLL)
endif()

if(server-includes)
    target_compile_definitions(${module_name} PUBLIC POSTGRESQL_SERVER_INCLUDES)
endif(server-includes)

if (DEFINED ENV{DOXYGEN_EXECUTABLE})
    set(DOXYGEN_EXECUTABLE $ENV{DOXYGEN_EXECUTABLE})
endif()

list(APPEND QORE_DOXYGEN_MODULES BulkSqlUtil)
set(MODULE_DOX_INPUT ${CMAKE_CURRENT_BINARY_DIR}/mainpage.dox ${QPP_DOX})
qore_external_binary_module(${module_name} ${PROJECT_VERSION} ${PostgreSQL_LIBRARY} Threads::Threads)

qore_dist(${PROJECT_VERSION})

# install qpp metadata files
if (COMMAND qore_install_qpp_metadata)
    qore_install_qpp_metadata(${module_name} ${QPP_META_SRC})
else()
    install(FILES ${QPP_META_SRC}
            DESTINATION ${CMAKE_INSTALL_DATADIR}/qore/metadata)
endif()

qore_config_info()

# NOTE: find_package(Doxygen) called in qore_external_binary_module()
if (DOXYGEN_FOUND)
    qore_wrap_dox(QORE_DOX_SRC ${QORE_DOX_TMPL_SRC})
    add_custom_target(QORE_MOD_DOX_FILES DEPENDS ${QORE_DOX_SRC})
    add_dependencies(docs-module QORE_MOD_DOX_FILES)
endif()
