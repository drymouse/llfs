#!/usr/bin/env python3
"""inspect.py - llfs イメージのメタデータを詳細ダンプして不整合の原因を可視化する。"""
import sys, struct

BLOCK_SIZE=4096; INODE_SIZE=64; N_BLOCKS=13; SB_BLOCK=1; ROOT_INO=1
MAX_INODES=BLOCK_SIZE//INODE_SIZE
DIRENT_HEADER=8

def rd(img,n): return img[n*BLOCK_SIZE:(n+1)*BLOCK_SIZE]
def bit(b,i): return (b[i//8]>>(i%8))&1

def main():
    path=sys.argv[1] if len(sys.argv)>1 else "vdisk.img"
    img=open(path,"rb").read()
    sb=rd(img,SB_BLOCK)
    magic,bs,itb,ibm,bbm=struct.unpack_from("<IIIII",sb,0)
    (isz,)=struct.unpack_from("<H",sb,20)
    jblk,jblks=struct.unpack_from("<II",sb,24)
    print(f"SB magic={magic:#x} itable={itb} ibmap={ibm} bbmap={bbm} jblk={jblk} jblks={jblks}")
    itable=rd(img,itb); ibmap=rd(img,ibm); bbmap=rd(img,bbm)

    # dirents
    root_blocks=struct.unpack_from("<%dI"%N_BLOCKS,itable,ROOT_INO*INODE_SIZE+12)
    dref={}
    for b in root_blocks:
        if b==0: continue
        p=0; blk=rd(img,b)
        while p+DIRENT_HEADER<=BLOCK_SIZE:
            ino,rl,nl,ft=struct.unpack_from("<IHBB",blk,p)
            if rl<DIRENT_HEADER: break
            nm=blk[p+DIRENT_HEADER:p+DIRENT_HEADER+nl].decode("latin1")
            if ino and nm not in(".",".."): dref.setdefault(ino,[]).append(nm)
            p+=rl

    # per-inode dump
    print("\n=== inodes (bm/mode/size/blocks/dirent/block[]) ===")
    owned={}
    for ino in range(MAX_INODES):
        mode,uid,size,blocks=struct.unpack_from("<HHII",itable,ino*INODE_SIZE)
        blk=list(struct.unpack_from("<%dI"%N_BLOCKS,itable,ino*INODE_SIZE+12))
        nz=[b for b in blk if b]
        inbm=bit(ibmap,ino)
        ref=dref.get(ino,[])
        for b in nz: owned.setdefault(b,[]).append(ino)
        if not(inbm or mode or ref or nz): continue
        print(f"ino {ino:2d}: bm={inbm} mode={mode:#06o} size={size:5d} blocks={blocks:2d} dirent={ref} nblk={len(nz)} blk={nz[:4]}{'...' if len(nz)>4 else ''}")

    # block bitmap used
    used=[b for b in range(BLOCK_SIZE*8) if bit(bbmap,b)]
    fixed=jblk+jblks if jblks else 5
    print(f"\n=== blocks: bm_used={len(used)} (max {used[-1] if used else 0}) fixed<= {fixed} ===")
    leak=[b for b in used if b>fixed and b not in owned]
    unmarked=[b for b in owned if b<BLOCK_SIZE*8 and not bit(bbmap,b)]
    cross={b:o for b,o in owned.items() if len(o)>1}
    print(f"LEAK (bm used, no owner): {len(leak)} -> {leak[:20]}{'...' if len(leak)>20 else ''}")
    print(f"UNMARKED (owned, bm free): {len(unmarked)} -> {unmarked[:20]}")
    print(f"CROSSLINK: {cross}")

if __name__=="__main__": main()
