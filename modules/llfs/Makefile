obj-m += llfs.o
llfs-y += super.o inode.o iomap.o

all:
	make -C /home/mouse/Documents/is/lab3/sngw/fs-test/linux-7.0.10 M=$(PWD) modules

clean:
	make -C /home/mouse/Documents/is/lab3/sngw/fs-test/linux-7.0.10 M=$(PWD) clean
