
########### next target ###############

SET(avltree_STAT_SRCS
   avl.c
   bst.c
   rb.c
   splay.c
)

add_library(avltree STATIC ${avltree_STAT_SRCS})
set_property(TARGET avltree PROPERTY POSITION_INDEPENDENT_CODE ON)
add_sanitizers(avltree)

########### install files ###############
