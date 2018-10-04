add_definitions(
  -D__USE_GNU
  -D_GNU_SOURCE
  ${GFAPI_CFLAGS}
)

set( LIB_PREFIX 64)

########### next target ###############

SET(fsalgluster_LIB_SRCS
   main.c
   export.c
   handle.c
   fsal_up.c
   gluster_internal.h
   gluster_internal.c
   mds.c
   ds.c
)

add_library(fsalgluster MODULE ${fsalgluster_LIB_SRCS})
add_sanitizers(fsalgluster)

target_link_libraries(fsalgluster
  "-L ${CMAKE_BINARY_DIR}/MainNFSD -l ganesha_nfsd"
  ${GFAPI_LIBRARIES}
  ${SYSTEM_LIBRARIES}
  ${LTTNG_LIBRARIES}
)

set_target_properties(fsalgluster PROPERTIES VERSION 4.2.0 SOVERSION 4)
install(TARGETS fsalgluster COMPONENT fsal DESTINATION ${FSAL_DESTINATION} )


########### install files ###############
