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

# Segmented io_uring COMPOUND engine. Built as an OBJECT library and folded
# into ganesha_nfsd alongside nfsproto, the same way every other protocol
# component is. It is only compiled when USE_FSAL_URING is on; with it off
# the legacy engine in Protocols/NFS is the only one present.

add_definitions(
  -D__USE_GNU
)

SET(nfsuring_STAT_SRCS
   nfs4_Compound_uring.c
   nfs4_uring_resume_pool.c
   nfs4_uring_optab.c
   nfs4_op_access_uring.c
   nfs4_op_allocate_uring.c
   nfs4_op_commit_uring.c
   nfs4_op_getattr_uring.c
   nfs4_op_read_uring.c
   nfs4_op_write_uring.c
   nfs4_uring_internal.h
)

add_library(nfsuring OBJECT ${nfsuring_STAT_SRCS})
add_sanitizers(nfsuring)
set_target_properties(nfsuring PROPERTIES COMPILE_FLAGS "-fPIC")

target_include_directories(nfsuring
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${LIBURING_INCLUDE_DIRS}
)

if (USE_LTTNG)
add_dependencies(nfsuring gsh_trace_header_generate)
include("${CMAKE_BINARY_DIR}/gsh_lttng_generation_file_properties.cmake")
endif (USE_LTTNG)

########### install files ###############
