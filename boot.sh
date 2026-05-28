set -x

APPEND="root=/dev/sda rw console=ttyS0 oops=panic panic=-1"
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
  -net user,hostfwd=tcp::2222-:22 
  -net nic 
  -append "$APPEND"
  -virtfs "local,id=host,path=$PROJECT_ROOT,security_model=none,mount_tag=host"
)

while [ $# -gt 0 ]; do
    case "$1" in
        --gdb)      GDB_MODE=1; shift ;;
        --test)     shift; TEST_CMD="${1:?--test requires a command}"; shift ;;
        --)         shift; EXTRA_QEMU_ARGS=("$@"); break ;;
        -h|--help)
            echo "Usage: $0 [--gdb] [--test CMD] [-- QEMU_ARGS...]"
            exit 0 ;;
        *)          die "Unknown option: $1" ;;
    esac
done

QEMU_ARGS+=("${EXTRA_QEMU_ARGS[@]+"${EXTRA_QEMU_ARGS[@]}"}")

qemu-system-x86_64 "${QEMU_ARGS[@]}"
