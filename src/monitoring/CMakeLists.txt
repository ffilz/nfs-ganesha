########### next target ###############

set(CMAKE_CXX_STANDARD 17)

SET(monitoring_SRCS
  monitoring.cc
)

set(MONITORING_LIBRARIES
  prometheus-cpp-pull
  prometheus-cpp-core
)

add_library(monitoring OBJECT ${monitoring_SRCS})
add_sanitizers(monitoring)
set_target_properties(monitoring PROPERTIES COMPILE_FLAGS "-fPIC")

########### install files ###############
