# All we need to do here is control the
# build of chosen platform

if(BSDBASED)
  SET(gos_STAT_SRCS
      bsd/mntent_compat.c
      bsd/subr.c
      ${gos_STAT_SRCS})
endif(BSDBASED)

if(FREEBSD)
  SET(gos_STAT_SRCS
      freebsd/atsyscalls.c
      freebsd/xattr.c
      ${gos_STAT_SRCS}
  )
endif(FREEBSD)

if(LINUX)
  SET(gos_STAT_SRCS
      linux/subr.c
      ${gos_STAT_SRCS}
  )
endif(LINUX)

add_library(gos OBJECT ${gos_STAT_SRCS})
add_sanitizers(gos)
set_target_properties(gos PROPERTIES COMPILE_FLAGS "-fPIC")

########### install files ###############
