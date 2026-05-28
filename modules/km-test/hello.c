#include <linux/module.h>
#include <linux/kernel.h>

static int my_init_module(void) {
  printk(KERN_INFO "Hello world!\n");

  return 0;
}

// void mcount(void) {};

static void my_cleanup_module(void) {
  printk(KERN_INFO "Goodbye world!\n");
}

module_init(my_init_module);
module_exit(my_cleanup_module);

MODULE_LICENSE("GPL");
