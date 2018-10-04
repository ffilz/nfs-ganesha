# All we need to do here is control the
# build of chosen platform

if(FREEBSD)
  SET(gos_STAT_SRCS
      freebsd/atsyscalls.c
      freebsd/mntent_compat.c
      freebsd/subr.c
      freebsd/xattr.c
      freebsd/memstream.c
  )
endif(FREEBSD)

if(LINUX)
  SET(gos_STAT_SRCS
      linux/subr.c
  )
endif(LINUX)

add_library(gos STATIC ${gos_STAT_SRCS})
set_property(TARGET gos PROPERTY POSITION_INDEPENDENT_CODE ON)
add_sanitizers(gos)

########### install files ###############
