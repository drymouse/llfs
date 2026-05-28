obj-m = hello.o

# make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules

all:
	make -C /home/mouse/Documents/is/lab3/sngw/fs-test/linux-7.0.10 M=$(PWD) modules

clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean
