#!/bin/bash
# Script outline to install and build kernel.
# Author: Siddhant Jajoo.

set -e
set -u

OUTDIR=/tmp/aeld
KERNEL_REPO=https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux-stable.git
KERNEL_VERSION=v5.15.163
BUSYBOX_VERSION=1_33_1
FINDER_APP_DIR=$(realpath $(dirname $0))
ARCH=arm64
CROSS_COMPILE=aarch64-none-linux-gnu-

if [ $# -lt 1 ]
then
	echo "Using default directory ${OUTDIR} for output"
else
	OUTDIR=$1
	echo "Using passed directory ${OUTDIR} for output"
fi

mkdir -p ${OUTDIR}

cd "$OUTDIR"
if [ ! -d "${OUTDIR}/linux-stable" ]; then
    #Clone only if the repository does not exist.
	echo "CLONING GIT LINUX STABLE VERSION ${KERNEL_VERSION} IN ${OUTDIR}"
	git clone ${KERNEL_REPO} --depth 1 --single-branch --branch ${KERNEL_VERSION}
fi
if [ ! -e ${OUTDIR}/linux-stable/arch/${ARCH}/boot/Image ]; then
    cd linux-stable
    echo "Checking out version ${KERNEL_VERSION}"
    git clean -fd
    git checkout ${KERNEL_VERSION}

    echo "Building Linux kernel"
    export TMPDIR=${OUTDIR}/tmp
    mkdir -p ${TMPDIR}
    make ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} mrproper
    make ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} defconfig
    make -j4 ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE}
fi

echo "Adding the Image in outdir"
cp ${OUTDIR}/linux-stable/arch/${ARCH}/boot/Image ${OUTDIR}/Image

echo "Creating the staging directory for the root filesystem"
cd "$OUTDIR"
if [ -d "${OUTDIR}/rootfs" ]
then
	echo "Deleting rootfs directory at ${OUTDIR}/rootfs and starting over"
    sudo rm  -rf ${OUTDIR}/rootfs
fi

mkdir -p ${OUTDIR}/rootfs
cd ${OUTDIR}/rootfs
mkdir -p bin sbin lib lib64 usr usr/bin usr/sbin usr/lib home proc sys dev

cd "$OUTDIR"
if [ ! -d "${OUTDIR}/busybox" ]
then
git clone https://git.busybox.net/busybox
    cd busybox
    git checkout ${BUSYBOX_VERSION}
    make distclean
    make defconfig
    sed -i 's/# CONFIG_STATIC is not set/CONFIG_STATIC=y/' .config
    # The "tc" applet references kernel pkt_sched.h/pkt_cls.h struct fields
    # (e.g. tc_cbq_lssopt) that newer host kernel headers no longer define,
    # which breaks the busybox build. Disable it since it isn't needed here.
    sed -i 's/^CONFIG_TC=y/# CONFIG_TC is not set/' .config
else
    cd busybox
fi

echo "Building and installing busybox"
make ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} install CONFIG_PREFIX=${OUTDIR}/rootfs

echo "Library dependencies"
${CROSS_COMPILE}readelf -a ${OUTDIR}/rootfs/bin/busybox | grep "program interpreter" || echo "busybox is statically linked, no interpreter"
${CROSS_COMPILE}readelf -a ${OUTDIR}/rootfs/bin/busybox | grep "Shared library" || echo "busybox is statically linked, no shared library dependencies"

echo "Copying library dependencies to rootfs"
SYSROOT=$(${CROSS_COMPILE}gcc -print-sysroot)
if [ -n "${SYSROOT}" ] && [ -e "${SYSROOT}/lib64/ld-linux-aarch64.so.1" ]; then
    LIBDIR=${SYSROOT}/lib64
else
    # Some cross toolchains (e.g. this host's aarch64-none-linux-gnu- build)
    # report no sysroot and install libraries into the multiarch path instead
    LIBDIR=/usr/lib/aarch64-linux-gnu
fi
cp ${LIBDIR}/ld-linux-aarch64.so.1 ${OUTDIR}/rootfs/lib64/
cp ${LIBDIR}/libm.so.6 ${OUTDIR}/rootfs/lib64/
cp ${LIBDIR}/libc.so.6 ${OUTDIR}/rootfs/lib64/

# writer's ELF interpreter path is /lib/ld-linux-aarch64.so.1 (not /lib64/...),
# and the loader's own default search path doesn't include /lib64, so every
# lib needs a copy under /lib as well or execve/dlopen fails with ENOENT
cp ${LIBDIR}/ld-linux-aarch64.so.1 ${OUTDIR}/rootfs/lib/
cp ${LIBDIR}/libm.so.6 ${OUTDIR}/rootfs/lib/
cp ${LIBDIR}/libc.so.6 ${OUTDIR}/rootfs/lib/

echo "Creating device nodes"
sudo mknod -m 666 ${OUTDIR}/rootfs/dev/null c 1 3
sudo mknod -m 666 ${OUTDIR}/rootfs/dev/console c 5 1
sudo mknod -m 666 ${OUTDIR}/rootfs/dev/tty c 5 0

echo "Building writer utility"
cd ${FINDER_APP_DIR}
make clean
make CROSS_COMPILE=${CROSS_COMPILE}

echo "Copying finder scripts and utilities to rootfs/home"
cp ${FINDER_APP_DIR}/writer ${OUTDIR}/rootfs/home/
cp ${FINDER_APP_DIR}/finder.sh ${OUTDIR}/rootfs/home/
mkdir -p ${OUTDIR}/rootfs/home/conf
cp ${FINDER_APP_DIR}/conf/username.txt ${OUTDIR}/rootfs/home/conf/
cp ${FINDER_APP_DIR}/conf/assignment.txt ${OUTDIR}/rootfs/home/conf/
cp ${FINDER_APP_DIR}/finder-test.sh ${OUTDIR}/rootfs/home/
sed -i 's|\.\./conf/assignment.txt|conf/assignment.txt|g' ${OUTDIR}/rootfs/home/finder-test.sh
cp ${FINDER_APP_DIR}/autorun-qemu.sh ${OUTDIR}/rootfs/home/

echo "Changing ownership of rootfs"
cd ${OUTDIR}/rootfs
sudo chown -R root:root .

echo "Creating initramfs"
cd ${OUTDIR}/rootfs
find . -print0 | cpio --null -ov --format=newc | gzip -9 > ${OUTDIR}/initramfs.cpio.gz
