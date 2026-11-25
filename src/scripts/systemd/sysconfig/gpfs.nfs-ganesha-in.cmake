#
# Storage Scale (GPFS) sysconfig template for NFS-Ganesha.
#
# Rendered at build time by CMake into gpfs.nfs-ganesha and installed to
# /etc/sysconfig/ganesha when built with GPFS and USE_GPFS_SCALE enabled.
# Read by gpfs.nfs-ganesha-config.sh to generate /run/sysconfig/ganesha.
#

# Default daemon command-line options (log file, config path, and log level)
OPTIONS="-L /var/log/ganesha.log -f /etc/ganesha/ganesha.conf -N NIV_EVENT"

# GPFS helper script invoked to compute cluster epoch and node ID
EPOCH_EXEC="@LIBEXECDIR@/ganesha/gpfs-epoch"

# Optional environment variables prepended to ExecStart (e.g., ASAN debugging)
#ENV_OPTIONS="ASAN_OPTIONS=alloc_dealloc_mismatch=true:print_stats=true:atexit=true:start_deactivated=false:print_cmdline=true:quarantine_size_mb=1:detect_leaks=false:abort_on_error=true:disable_coredump=false:verbosity=2:log_path=/tmp/asan.log:log_exe_name=1"

