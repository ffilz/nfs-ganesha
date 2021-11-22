########### next target ###############

set(CMAKE_CXX_STANDARD 17)

SET(monitoring_SRCS
  monitoring.cc
  nfs_names.c
)

add_library(monitoring OBJECT ${monitoring_SRCS})
add_sanitizers(monitoring)
set_target_properties(monitoring PROPERTIES COMPILE_FLAGS "-fPIC")
target_link_libraries(monitoring
  metrics
)

########### install files ###############
