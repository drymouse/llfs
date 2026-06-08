set -x

APPEND="root=/dev/sda rw console=ttyS0 oops=panic panic=-1 nokaslr"
PROJECT_ROOT="./modules"

die() { echo "ERROR: $*" >&2; exit 1; }

QEMU_ARGS=(
  -enable-kvm 
  -m 2G 
  -smp 2 
  -kernel linux-7.0.10/arch/x86_64/boot/bzImage 
  # -drive file=rootfs.img,format=raw,index=1,media=disk 
  -initrd initramfs.cpio
  -nographic 
  -no-reboot
  -net user,hostfwd=tcp::2222-:22 
  -net nic 
  -append "$APPEND"
  -virtfs "local,id=host,path=$PROJECT_ROOT,security_model=none,mount_tag=host"
)

while [ $# -gt 0 ]; do
    case "$1" in
        --gdb)      GDB_MODE=1; shift ;;
        --test)     shift; TEST_CMD="${1:?--test requires a command}"; shift ;;
        --bdev)     WITH_BDEV=1; shift ;;
        --refresh)  REFRESH=1; shift ;;
        --)         shift; EXTRA_QEMU_ARGS=("$@"); break ;;
        -h|--help)
            echo "Usage: $0 [--gdb] [--test CMD] [-- QEMU_ARGS...]"
            exit 0 ;;
        *)          die "Unknown option: $1" ;;
    esac
done

if [ "$WITH_BDEV" -eq 1 ]; then
    QEMU_ARGS+=(-drive file=vdisk.img,format=raw,index=1,media=disk)
    # echo "QEMU waiting for GDB on localhost:$QEMU_GDB_PORT"
fi

if [ "$REFRESH" -eq 1]; then
    uv run ./modules/llfs/mkfs.py ./vdisk.img
fi

QEMU_ARGS+=("${EXTRA_QEMU_ARGS[@]+"${EXTRA_QEMU_ARGS[@]}"}")

qemu-system-x86_64 "${QEMU_ARGS[@]}"
