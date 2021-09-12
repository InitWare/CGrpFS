project(cgrpfs)

cmake_minimum_required(VERSION 3.9)
cmake_policy(VERSION 3.9)

include(FindPkgConfig)
include(GNUInstallDirs)

pkg_check_modules(fuse REQUIRED IMPORTED_TARGET fuse)

add_executable(cgrpfs cgrpfs.c cgrpfs_vnops.c)
target_link_libraries(cgrpfs PkgConfig::fuse)

install(TARGETS cgrpfs DESTINATION ${CMAKE_INSTALL_LIBEXECDIR})
