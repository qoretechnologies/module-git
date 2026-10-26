#!/bin/bash

set -e
set -x

ENV_FILE=/tmp/env.sh

. ${ENV_FILE}

# setup MODULE_SRC_DIR env var
cwd=`pwd`
if [ -z "${MODULE_SRC_DIR}" ]; then
    if [ -e "$cwd/src/git-module.cpp" ]; then
        MODULE_SRC_DIR=$cwd
    else
        MODULE_SRC_DIR=$WORKDIR/module-git
    fi
fi
echo "export MODULE_SRC_DIR=${MODULE_SRC_DIR}" >> ${ENV_FILE}

. ${ENV_FILE}

export MAKE_JOBS=4

# install libgit2 development headers
apk add --no-cache libgit2-dev > /dev/null 2>&1

# the tests must load the modules of this tree: reject relative module paths in them
${MODULE_SRC_DIR}/test/docker_test/check-module-paths.sh git

# build module and install
echo && echo "-- building module --"
mkdir -p ${MODULE_SRC_DIR}/build
cd ${MODULE_SRC_DIR}/build
cmake .. -DCMAKE_BUILD_TYPE=debug -DCMAKE_INSTALL_PREFIX=${INSTALL_PREFIX}
make -j${MAKE_JOBS}
make install

# Verify that source-owned provider presentation catalogs match this checkout.
${MODULE_SRC_DIR}/test/docker_test/check-i18n.sh

# run the tests
export QORE_MODULE_DIR=${MODULE_SRC_DIR}/qlib:${QORE_MODULE_DIR}
cd ${MODULE_SRC_DIR}

# the tests must load the module that this job built and installed, not one that the image already has
loaded=$(qore -l git -e 'printf("%s", get_module_hash().git.filename);')
if ! grep -qxF "${loaded}" ${MODULE_SRC_DIR}/build/install_manifest.txt; then
    echo "ERROR: the tests would load '${loaded}', which this job did not install" >&2
    exit 1
fi
echo "-- the tests load ${loaded}"

for test in test/*.qtest; do
    qore $test -vv
done
