if(_MSPAC_SUPPORT)
  include_directories(
    ${WBCLIENT_INCLUDE_DIR}
    )
endif(_MSPAC_SUPPORT)

########### next target ###############

SET(idmap_STAT_SRCS
   idmapper.c
   idmapper_cache.c
)

add_library(idmap STATIC ${idmap_STAT_SRCS})
set_property(TARGET idmap PROPERTY POSITION_INDEPENDENT_CODE ON)
add_sanitizers(idmap)

########### install files ###############
