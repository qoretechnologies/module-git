cmake_minimum_required(VERSION 3.5...3.31)

project(qore-git-module VERSION 1.0.0)

include(CheckCXXCompilerFlag)
include(CheckCXXSymbolExists)

find_package(Qore 2.0 REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBGIT2 REQUIRED libgit2)

set(CMAKE_THREAD_PREFER_PTHREAD ON)
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)
if(CMAKE_USE_PTHREADS_INIT)
    message(STATUS "Found POSIX Threads: TRUE")
else()
    message(STATUS "Found POSIX Threads: FALSE")
    message(FATAL_ERROR "POSIX threads do not seem to be supported on this platform, aborting")
endif()

include_directories(${LIBGIT2_INCLUDE_DIRS})
include_directories(${CMAKE_SOURCE_DIR}/src)
include_directories(${CMAKE_BINARY_DIR})

# Check for C++11.
CHECK_CXX_COMPILER_FLAG("-std=c++11" COMPILER_SUPPORTS_CXX11)
if(COMPILER_SUPPORTS_CXX11)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11")
else()
    message(FATAL_ERROR "The compiler ${CMAKE_CXX_COMPILER} has no C++11 support. Please use a different C++ compiler.")
endif()

# Check for optional libgit2 API symbols
set(CMAKE_REQUIRED_INCLUDES ${LIBGIT2_INCLUDE_DIRS})
set(CMAKE_REQUIRED_LIBRARIES ${LIBGIT2_LDFLAGS})
check_cxx_symbol_exists(git_clone_options_init git2.h HAVE_GIT_CLONE_OPTIONS_INIT)
check_cxx_symbol_exists(git_diff_options_init git2.h HAVE_GIT_DIFF_OPTIONS_INIT)
check_cxx_symbol_exists(git_status_options_init git2.h HAVE_GIT_STATUS_OPTIONS_INIT)
check_cxx_symbol_exists(git_checkout_options_init git2.h HAVE_GIT_CHECKOUT_OPTIONS_INIT)
check_cxx_symbol_exists(git_merge_options_init git2.h HAVE_GIT_MERGE_OPTIONS_INIT)
check_cxx_symbol_exists(git_stash_save_with_opts git2.h HAVE_GIT_STASH_SAVE_WITH_OPTS)
check_cxx_symbol_exists(git_blame_file git2.h HAVE_GIT_BLAME_FILE)
check_cxx_symbol_exists(git_index_conflict_get git2.h HAVE_GIT_INDEX_CONFLICT_GET)
check_cxx_symbol_exists(git_credential_ssh_key_memory_new git2.h HAVE_GIT_CREDENTIAL_SSH_KEY_MEMORY_NEW)
check_cxx_symbol_exists(git_signature_default_from_env git2.h HAVE_GIT_SIGNATURE_DEFAULT_FROM_ENV)
check_cxx_symbol_exists(git_remote_connect_ext git2.h HAVE_GIT_REMOTE_CONNECT_EXT)
unset(CMAKE_REQUIRED_INCLUDES)
unset(CMAKE_REQUIRED_LIBRARIES)

check_cxx_compiler_flag(-fvisibility=hidden HAVE_GCC_VISIBILITY)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_definitions(-D_GNU_SOURCE)
endif()

configure_file(${CMAKE_SOURCE_DIR}/cmake/config.h.cmake
               ${CMAKE_BINARY_DIR}/config.h)

set(QPP_SRC
    src/QC_GitRepository.qpp
)

set(CPP_SRC
    src/git-module.cpp
    src/QoreGitRepository.cpp
)

qore_wrap_qpp_value(QPP_SOURCES DOXLIST _dox_src METALIST QPP_META_SRC ${QPP_SRC})

set(module_name "git")

set(QORE_DOX_TMPL_SRC
    docs/mainpage.dox.tmpl
)

if (DEFINED ENV{DOXYGEN_EXECUTABLE})
    set(DOXYGEN_EXECUTABLE $ENV{DOXYGEN_EXECUTABLE})
endif()

add_library(${module_name} MODULE ${CPP_SRC} ${QPP_SOURCES})
set(MODULE_DOX_INPUT ${CMAKE_BINARY_DIR}/mainpage.dox ${_dox_src})
qore_external_binary_module(${module_name} ${PROJECT_VERSION} ${LIBGIT2_LDFLAGS})

qore_external_user_module("qlib/GitDataProvider" "")
qore_external_user_module("qlib/GitConfigManager" "GitDataProvider")
qore_external_user_module("qlib/GitForgeHelper" "")
qore_external_user_module("qlib/GitConnections.qm" "")

qore_dist(${PROJECT_VERSION})

# install qpp metadata files
if (COMMAND qore_install_qpp_metadata)
    qore_install_qpp_metadata(${module_name} ${QPP_META_SRC})
else()
    install(FILES ${QPP_META_SRC}
            DESTINATION ${CMAKE_INSTALL_DATADIR}/qore/metadata)
endif()

qore_config_info()

if (DOXYGEN_FOUND)
    qore_wrap_dox(QORE_DOX_SRC ${QORE_DOX_TMPL_SRC})
    add_custom_target(QORE_MOD_DOX_FILES DEPENDS ${QORE_DOX_SRC})
    add_dependencies(docs-module QORE_MOD_DOX_FILES)
endif()
