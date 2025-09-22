########### next target ###############

set(CMAKE_CXX_STANDARD 17)

SET(ganesha_monitoring_SRCS
  prometheus_exposer.cc
  dynamic_metrics.cc
)

add_library(ganesha_monitoring SHARED ${ganesha_monitoring_SRCS})

find_library(PROCPS_LIB procps)
if(PROCPS_LIB)
    target_link_libraries(ganesha_monitoring ${PROCPS_LIB})
    add_definitions(-DHAVE_PROCPS)
else()
    message(WARNING "procps library not found; please install procps lib for ganesha resource related metrics")
endif()

add_sanitizers(ganesha_monitoring)
set_target_properties(ganesha_monitoring PROPERTIES COMPILE_FLAGS "-fPIC")
target_include_directories(ganesha_monitoring     PRIVATE ${PROJECT_SOURCE_DIR}/libntirpc/src/monitoring/prometheus-cpp-lite/core/include)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -pedantic-errors -Werror -Wall -Wextra")

install(TARGETS ganesha_monitoring LIBRARY DESTINATION ${LIB_INSTALL_DIR})

########### install files ###############
