#!/bin/bash
set -eu
args=(${@:-""})
base=$(realpath $(dirname ${BASH_SOURCE[0]}))
GIT_ROOT=$(cd ${base} && git rev-parse --show-toplevel)
cd ${GIT_ROOT}/src

CMAKE="cmake"
if grep -q -i CentOS /etc/os-release ; then
  # on centos use the cmake3 binary
  CMAKE="cmake3"
fi

if [ "${args[0]}" == "clean" ]; then
  ${CMAKE}  clean .
  make clean
elif [ "${args[0]}" == "debug" ]; then
  ${CMAKE} .  -DCMAKE_BUILD_TYPE=Debug -DUSE_ADMIN_TOOLS=ON -DUSE_GUI_ADMIN_TOOLS=OFF -DUSE_DBUS=ON
  make -j 16
else
  ${CMAKE} .  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DUSE_ADMIN_TOOLS=ON -DUSE_GUI_ADMIN_TOOLS=OFF -DUSE_DBUS=ON
  make -j 16 $@
fi
