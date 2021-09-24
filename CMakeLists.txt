project(cgrpfs)

cmake_minimum_required(VERSION 3.9)
cmake_policy(VERSION 3.9)

include(FindPkgConfig)
include(GNUInstallDirs)

if (CMAKE_SYSTEM_NAME MATCHES "kOpenBSD.*|OpenBSD.*")
	find_package(Threads REQUIRED)
	set(CGRPFS_THREADED 1)
	set(FUSE_LIB fuse Threads::Threads)
	set(CGRPFS_MAIN_SRC cgrpfs_main_threads.c)
else ()
	pkg_check_modules(fuse REQUIRED IMPORTED_TARGET fuse)
	set(FUSE_LIB PkgConfig::fuse)
	set(CGRPFS_MAIN_SRC cgrpfs_main.c)
endif()

add_executable(cgrpfs ${CGRPFS_MAIN_SRC} cgrpfs.c cgrpfs_vnops.c)
target_link_libraries(cgrpfs ${FUSE_LIB})

if (CGRPFS_THREADED)
	target_compile_definitions(cgrpfs PRIVATE -DCGRPFS_THREADED)
endif ()

install(TARGETS cgrpfs DESTINATION ${CMAKE_INSTALL_LIBEXECDIR})
