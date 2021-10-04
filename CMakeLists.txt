project(cgrpfs)

cmake_minimum_required(VERSION 3.9)
cmake_policy(VERSION 3.9)

include(FindPkgConfig)
include(GNUInstallDirs)

list(APPEND CGRPFS_SRCS cgrpfs.c)

if (CMAKE_SYSTEM_NAME MATCHES "kOpenBSD.*|OpenBSD.*")
	find_package(Threads REQUIRED)
	set(CGRPFS_THREADED true)
	set(FUSE_LIB fuse Threads::Threads)
	list(APPEND CGRPFS_SRCS cgrpfs_main_threads.c cgrpfs_fuseops.c)
#elseif (CMAKE_SYSTEM_NAME MATCHES "kNetBSD.*|NetBSD.*")
#	set(FUSE_LIB puffs util)
#	set(CGRPFS_PUFFS true)
#	list(APPEND CGRPFS_SRCS cgrpfs_main_puffs.c cgrpfs_vnops.c
#	    cgrpfs_vfsops.c)
else ()
	pkg_check_modules(fuse REQUIRED IMPORTED_TARGET fuse)
	set(FUSE_LIB PkgConfig::fuse)
	list(APPEND CGRPFS_SRCS cgrpfs_main.c cgrpfs_fuseops.c)
endif()

add_executable(cgrpfs ${CGRPFS_SRCS})
target_link_libraries(cgrpfs ${FUSE_LIB})

if (CGRPFS_THREADED)
	target_compile_definitions(cgrpfs PRIVATE -DCGRPFS_THREADED)
endif ()

if (CGRPFS_PUFFS)
	target_compile_definitions(cgrpfs PRIVATE -DCGRPFS_PUFFS)
endif ()

install(TARGETS cgrpfs DESTINATION ${CMAKE_INSTALL_LIBEXECDIR})
