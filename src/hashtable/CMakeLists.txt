########### next target ###############

SET(hashtable_STAT_SRCS
   hashtable.c
)

add_library(hashtable STATIC ${hashtable_STAT_SRCS})
set_property(TARGET hashtable PROPERTY POSITION_INDEPENDENT_CODE ON)
add_sanitizers(hashtable)


########### install files ###############
