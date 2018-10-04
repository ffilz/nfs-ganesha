# All we need to do here is control the
# build of chosen platform

if(FREEBSD)
  if(USE_FSAL_PANFS)
      add_definitions(-D__PanFS__)
  endif(USE_FSAL_PANFS)
  SET(fsal_os_STAT_SRCS
      freebsd/handle_syscalls.c
  )
endif(FREEBSD)

if(LINUX)
  SET(fsal_os_STAT_SRCS
      linux/handle_syscalls.c
  )
endif(LINUX)

add_library(fsal_os STATIC ${fsal_os_STAT_SRCS})
set_property(TARGET fsal_os PROPERTY POSITION_INDEPENDENT_CODE ON)
add_sanitizers(fsal_os)

########### install files ###############
