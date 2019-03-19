include_directories(
  ${DBUS_INCLUDE_DIRS}
)

########### next target ###############

if(FREEBSD)
   set (FBSD_SUBR "${PROJECT_SOURCE_DIR}/os/freebsd/memstream.c")
endif(FREEBSD)

SET(gshdbus_STAT_SRCS
   dbus_server.c
   properties_handler.c
   signal_handler.c
   dbus_heartbeat.c
   ${FBSD_SUBR}
)

add_library(gshdbus OBJECT ${gshdbus_STAT_SRCS})
add_sanitizers(gshdbus)
set_target_properties(gshdbus PROPERTIES COMPILE_FLAGS "-fPIC")

########### install files ###############
