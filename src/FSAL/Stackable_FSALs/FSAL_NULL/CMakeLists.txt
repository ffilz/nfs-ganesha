add_definitions(
  -D__USE_GNU
  -D_GNU_SOURCE
)

set( LIB_PREFIX 64)

########### next target ###############

SET(fsalnull_LIB_SRCS
   handle.c
   file.c
   xattrs.c
   nullfs_methods.h
   main.c
   export.c
)

SET(GANESHA_NFSD_LIBS
  "-L ${CMAKE_BINARY_DIR}/MainNFSD -l ganesha_nfsd"
  ${SYSTEM_LIBRARIES}
  ${LTTNG_LIBRARIES}
)

add_library(fsalnull MODULE ${fsalnull_LIB_SRCS})
add_sanitizers(fsalnull)

target_link_libraries(fsalnull ${GANESHA_NFSD_LIBS})

set_target_properties(fsalnull PROPERTIES VERSION 4.2.0 SOVERSION 4)
install(TARGETS fsalnull COMPONENT fsal DESTINATION ${FSAL_DESTINATION} )


########### install files ###############
