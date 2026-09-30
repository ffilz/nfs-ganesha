# SPDX-License-Identifier: LGPL-3.0-or-later
#-------------------------------------------------------------------------------
#
# Copyright (C) 2026, IBM. All rights reserved.
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU Lesser General Public
# License as published by the Free Software Foundation; either
# version 3 of the License, or (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public
# License along with this library; if not, write to the Free Software
# Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
#
#-------------------------------------------------------------------------------
#
# FSAL_VFS_URING is FSAL_VFS plus an io_uring engine. It reuses the FSAL_VFS
# sources verbatim and only replaces main.c, exactly the way FSAL_LUSTRE and
# FSAL_DUMMYLUSTRE do in FSAL_VFS/vfs/CMakeLists.txt. Keeping it a separate
# module is what lets plain FSAL_VFS stay free of io_uring code.
#
if(USE_FSAL_VFS AND USE_FSAL_URING)

add_definitions(
  -D__USE_GNU
)

# main.c lives here but includes "vfs_methods.h", which lives in FSAL_VFS.
# The FSAL_VFS sources themselves reach their own headers relatively and do
# not need this.
include_directories(
  "${PROJECT_SOURCE_DIR}/FSAL/FSAL_VFS"
)

set(fsalvfsuring_LIB_SRCS
   ../FSAL_VFS/export.c
   ../FSAL_VFS/handle.c
   ../FSAL_VFS/handle_syscalls.c
   ../FSAL_VFS/file.c
   ../FSAL_VFS/xattrs.c
   ../FSAL_VFS/vfs_methods.h
   ../FSAL_VFS/state.c
   ../FSAL_VFS/subfsal_helpers.c
   ../FSAL_VFS/empty_check_hsm.c
   ../FSAL_VFS/vfs/subfsal_vfs.c
   ../FSAL_VFS/vfs/attrs.c
   vfs_uring_methods.h
   vfs_uring_fd.c
   vfs_uring_io.c
   vfs_uring_meta.c
   vfs_uring_rw.c
   vfs_uring_fops.c
   main.c
)

# Built as part of FSAL_VFS (FSAL/FSAL_VFS/os), which is always added before
# this directory because FSAL/CMakeLists.txt descends into FSAL_VFS first.
set(fsalvfsuring_OBJS
    $<TARGET_OBJECTS:fsal_os>
)

if(ENABLE_VFS_POSIX_ACL)
    set(fsalvfsuring_LIB_SRCS
        ${fsalvfsuring_LIB_SRCS}
        ../posix_acls.c
    )
    set(fsalvfsuring_OBJS
        ${fsalvfsuring_OBJS}
        $<TARGET_OBJECTS:gos>
    )
endif(ENABLE_VFS_POSIX_ACL)

add_library(fsalvfsuring MODULE
  ${fsalvfsuring_LIB_SRCS}
  ${fsalvfsuring_OBJS}
)
add_sanitizers(fsalvfsuring)

target_link_libraries(fsalvfsuring
  ganesha_nfsd
  ${SYSTEM_LIBRARIES}
  ${LDFLAG_DISALLOW_UNDEF}
)

# OUTPUT_NAME is not cosmetic. load_fsal() turns the FSAL name from the
# config into a path with "%s/libfsal%s.so" and then lowercases the
# basename, so the FSAL registered as "VFS_URING" must be shipped as
# libfsalvfs_uring.so or it will never be found.
set_target_properties(fsalvfsuring PROPERTIES
  OUTPUT_NAME fsalvfs_uring
  VERSION 4.2.0
  SOVERSION 4
)

install(TARGETS fsalvfsuring
  COMPONENT fsal
  DESTINATION ${FSAL_DESTINATION}
)

endif(USE_FSAL_VFS AND USE_FSAL_URING)

########### install files ###############
