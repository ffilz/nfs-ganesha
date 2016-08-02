# - Find CephFS
# Find the Linux Trace Toolkit - next generation with associated includes path.
# See http://ceph.org/
#
# This module accepts the following optional variables:
#    CEPH_PREFIX   = A hint on CEPHFS install path.
#
# This module defines the following variables:
#    CEPHFS_FOUND       = Was CephFS found or not?
#    CEPHFS_LIBRARIES   = The list of libraries to link to when using CephFS
#    CEPHFS_INCLUDE_DIR = The path to CephFS include directory
#
# On can set CEPH_PREFIX before using find_package(CephFS) and the
# module with use the PATH as a hint to find CephFS.
#
# The hint can be given on the command line too:
#   cmake -DCEPH_PREFIX=/DATA/ERIC/CephFS /path/to/source

if(CEPH_PREFIX)
	message(STATUS "FindLTTng: using PATH HINT: ${CEPH_PREFIX}")
else()
	set(CEPH_PREFIX)
endif()

#One can add his/her own builtin PATH.
#FILE(TO_CMAKE_PATH "/DATA/ERIC/CephFS" MYPATH)
#list(APPEND CEPH_PREFIX ${MYPATH})

find_path(CEPHFS_INCLUDE_DIR
  NAMES cephfs/libcephfs.h
  PATHS ${CEPH_PREFIX}
  PATH_SUFFIXES include
  DOC "The CephFS include headers")

find_path(CEPHFS_LIBRARY_DIR
  NAMES libcephfs.so
  PATHS ${CEPH_PREFIX}
  PATH_SUFFIXES lib lib64
  DOC "The CephFS libraries")

find_library(CEPHFS_LIBRARY cephfs PATHS ${CEPHFS_LIBRARY_DIR})
#check_library_exists(cephfs ceph_ll_connectable_x ${CEPHFS_LIBRARY_DIR} CEPH_FS)
#if (NOT CEPH_FS)
  #unset(CEPHFS_LIBRARY_DIR CACHE)
  #unset(CEPHFS_INCLUDE_DIR CACHE)
#else (NOT CEPH_FS)
  check_library_exists(cephfs ceph_ll_mknod ${CEPHFS_LIBRARY_DIR} CEPH_FS_MKNOD)
  if(NOT CEPH_FS_MKNOD)
    message(WARNING "Cannot find ceph_ll_mknod.  Disabling CEPH fsal mknod method")
    set(USE_FSAL_CEPH_MKNOD OFF)
  else(CEPH_FS_MKNOD)
    set(USE_FSAL_CEPH_MKNOD ON)
  endif(NOT CEPH_FS_MKNOD)
  check_library_exists(cephfs ceph_ll_setlk ${CEPHFS_LIBRARY_DIR} CEPH_FS_SETLK)
  if(NOT CEPH_FS_SETLK)
    message(WARNING "Cannot find ceph_ll_setlk.  Disabling CEPH fsal lock2 method")
    set(USE_FSAL_CEPH_SETLK OFF)
  else(CEPH_FS_SETLK)
    set(USE_FSAL_CEPH_SETLK ON)
  endif(NOT CEPH_FS_SETLK)
#endif (NOT CEPH_FS)

set(CEPHFS_LIBRARIES ${CEPHFS_LIBRARY})

# handle the QUIETLY and REQUIRED arguments and set PRELUDE_FOUND to TRUE if
# all listed variables are TRUE
include(FindPackageHandleStandardArgs)
FIND_PACKAGE_HANDLE_STANDARD_ARGS(CEPHFS
	REQUIRED_VARS CEPHFS_INCLUDE_DIR CEPHFS_LIBRARY_DIR)
# VERSION FPHSA options not handled by CMake version < 2.8.2)
#                                  VERSION_VAR)
mark_as_advanced(CEPHFS_INCLUDE_DIR)
mark_as_advanced(CEPHFS_LIBRARY_DIR)
mark_as_advanced(USE_FSAL_CEPH_MKNOD)
mark_as_advanced(USE_FSAL_CEPH_SETLK)

