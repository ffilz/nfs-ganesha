#!/bin/sh

#
# Storage Scale (GPFS) variant of nfs-ganesha-config.sh
#
# Extracts configuration from /etc/sysconfig/ganesha and writes runtime
# environment variables to /run/sysconfig/ganesha for nfs-ganesha.service.
#

CONFIGFILE=/etc/sysconfig/ganesha
RUNCONFIG=/run/sysconfig/ganesha

if [ -r ${CONFIGFILE} ]; then
	. ${CONFIGFILE}
	[ -x "${EPOCH_EXEC%% *}" ] && EPOCHVALUE=`${EPOCH_EXEC}`

	# Derive cluster node ID from NODEID_EXEC or upper 16 bits of gpfs-epoch
	if [ -x "${NODEID_EXEC%% *}" ]; then
		NODEID=`${NODEID_EXEC}`
	elif [ "$(basename "${EPOCH_EXEC%% *}")" = "gpfs-epoch" ]; then
		NODEID=$(( EPOCHVALUE >> 16 ))
	fi

	# Apply custom LimitNOFILE via systemd drop-in when NOFILE is configured
	NOFILE_CONF=/lib/systemd/system/nfs-ganesha.service.d/10-nofile.conf
	if [ -n "$NOFILE" ]; then
		mkdir -p $(command dirname ${NOFILE_CONF})
		printf "[Service]\nLimitNOFILE=$NOFILE\n" > $NOFILE_CONF
		systemctl daemon-reload
	fi

	mkdir -p $(command dirname ${RUNCONFIG} 2>/dev/null)
	{
		cat ${CONFIGFILE}
		[ -n "${EPOCHVALUE}" ] && echo EPOCH=\"-E $EPOCHVALUE\"
		[ -n "${NODEID}" ] && echo GNODEID=\"-I $NODEID\"

		# Enable NUMA memory interleaving if numactl is installed
		NUMACTL=$(command -v numactl 2>/dev/null)
		if [ -n "${NUMACTL}" ]; then
			echo NUMACTL=${NUMACTL}
			echo NUMAOPTS=--interleave=all
		fi
	} > ${RUNCONFIG}
fi
