#XXX# if (USE_RADOS_RECOV)
#XXX#   add_executable(ganesha-rados-grace ganesha-rados-grace.c)
#XXX#   include_directories(${RADOS_INCLUDE_DIR})
#XXX#   target_link_libraries(ganesha-rados-grace rados_grace ${RADOS_LIBRARIES})
#XXX#   install(TARGETS ganesha-rados-grace COMPONENT tools DESTINATION bin)
#XXX# endif(USE_RADOS_RECOV)

########### install files ###############

if(USE_TOOL_MULTILOCK)
  add_subdirectory(multilock)
endif(USE_TOOL_MULTILOCK)
