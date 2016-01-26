# Only build VFS fsal

set(USE_FSAL_PROXY  OFF)
set(USE_FSAL_CEPH OFF)
set(USE_FSAL_GPFS OFF)
set(USE_FSAL_ZFS OFF)
set(USE_FSAL_LUSTRE OFF)
set(USE_FSAL_PANFS OFF)
set(_MSPAC_SUPPORT OFF)
set(USE_9P OFF)
set(USE_DBUS ON)

message(STATUS "Building vfs_only configuration")
