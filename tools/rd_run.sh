#!/bin/bash

set -e

COMMIT=$1

REPO=https://git.xiph.org/daala.git
WORK_DIR=/Users/jack/tmp/daala-auto

# Prepare work area and repository
mkdir -p ${WORK_DIR}
if [ -d ${WORK_DIR}/repo ]; then
    cd ${WORK_DIR}/repo && git fetch
else
    cd ${WORK_DIR} && git clone --mirror ${REPO} repo
fi

# Prepare working tree
rm -rf ${WORK_DIR}/work
mkdir -p ${WORK_DIR}/work
cd ${WORK_DIR}/work && git clone --shared ${WORK_DIR}/repo source
cd ${WORK_DIR}/work/source && git checkout ${COMMIT}

# Build Daala
mkdir -p ${WORK_DIR}/work/build
cd ${WORK_DIR}/work/build && ../source/autogen.sh && ../source/configure --enable-logging && make all tools

