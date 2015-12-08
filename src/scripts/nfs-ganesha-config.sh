#!/bin/sh

#
# Extract configuration from /etc/sysconfig/nfs and generate
# new environment variables to /run/sysconfig/nfs-utils if required
# to be used by nfs-ganesha service
#

nfs_config=/etc/sysconfig/ganesha
if test -r $nfs_config; then
            . $nfs_config
fi

[ -x $EPOCH_SCRIPT ] && $EPOCH_SCRIPT
[ -f /var/lib/ganesha/epoch ] && . /var/lib/ganesha/epoch

mkdir -p /run/sysconfig
{
cat $nfs_config
echo EPOCH=\"$EPOCH\"
} > /run/sysconfig/ganesha
