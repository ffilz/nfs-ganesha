add_definitions(
  -D__USE_GNU
  -D_GNU_SOURCE
)

########### next target ###############

SET(MainServices_STAT_SRCS
   nfs_admin_thread.c
   nfs_rpc_callback.c
   nfs_worker_thread.c
   nfs_rpc_dispatcher_thread.c
   nfs_rpc_tcp_socket_manager_thread.c
   nfs_init.c
   nfs_lib.c
   nfs_reaper_thread.c
   ../support/client_mgr.c
)

if(USE_9P)
  SET(MainServices_STAT_SRCS
    ${MainServices_STAT_SRCS}
    9p_dispatcher.c)
endif(USE_9P)

if(USE_9P AND USE_9P_RDMA)
  SET(MainServices_STAT_SRCS
    ${MainServices_STAT_SRCS}
    9p_rdma_dispatcher.c
    9p_rdma_callbacks.c)
endif(USE_9P AND USE_9P_RDMA)

if(USE_NFS_RDMA)
  add_definitions(-D_USE_NFS_RDMA)
endif(USE_NFS_RDMA)

if(USE_CB_SIMULATOR)
  SET(MainServices_STAT_SRCS
    ${MainServices_STAT_SRCS}
    nfs_rpc_callback_simulator.c)
endif(USE_CB_SIMULATOR)

if(USE_UPCALL_SIMULATOR)
  SET(MainServices_STAT_SRCS
    ${MainServices_STAT_SRCS}
    fsal_upcall_simulator_thread.c
    )
endif(USE_UPCALL_SIMULATOR)

if(USE_CB_SIMULATOR)
  SET(MainServices_STAT_SRCS
    ${MainServices_STAT_SRCS}
    nfs_rpc_callback_simulator.c
    )
endif(USE_CB_SIMULATOR)

add_library(MainServices STATIC ${MainServices_STAT_SRCS})
add_sanitizers(MainServices)

# FSAL core sources
# fsal_manager and fsal_destroyer are the only objects referenced by the
# core server all the rest are for the common support of fsal plugins.

set(fsal_CORE_SRCS
   ../FSAL/fsal_convert.c
   ../FSAL/commonlib.c
   ../FSAL/fsal_manager.c
   ../FSAL/access_check.c
   ../FSAL/fsal_config.c
   ../FSAL/default_methods.c
   ../FSAL/common_pnfs.c
   ../FSAL/fsal_destroyer.c
   ../FSAL/fsal_helper.c
   ../FSAL_UP/fsal_up_top.c
   ../FSAL_UP/fsal_up_async.c
)

if (USE_ACL_MAPPING)
  set(fsal_CORE_SRCS
      ${fsal_CORE_SRCS}
      ../FSAL/posix_acls.c
  )
endif(USE_ACL_MAPPING)

add_library(FsalCore STATIC ${fsal_CORE_SRCS})
add_sanitizers(FsalCore)

SET(nfsproto_STAT_SRCS
   ../Protocols/NFS/mnt_Dump.c
   ../Protocols/NFS/mnt_Export.c
   ../Protocols/NFS/mnt_Mnt.c
   ../Protocols/NFS/mnt_Null.c
   ../Protocols/NFS/mnt_Umnt.c
   ../Protocols/NFS/mnt_UmntAll.c
   ../Protocols/NFS/nfs_null.c
   ../Protocols/NFS/nfs4_Compound.c
   ../Protocols/NFS/nfs4_op_access.c
   ../Protocols/NFS/nfs4_op_allocate.c
   ../Protocols/NFS/nfs4_op_bind_conn.c
   ../Protocols/NFS/nfs4_op_close.c
   ../Protocols/NFS/nfs4_op_commit.c
   ../Protocols/NFS/nfs4_op_create.c
   ../Protocols/NFS/nfs4_op_create_session.c
   ../Protocols/NFS/nfs4_op_delegpurge.c
   ../Protocols/NFS/nfs4_op_delegreturn.c
   ../Protocols/NFS/nfs4_op_destroy_session.c
   ../Protocols/NFS/nfs4_op_exchange_id.c
   ../Protocols/NFS/nfs4_op_free_stateid.c
   ../Protocols/NFS/nfs4_op_getattr.c
   ../Protocols/NFS/nfs4_op_getdeviceinfo.c
   ../Protocols/NFS/nfs4_op_getdevicelist.c
   ../Protocols/NFS/nfs4_op_getfh.c
   ../Protocols/NFS/nfs4_op_illegal.c
   ../Protocols/NFS/nfs4_op_layoutcommit.c
   ../Protocols/NFS/nfs4_op_layoutget.c
   ../Protocols/NFS/nfs4_op_layoutreturn.c
   ../Protocols/NFS/nfs4_op_link.c
   ../Protocols/NFS/nfs4_op_lock.c
   ../Protocols/NFS/nfs4_op_lockt.c
   ../Protocols/NFS/nfs4_op_locku.c
   ../Protocols/NFS/nfs4_op_lookup.c
   ../Protocols/NFS/nfs4_op_lookupp.c
   ../Protocols/NFS/nfs4_op_nverify.c
   ../Protocols/NFS/nfs4_op_open.c
   ../Protocols/NFS/nfs4_op_open_confirm.c
   ../Protocols/NFS/nfs4_op_open_downgrade.c
   ../Protocols/NFS/nfs4_op_openattr.c
   ../Protocols/NFS/nfs4_op_putfh.c
   ../Protocols/NFS/nfs4_op_putpubfh.c
   ../Protocols/NFS/nfs4_op_putrootfh.c
   ../Protocols/NFS/nfs4_op_read.c
   ../Protocols/NFS/nfs4_op_readdir.c
   ../Protocols/NFS/nfs4_op_readlink.c
   ../Protocols/NFS/nfs4_op_reclaim_complete.c
   ../Protocols/NFS/nfs4_op_release_lockowner.c
   ../Protocols/NFS/nfs4_op_remove.c
   ../Protocols/NFS/nfs4_op_rename.c
   ../Protocols/NFS/nfs4_op_renew.c
   ../Protocols/NFS/nfs4_op_restorefh.c
   ../Protocols/NFS/nfs4_op_savefh.c
   ../Protocols/NFS/nfs4_op_secinfo.c
   ../Protocols/NFS/nfs4_op_secinfo_no_name.c
   ../Protocols/NFS/nfs4_op_sequence.c
   ../Protocols/NFS/nfs4_op_set_ssv.c
   ../Protocols/NFS/nfs4_op_setattr.c
   ../Protocols/NFS/nfs4_op_xattr.c
   ../Protocols/NFS/nfs4_op_setclientid.c
   ../Protocols/NFS/nfs4_op_setclientid_confirm.c
   ../Protocols/NFS/nfs4_op_destroy_clientid.c
   ../Protocols/NFS/nfs4_op_test_stateid.c
   ../Protocols/NFS/nfs4_op_verify.c
   ../Protocols/NFS/nfs4_op_write.c
   ../Protocols/NFS/nfs4_pseudo.c
   ../Protocols/NFS/nfs_proto_tools.c
)

if(USE_NFS3)
  SET(nfsproto_STAT_SRCS
    ${nfsproto_STAT_SRCS}
    ../Protocols/NFS/nfs3_access.c
    ../Protocols/NFS/nfs3_commit.c
    ../Protocols/NFS/nfs3_create.c
    ../Protocols/NFS/nfs3_fsinfo.c
    ../Protocols/NFS/nfs3_fsstat.c
    ../Protocols/NFS/nfs3_getattr.c
    ../Protocols/NFS/nfs3_link.c
    ../Protocols/NFS/nfs3_lookup.c
    ../Protocols/NFS/nfs3_mkdir.c
    ../Protocols/NFS/nfs3_mknod.c
    ../Protocols/NFS/nfs3_pathconf.c
    ../Protocols/NFS/nfs3_read.c
    ../Protocols/NFS/nfs3_readdir.c
    ../Protocols/NFS/nfs3_readdirplus.c
    ../Protocols/NFS/nfs3_readlink.c
    ../Protocols/NFS/nfs3_remove.c
    ../Protocols/NFS/nfs3_rename.c
    ../Protocols/NFS/nfs3_rmdir.c
    ../Protocols/NFS/nfs3_setattr.c
    ../Protocols/NFS/nfs3_symlink.c
    ../Protocols/NFS/nfs3_write.c
    )
endif(USE_NFS3)

SET(nfs4callback_SRCS
   ../Protocols/NFS/nfs4_cb_Compound.c
)

if(USE_NLM)
   SET(nlm_STAT_SRCS
      ../Protocols/NLM/nlm_Cancel.c
      ../Protocols/NLM/nlm_Free_All.c
      ../Protocols/NLM/nlm_Granted_Res.c
      ../Protocols/NLM/nlm_Lock.c
      ../Protocols/NLM/nlm_Null.c
      ../Protocols/NLM/nlm_Share.c
      ../Protocols/NLM/nlm_Sm_Notify.c
      ../Protocols/NLM/nlm_Test.c
      ../Protocols/NLM/nlm_Unlock.c
      ../Protocols/NLM/nlm_Unshare.c
      ../Protocols/NLM/nlm_async.c
      ../Protocols/NLM/nlm_util.c
      ../Protocols/NLM/nsm.c
   )
endif(USE_NLM)

SET(nfs_mnt_xdr_STAT_SRCS
   ../Protocols/XDR/xdr_mount.c
   ../Protocols/XDR/xdr_nfs23.c
   ../Protocols/XDR/xdr_nfsv41.c
   ../Protocols/XDR/xdr_rquota.c
   ../Protocols/XDR/xdr_nlm4.c
   ../Protocols/XDR/xdr_nsm.c
)

SET(rquota_STAT_SRCS
   ../Protocols/RQUOTA/rquota_Null.c
   ../Protocols/RQUOTA/rquota_getquota.c
   ../Protocols/RQUOTA/rquota_getactivequota.c
   ../Protocols/RQUOTA/rquota_setquota.c
   ../Protocols/RQUOTA/rquota_setactivequota.c
   ../Protocols/RQUOTA/rquota_common.c
)

if(USE_9P)
   SET(9p_STAT_SRCS
      ../Protocols/9P/9p_interpreter.c
      ../Protocols/9P/9p_proto_tools.c
      ../Protocols/9P/9p_read_conf.c
      ../Protocols/9P/9p_attach.c
      ../Protocols/9P/9p_auth.c
      ../Protocols/9P/9p_clunk.c
      ../Protocols/9P/9p_flush.c
      ../Protocols/9P/9p_flush_hook.c
      ../Protocols/9P/9p_getattr.c
      ../Protocols/9P/9p_getlock.c
      ../Protocols/9P/9p_lcreate.c
      ../Protocols/9P/9p_lopen.c
      ../Protocols/9P/9p_link.c
      ../Protocols/9P/9p_lock.c
      ../Protocols/9P/9p_mkdir.c
      ../Protocols/9P/9p_mknod.c
      ../Protocols/9P/9p_read.c
      ../Protocols/9P/9p_readdir.c
      ../Protocols/9P/9p_readlink.c
      ../Protocols/9P/9p_remove.c
      ../Protocols/9P/9p_rename.c
      ../Protocols/9P/9p_renameat.c
      ../Protocols/9P/9p_fsync.c
      ../Protocols/9P/9p_unlinkat.c
      ../Protocols/9P/9p_setattr.c
      ../Protocols/9P/9p_statfs.c
      ../Protocols/9P/9p_symlink.c
      ../Protocols/9P/9p_version.c
      ../Protocols/9P/9p_walk.c
      ../Protocols/9P/9p_xattrcreate.c
      ../Protocols/9P/9p_xattrwalk.c
      ../Protocols/9P/9p_write.c
      ../Protocols/9P/9p_rerror.c
   )
endif(USE_9P)

SET(avltree_STAT_SRCS
   ../avl/avl.c
   ../avl/bst.c
   ../avl/rb.c
   ../avl/splay.c
)

SET(log_STAT_SRCS
   ../log/display.c
   ../log/log_functions.c
)

if(USE_DBUS)
   SET(gshdbus_STAT_SRCS
      ../dbus/dbus_server.c
      ../dbus/properties_handler.c
      ../dbus/signal_handler.c
      ../dbus/dbus_heartbeat.c
   )
   include_directories(${DBUS_INCLUDE_DIRS})
endif(USE_DBUS)

if(USE_LTTNG)
   set(ganesha_trace_LIB_SRCS
      ../tracing/fsal_mem.c
      ../tracing/logger.c
      ../tracing/mdcache.c
      ../tracing/nfs4.c
      ../tracing/nfs_rpc.c
      ../tracing/state.c
   )
   include_directories(${LTTNG_INCLUDE_DIR})
endif(USE_LTTNG)

SET(cidr_STAT_SRCS
   ../cidr/cidr_addr.c
   ../cidr/cidr_compare.c
   ../cidr/cidr_from_str.c
   ../cidr/cidr_get.c
   ../cidr/cidr_inaddr.c
   ../cidr/cidr_mem.c
   ../cidr/cidr_misc.c
   ../cidr/cidr_net.c
   ../cidr/cidr_num.c
   ../cidr/cidr_to_str.c
   ../cidr/cidr_pow2_p.h
)

BISON_TARGET(
  ConfigParser
  ${CMAKE_CURRENT_SOURCE_DIR}/../config_parsing/conf_yacc.y
  ${CMAKE_CURRENT_BINARY_DIR}/../config_parsing/conf_yacc.c
  COMPILE_FLAGS "--defines -pganesha_yy"
)

FLEX_TARGET(
  ConfigScanner
  ${CMAKE_CURRENT_SOURCE_DIR}/../config_parsing/conf_lex.l
  ${CMAKE_CURRENT_BINARY_DIR}/../config_parsing/conf_lex.c
  COMPILE_FLAGS "-Pganeshun_yy -olex.yy.c"
)

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../config_parsing/ ${CMAKE_CURRENT_BINARY_DIR}/../config_parsing/)

ADD_FLEX_BISON_DEPENDENCY(ConfigScanner ConfigParser)

SET(config_parsing_STAT_SRCS
   ../config_parsing/analyse.c
   ../config_parsing/config_parsing.c
   ../config_parsing/conf_url.c
   ../config_parsing/analyse.h
   ../config_parsing/conf_lex.c
   ../config_parsing/conf_yacc.c
)

if(RADOS_URLS)
  SET(config_parsing_STAT_SRCS
    ${config_parsing_STAT_SRCS}
    ../config_parsing/conf_url_rados.c
  )
  include_directories(${RADOS_INCLUDE_DIR})
endif(RADOS_URLS)

SET(hashtable_STAT_SRCS
   ../hashtable/hashtable.c
)

SET(idmap_STAT_SRCS
   ../idmapper/idmapper.c
   ../idmapper/idmapper_cache.c
)

if(_MSPAC_SUPPORT)
  include_directories(${WBCLIENT_INCLUDE_DIR})
endif(_MSPAC_SUPPORT)

if(FREEBSD)
  SET(gos_STAT_SRCS
      ../os/freebsd/atsyscalls.c
      ../os/freebsd/mntent_compat.c
      ../os/freebsd/subr.c
      ../os/freebsd/xattr.c
      ../os/freebsd/memstream.c
  )
endif(FREEBSD)

if(LINUX)
  SET(gos_STAT_SRCS
      ../os/linux/subr.c
  )
endif(LINUX)

SET(rpcal_STAT_SRCS
   ../RPCAL/nfs_dupreq.c
   ../RPCAL/rpc_tools.c
)

if(_HAVE_GSSAPI)
   set(rpcal_STAT_SRCS
      ${rpcal_STAT_SRCS}
      ../RPCAL/gss_credcache.c
      ../RPCAL/gss_extra.c
   )
endif(_HAVE_GSSAPI)

SET(sal_STAT_SRCS
   ../SAL/state_async.c
   ../SAL/state_lock.c
   ../SAL/state_share.c
   ../SAL/state_misc.c
   ../SAL/state_layout.c
   ../SAL/state_deleg.c
   ../SAL/nfs4_clientid.c
   ../SAL/nfs4_state.c
   ../SAL/nfs4_state_id.c
   ../SAL/nfs4_lease.c
   ../SAL/nfs4_recovery.c
   ../SAL/nfs41_session_id.c
   ../SAL/nfs4_owner.c
   ../SAL/recovery/recovery_fs.c
   ../SAL/recovery/recovery_fs_ng.c
)

if(USE_NLM)
   set(sal_STAT_SRCS
      ${sal_STAT_SRCS}
      ../SAL/nlm_owner.c
      ../SAL/nlm_state.c
   )
endif(USE_NLM)

if(USE_9P)
   set(sal_STAT_SRCS
      ${sal_STAT_SRCS}
      ../SAL/9p_owner.c
   )
endif(USE_9P)

if(USE_RADOS_RECOV)
   set(sal_STAT_SRCS
      ${sal_STAT_SRCS}
      ../SAL/recovery/recovery_rados_kv.c
      ../SAL/recovery/recovery_rados_ng.c
      ../SAL/recovery/recovery_rados_cluster.c
   )
  include_directories(${RADOS_INCLUDE_DIR})
endif(USE_RADOS_RECOV)

SET(string_utils_STAT_SRCS
   ../support/strlcpy.c
   ../support/strnlen.c
)

SET(hash_SRCS
   ../support/murmur3.c
   ../support/city.c
)

SET(uid2grp_SRCS
   ../support/uid2grp.c
   ../support/uid2grp_cache.c
)

SET(netgroup_cache_SRCS
   ../support/netgroup_cache.c
)

if(USE_RADOS_RECOV)
   SET(rados_grace_SRCS
      ../support/rados_grace.c
   )
endif(USE_RADOS_RECOV)

SET(support_STAT_SRCS
   ../support/nfs4_acls.c
   ../support/nfs_creds.c
   ../support/nfs_filehandle_mgmt.c
   ../support/nfs_read_conf.c
   ../support/nfs_convert.c
   ../support/nfs_ip_name.c
   ../support/ds.c
   ../support/exports.c
   ../support/fridgethr.c
   ../support/delayed_exec.c
   ../support/misc.c
   ../support/bsd-base64.c
   ../support/server_stats.c
   ../support/export_mgr.c
   ../support/nfs4_fs_locations.c
)

if(ERROR_INJECTION)
  set(support_STAT_SRCS
    ${support_STAT_SRCS}
    ../support/err_inject.c
    )
endif(ERROR_INJECTION)

if(APPLE)
  set(support_STAT_SRCS
    ${support_STAT_SRCS}
    ../support/misc.c
    )
endif(APPLE)

SET(fsalpseudo_LIB_SRCS
   ../FSAL/FSAL_PSEUDO/handle.c
   ../FSAL/FSAL_PSEUDO/pseudofs_methods.h
   ../FSAL/FSAL_PSEUDO/main.c
   ../FSAL/FSAL_PSEUDO/export.c
)

SET(fsalmdcache_LIB_SRCS
   ../FSAL/Stackable_FSALs/FSAL_MDCACHE/mdcache_handle.c
   ../FSAL/Stackable_FSALs/FSAL_MDCACHE/mdcache_file.c
   ../FSAL/Stackable_FSALs/FSAL_MDCACHE/mdcache_xattrs.c
   ../FSAL/Stackable_FSALs/FSAL_MDCACHE/mdcache_main.c
   ../FSAL/Stackable_FSALs/FSAL_MDCACHE/mdcache_export.c
   ../FSAL/Stackable_FSALs/FSAL_MDCACHE/mdcache_helpers.c
   ../FSAL/Stackable_FSALs/FSAL_MDCACHE/mdcache_lru.c
   ../FSAL/Stackable_FSALs/FSAL_MDCACHE/mdcache_hash.c
   ../FSAL/Stackable_FSALs/FSAL_MDCACHE/mdcache_avl.c
   ../FSAL/Stackable_FSALs/FSAL_MDCACHE/mdcache_read_conf.c
   ../FSAL/Stackable_FSALs/FSAL_MDCACHE/mdcache_up.c
)

########### next target ###############

add_library(ganesha_nfsd SHARED
    ${MainServices_STAT_SRCS}
    ${fsal_STAT_SRCS}
    ${fsal_CORE_SRCS}
    ${nfsproto_STAT_SRCS}
    ${nfs4callback_SRCS}
    ${nlm_STAT_SRCS}
    ${nfs_mnt_xdr_STAT_SRCS}
    ${9p_STAT_SRCS}
    ${rquota_STAT_SRCS}
    ${avltree_STAT_SRCS}
    ${log_STAT_SRCS}
    ${gshdbus_STAT_SRCS}
    ${cidr_STAT_SRCS}
    ${config_parsing_STAT_SRCS}
    ${BISON_ConfigParser_OUTPUTS}
    ${FLEX_ConfigScanner_OUTPUTS}
    ${hashtable_STAT_SRCS}
    ${idmap_STAT_SRCS}
    ${gos_STAT_SRCS}
    ${rpcal_STAT_SRCS}
    ${sal_STAT_SRCS}
    ${string_utils_STAT_SRCS}
    ${hash_SRCS}
    ${uid2grp_SRCS}
    ${netgroup_cache_SRCS}
    ${rados_grace_SRCS}
    ${support_STAT_SRCS}
    ${fsalpseudo_LIB_SRCS}
    ${fsalmdcache_LIB_SRCS}
)

if(RADOS_URLS OR RADOS_RECOV)
  target_link_libraries(ganesha_nfsd ${RADOS_LIBRARIES})
endif(RADOS_URLS)

add_sanitizers(ganesha_nfsd)
set_target_properties(ganesha_nfsd PROPERTIES
  VERSION "${GANESHA_MAJOR_VERSION}.${GANESHA_MINOR_VERSION}"
  SOVERSION "${GANESHA_MAJOR_VERSION}.${GANESHA_MINOR_VERSION}"
)

install(TARGETS ganesha_nfsd DESTINATION ${LIB_INSTALL_DIR})

########### next target ###############

SET(ganesha.nfsd_SRCS
   nfs_main.c
)

add_executable(ganesha.nfsd
  ${ganesha.nfsd_SRCS}
)
add_sanitizers(ganesha.nfsd)

target_link_libraries(ganesha.nfsd
  ganesha_nfsd
  ${LIBTIRPC_LIBRARIES}
  ${SYSTEM_LIBRARIES}
  ${LTTNG_LIBRARIES}
)

install(TARGETS ganesha.nfsd COMPONENT daemon DESTINATION bin)

########### install files ###############

# We are still missing the install of docs and stuff
