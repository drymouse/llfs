insmod /mnt/host/llfs/llfs.ko
mount -t llfs /dev/sda /mnt/host/llfs/mnt
# メタデータ更新負荷(電源断で不整合を起こすテスト)。データのみ負荷は /root/write-loop
/root/meta-loop
