import gdb

class LoadSymbol(gdb.Command):
    def __init__(self):
        super(LoadSymbol, self).__init__("loadsym", gdb.COMMAND_USER)

    def invoke(self, arg, from_tty):
        gdb.execute("b panic")
        gdb.execute("add-symbol-file ../../fs-test/modules/dummyfs/dummyfs.ko 0xffffffffc0000000")
        gdb.execute("b dummyfs_get_tree")
        gdb.execute("b vfs_get_tree")
        

LoadSymbol()

gdb.execute("gef")
gdb.execute("target remote :12345")
gdb.execute("c")
