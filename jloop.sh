#!/bin/bash
# jloop.sh - クラッシュ整合性を複数回ランダムタイミングで検証。
#   各ラウンド: fresh mkfs → boot/mount/churn → ランダム遅延で kill -9(電源断)
#              → reboot/mount(=recovery)/umount → fsck。
#   目的: replay 窓(commit 済み・checkpoint 未完)に当て、replay 経路 + 整合性を確認。
set -u
cd /home/mouse/Documents/is/lab3/sngw/fs-test

QEMU=qemu-system-x86_64
APPEND="root=/dev/sda rw console=ttyS0 oops=panic panic=-1 nokaslr"
PROJECT_ROOT="./modules"
BZ=linux-7.0.10/arch/x86_64/boot/bzImage
ROUNDS=${1:-6}

wait_for() { for ((i=0;i<$3;i++)); do grep -q "$2" "$1" 2>/dev/null && return 0; sleep 1; done; return 1; }
launch() {
  "$QEMU" -enable-kvm -cpu host -m 2G -smp 2 -kernel "$BZ" -initrd initramfs.cpio \
    -nographic -no-reboot -net user -net nic -append "$APPEND" \
    -drive file=vdisk.img,format=raw,index=1,media=disk \
    -virtfs "local,id=host,path=$PROJECT_ROOT,security_model=none,mount_tag=host" \
    < "$1" > "$2" 2>&1 &
  QPID=$!
}

pass=0; fail=0; replays=0
for ((r=1;r<=ROUNDS;r++)); do
  # ランダム遅延 1.5〜9.0s
  d=$(awk -v s=$RANDOM 'BEGIN{srand(s);printf "%.1f",3.0+rand()*8.0}')
  echo "===== ROUND $r/$ROUNDS  (kill after ${d}s churn) ====="
  uv run modules/llfs/mkfs.py vdisk.img >/dev/null

  # Phase1
  F1=/tmp/ji1.$$.$r; L1=/tmp/jl1.$$.$r; mkfifo "$F1"; exec 3<>"$F1"
  launch "$F1" "$L1"
  if ! wait_for "$L1" "Welcome to Minimal" 120; then echo "  BOOT1 TIMEOUT"; kill -9 $QPID 2>/dev/null; exec 3>&-; rm -f "$F1"; continue; fi
  sleep 1
  printf 'insmod /mnt/host/llfs/llfs.ko\n' >&3
  printf 'mount -t llfs /dev/sda /mnt/host/llfs/mnt && echo MOUNT_OK\n' >&3
  wait_for "$L1" "MOUNT_OK" 15
  printf '/root/meta-loop &\n' >&3
  sleep "$d"
  kill -9 $QPID 2>/dev/null; wait $QPID 2>/dev/null
  exec 3>&-; rm -f "$F1"

  # Phase2 (recovery)
  F2=/tmp/ji2.$$.$r; L2=/tmp/jl2.$$.$r; mkfifo "$F2"; exec 4<>"$F2"
  launch "$F2" "$L2"
  if ! wait_for "$L2" "Welcome to Minimal" 120; then echo "  BOOT2 TIMEOUT"; kill -9 $QPID 2>/dev/null; exec 4>&-; rm -f "$F2"; continue; fi
  sleep 1
  printf 'insmod /mnt/host/llfs/llfs.ko\n' >&4
  printf 'mount -t llfs /dev/sda /mnt/host/llfs/mnt && echo MOUNT2_OK\n' >&4
  wait_for "$L2" "MOUNT2_OK" 20
  printf 'umount /mnt/host/llfs/mnt && echo UMOUNT_OK\n' >&4
  wait_for "$L2" "UMOUNT_OK" 20
  sleep 1
  kill -9 $QPID 2>/dev/null; wait $QPID 2>/dev/null
  exec 4>&-; rm -f "$F2"

  # recovery 種別
  if grep -q "replaying journal" "$L2"; then echo "  recovery: REPLAY"; replays=$((replays+1));
  elif grep -q "not committed (torn)" "$L2"; then echo "  recovery: TORN-DISCARD";
  else echo "  recovery: empty-journal"; fi

  # fsck
  if uv run modules/llfs/fsck.py vdisk.img > /tmp/jfsck.$$.$r 2>&1; then
    echo "  fsck: CONSISTENT"; pass=$((pass+1))
  else
    echo "  fsck: INCONSISTENT"; fail=$((fail+1)); sed -n '/INCONSISTENT/,$p' /tmp/jfsck.$$.$r | head -15
  fi
  rm -f "$L1" "$L2" /tmp/jfsck.$$.$r
done

echo "================================================"
echo "RESULT: pass=$pass fail=$fail (replay-rounds=$replays / $ROUNDS)"
[ "$fail" -eq 0 ] && echo "ALL CONSISTENT" || echo "SOME INCONSISTENT"
