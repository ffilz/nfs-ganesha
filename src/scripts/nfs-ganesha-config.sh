#!/bin/sh

#
# Extract configuration from /etc/sysconfig/ganesha and
# copy or generate new environment variables to
# /run/sysconfig/ganesha to be used by nfs-ganesha service
#

CONFIGFILE=/etc/sysconfig/ganesha
[ -r ${CONFIGFILE} ] && . ${CONFIGFILE}
[ -x ${EPOCHFILE} ] &&  EPOCHVALUE=`${EPOCHFILE}`

mkdir -p /run/sysconfig
{
cat $CONFIGFILE
[ -n "${EPOCHVALUE}" ] && echo EPOCH=\"-E $EPOCHVALUE\"
} > /run/sysconfig/ganesha
