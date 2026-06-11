#!/bin/bash
# jone.sh - 1回だけクラッシュ+リカバリし、不整合なら /tmp/bad.img に保存して解析する。
set -u
cd /home/mouse/Documents/is/lab3/sngw/fs-test
QEMU=qemu-system-x86_64
APPEND="root=/dev/sda rw console=ttyS0 oops=panic panic=-1 nokaslr"
BZ=linux-7.0.10/arch/x86_64/boot/bzImage
SMP=${SMP:-2}
KILL=${KILL:-7.0}
wait_for(){ for ((i=0;i<$3;i++)); do grep -q "$2" "$1" 2>/dev/null && return 0; sleep 1; done; return 1; }
launch(){ "$QEMU" -enable-kvm -cpu host -m 2G -smp $SMP -kernel "$BZ" -initrd initramfs.cpio \
  -nographic -no-reboot -net user -net nic -append "$APPEND" \
  -drive file=vdisk.img,format=raw,index=1,media=disk \
  -virtfs "local,id=host,path=./modules,security_model=none,mount_tag=host" < "$1" > "$2" 2>&1 & QPID=$!; }

uv run modules/llfs/mkfs.py vdisk.img >/dev/null
F1=/tmp/o1.$$; L1=/tmp/ol1.$$; mkfifo "$F1"; exec 3<>"$F1"; launch "$F1" "$L1"
wait_for "$L1" "Welcome" 120; sleep 1
printf 'insmod /mnt/host/llfs/llfs.ko\n' >&3
printf 'mount -t llfs /dev/sda /mnt/host/llfs/mnt && echo MOUNT_OK\n' >&3
wait_for "$L1" "MOUNT_OK" 15
printf '/root/meta-loop &\n' >&3
sleep "$KILL"
kill -9 $QPID 2>/dev/null; wait $QPID 2>/dev/null; exec 3>&-; rm -f "$F1"
cp vdisk.img /tmp/crash.img    # リカバリ前の生イメージ

F2=/tmp/o2.$$; L2=/tmp/ol2.$$; mkfifo "$F2"; exec 4<>"$F2"; launch "$F2" "$L2"
wait_for "$L2" "Welcome" 120; sleep 1
printf 'insmod /mnt/host/llfs/llfs.ko\n' >&4
printf 'mount -t llfs /dev/sda /mnt/host/llfs/mnt && echo MOUNT2_OK\n' >&4
wait_for "$L2" "MOUNT2_OK" 20
printf 'umount /mnt/host/llfs/mnt && echo UMOUNT_OK\n' >&4
wait_for "$L2" "UMOUNT_OK" 20; sleep 1
kill -9 $QPID 2>/dev/null; wait $QPID 2>/dev/null; exec 4>&-; rm -f "$F2"

echo "=== recovery messages ==="; grep -E "journal|replay|recover|torn|commit" "$L2" | head
echo "=== fsck post-recovery ==="
if uv run modules/llfs/fsck.py vdisk.img > /tmp/of.$$ 2>&1; then
  echo "CONSISTENT"; tail -1 /tmp/of.$$
else
  echo "INCONSISTENT -> saving /tmp/bad.img (post-recovery) /tmp/crash.img (pre-recovery)"
  cp vdisk.img /tmp/bad.img
  grep -E "INCONSISTENT|LEAK|DANGLING|CROSS|UNMARK" /tmp/of.$$ | head -6
  echo "--- inspect POST-recovery (bad.img) ---"; uv run modules/llfs/inspect.py /tmp/bad.img
  echo "--- inspect PRE-recovery (crash.img) ---"; uv run modules/llfs/inspect.py /tmp/crash.img
fi
rm -f "$L1" "$L2" /tmp/of.$$
