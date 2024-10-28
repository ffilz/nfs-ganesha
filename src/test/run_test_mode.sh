# Script to run Ganesha for presubmit testing (e.g. pynfs, cthon)
# Usage:
# run_test_mode.sh <build_path>
set -euo pipefail

if [ $# -ne 1 ]; then
  echo "Usage: $0 <build_path>"
  exit 1
fi

BUILD_PATH="$1"
GANESHA_EXE="$BUILD_PATH/ganesha.nfsd"
if [ ! -f "$GANESHA_EXE" ]; then
  echo "Error: Ganesha executable is missing: $GANESHA_EXE"
  exit 1
fi

TEMP_DIR=$(mktemp --directory)
CONFIG_FILE="$TEMP_DIR/conf"
LOG_FILE="$TEMP_DIR/log"
PID_FILE="$TEMP_DIR/pid"
EXPORT_PATH="$TEMP_DIR/export/"

mkdir --parents $EXPORT_PATH
cat << EOF > "$CONFIG_FILE"
NFS_CORE_PARAM {
	Plugins_Dir = "$BUILD_PATH/FSAL/FSAL_VFS/vfs/";
	Protocols = 3, 4;
}
EXPORT_DEFAULTS {
	Access_Type = RW;
}
EXPORT
{
	Export_Id = 1;
	Path = $EXPORT_PATH;
	Pseudo = /export;
	Protocols = 3, 4;
	Access_Type = RW;
	Sectype = sys;
	FSAL {
		Name = VFS;
	}
}
LOG {
	Default_Log_Level = INFO;
}
EOF

set -x
sudo /tmp/ganesha/ganesha.nfsd -F -x -f $CONFIG_FILE -L $LOG_FILE -p $PID_FILE
