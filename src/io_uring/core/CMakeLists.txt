# SPDX-License-Identifier: LGPL-3.0-or-later

if(USE_FSAL_URING)
  add_library(uring_core STATIC uring_engine.c)

  # ganesha_nfsd is a shared library, so every object folded into it must be
  # position independent. Without this the static archive fails to link with
  # "relocation R_X86_64_PC32 against symbol ... can not be used when making
  # a shared object".
  set_target_properties(uring_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

  # cmake is configured from src/, so CMAKE_SOURCE_DIR is already <repo>/src.
  # Sources include the engine as "io_uring/core/uring_engine.h", so the path
  # that has to be visible is src/ itself.
  target_include_directories(uring_core
    PUBLIC
      ${PROJECT_SOURCE_DIR}
      ${PROJECT_SOURCE_DIR}/include
      ${PROJECT_BINARY_DIR}/include
      ${LIBURING_INCLUDE_DIRS}
  )

  target_link_libraries(uring_core
    ${LIBURING_LIBRARIES}
  )
endif()
