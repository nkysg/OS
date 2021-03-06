// File system implementation.  Five layers:
//   + Blocks: allocator for raw disk blocks.
//   + Log: crash recovery for multi-step updates.
//   + Files: inode allocator, reading, writing, metadata.
//   + Directories: inode with special contents (list of other inodes!)
//   + Names: paths like /usr/rtm/xv6/fs.c for convenient naming.
//
// This file contains the low-level file system manipulation 
// routines.  The (higher-level) system call implementations
// are in sysfile.c.

#include "types.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "mmu.h"
#include "proc.h"
#include "spinlock.h"
#include "buf.h"
#include "fs.h"
#include "file.h"

#define min(a, b) ((a) < (b) ? (a) : (b))
static void itrunc(struct inode*);

// Read the super block.
void
readsb(int dev, struct superblock *sb)
{
  struct buf *bp;
  
  bp = bread(dev, 1);
  memmove(sb, bp->data, sizeof(*sb));
  brelse(bp);
}

// Zero a block (on the cache)
static void
bzero(int dev, int bno)
{
  struct buf *bp;
  
  bp = bread(dev, bno);
  memset(bp->data, 0, BSIZE);
  log_write(bp);
  brelse(bp);
}

// Blocks. 

// Allocate a zeroed disk block.
// by setting its corresponding bit in bitmap to 1
// and also zero the block on the cache
static uint
balloc(uint dev)
{
  int b, bi, m;
  struct buf *bp;
  struct superblock sb;

  bp = 0;
  readsb(dev, &sb);
  // BPB = 512 * 8, means using a block can hold 512 * 8 bits.
  // and each bit can be used to reprenset the status of a block.
  for (b = 0; b < sb.size; b += BPB) {
    // read a bitmap block
    bp = bread(dev, BBLOCK(b, sb.ninodes));
    // b + bi = sector id in the disk
    for (bi = 0; bi < BPB && b + bi < sb.size; bi++) {
      // generate bit mask
      // 0x01, 0x02, 0x04 0x08, 0x10, 0x20, 0x40, 0x80
      m = 1 << (bi % 8);
      if ((bp->data[bi/8] & m) == 0){  // Is block free?
        bp->data[bi/8] |= m;  // Mark block in use.
        log_write(bp);
        brelse(bp);
        bzero(dev, b + bi);
        return b + bi;
      }
    }
    brelse(bp);
  }
  panic("balloc: out of blocks");
}

// Free a disk block, by setting its corresponding bit in bitmap
// to 0
static void
bfree(int dev, uint b) {
  struct buf *bp;
  struct superblock sb;
  int bi, m;

  readsb(dev, &sb);
  bp = bread(dev, BBLOCK(b, sb.ninodes));
  bi = b % BPB;
  m = 1 << (bi % 8);
  if((bp->data[bi/8] & m) == 0)
    panic("freeing free block");
  bp->data[bi/8] &= ~m;
  log_write(bp);
  brelse(bp);
}

// Inodes.
//
// An inode describes a single unnamed file.
// The inode disk structure holds metadata: the file's type,
// its size, the number of links referring to it, and the
// list of blocks holding the file's content.
//
// The inodes are laid out sequentially on disk immediately after
// the superblock. Each inode has a number, indicating its
// position on the disk.
//
// The kernel keeps a cache of in-use inodes in memory
// to provide a place for synchronizing access
// to inodes used by multiple processes. The cached
// inodes include book-keeping information that is
// not stored on disk: ip->ref and ip->flags.
//
// An inode and its in-memory represtative go through a
// sequence of states before they can be used by the
// rest of the file system code.
//
// * Allocation: an inode is allocated if its type (on disk)
//   is non-zero. ialloc() allocates, iput() frees if
//   the link count has fallen to zero.
//
// * Referencing in cache: an entry in the inode cache
//   is free if ip->ref is zero. Otherwise ip->ref tracks
//   the number of in-memory pointers to the entry (open
//   files and current directories). iget() to find or
//   create a cache entry and increment its ref, iput()
//   to decrement ref.
//
// * Valid: the information (type, size, &c) in an inode
//   cache entry is only correct when the I_VALID bit
//   is set in ip->flags. ilock() reads the inode from
//   the disk and sets I_VALID, while iput() clears
//   I_VALID if ip->ref has fallen to zero.
//
// * Locked: file system code may only examine and modify
//   the information in an inode and its content if it
//   has first locked the inode. The I_BUSY flag indicates
//   that the inode is locked. ilock() sets I_BUSY,
//   while iunlock clears it.
//
// Thus a typical sequence is:
//   ip = iget(dev, inum)
//   ilock(ip)
//   ... examine and modify ip->xxx ...
//   iunlock(ip)
//   iput(ip)
//
// ilock() is separate from iget() so that system calls can
// get a long-term reference to an inode (as for an open file)
// and only lock it for short periods (e.g., in read()).
// The separation also helps avoid deadlock and races during
// pathname lookup. iget() increments ip->ref so that the inode
// stays cached and pointers to it remain valid.
//
// Many internal file system functions expect the caller to
// have locked the inodes involved; this lets callers create
// multi-step atomic operations.

struct {
  struct spinlock lock;

  // in memory copy of inode on disk,
  // served as a cache for indoe
  struct inode inode[NINODE]; 
} icache;

void
iinit(void)
{
  initlock(&icache.lock, "icache");
}

static struct inode* iget(uint dev, uint inum);

//PAGEBREAK!
// Allocate a new inode with the given type on device dev.
// A free inode has a type of zero.
struct inode*
ialloc(uint dev, short type)
{
  int inum;
  struct buf *bp;
  struct dinode *dip;
  struct superblock sb;

  readsb(dev, &sb);

  for(inum = 1; inum < sb.ninodes; inum++){
    // read the block that contains the inode inum.
    bp = bread(dev, IBLOCK(inum));
    // read inum's data from the block bp
    dip = (struct dinode*)bp->data + inum%IPB;

    if(dip->type == 0){  // a free inode
      memset(dip, 0, sizeof(*dip));
      dip->type = type;
      log_write(bp);   // mark it allocated on the disk
      brelse(bp);
      // given dev and inum
      // return the in-memory indoe buffer
      return iget(dev, inum);
    }
    brelse(bp);
  }
  panic("ialloc: no inodes");
}

// Copy a modified in-memory inode to disk.
void
iupdate(struct inode *ip)
{
  struct buf *bp;
  struct dinode *dip;
  
  // read the data from disk to buffer cache
  bp = bread(ip->dev, IBLOCK(ip->inum));
  dip = (struct dinode*)bp->data + ip->inum%IPB;

  // copy the in-memory inode to buffer cached inode
  dip->type = ip->type;
  dip->major = ip->major;
  dip->minor = ip->minor;
  dip->nlink = ip->nlink;
  dip->size = ip->size;

  memmove(dip->addrs, ip->addrs, sizeof(ip->addrs));
  
  log_write(bp);
  brelse(bp);
}

// Find the inode with number inum on device dev
// and return the in-memory copy. Does not lock
// the inode and does not read it from disk.
static struct inode*
iget(uint dev, uint inum)
{
  struct inode *ip, *empty;

  acquire(&icache.lock);

  // Is the inode already cached?
  empty = 0;
  for(ip = &icache.inode[0]; ip < &icache.inode[NINODE]; ip++){
    if(ip->ref > 0 && ip->dev == dev && ip->inum == inum){
      ip->ref++;
      release(&icache.lock);
      return ip;
    }
    if(empty == 0 && ip->ref == 0)    // Remember empty slot.
      empty = ip;
  }

  // Recycle an inode cache entry.
  if(empty == 0)
    panic("iget: no inodes");

  ip = empty;
  ip->dev = dev;
  ip->inum = inum;
  ip->ref = 1;
  ip->flags = 0;
  release(&icache.lock);

  return ip;
}

// Increment reference count for ip.
// Returns ip to enable ip = idup(ip1) idiom.
struct inode*
idup(struct inode *ip)
{
  acquire(&icache.lock);
  ip->ref++;
  release(&icache.lock);
  return ip;
}

// Lock the given inode.
// Reads the inode from disk if necessary.
void
ilock(struct inode *ip)
{
  struct buf *bp;
  struct dinode *dip;

  if(ip == 0 || ip->ref < 1)
    panic("ilock");

  acquire(&icache.lock);
  while(ip->flags & I_BUSY)
    sleep(ip, &icache.lock);
  ip->flags |= I_BUSY;
  release(&icache.lock);
  
  // reads the inode from disk if necessary
  if(!(ip->flags & I_VALID)){
    bp = bread(ip->dev, IBLOCK(ip->inum));
    dip = (struct dinode*)bp->data + ip->inum%IPB;
    ip->type = dip->type;
    ip->major = dip->major;
    ip->minor = dip->minor;
    ip->nlink = dip->nlink;
    ip->size = dip->size;
    memmove(ip->addrs, dip->addrs, sizeof(ip->addrs));
    brelse(bp);
    ip->flags |= I_VALID;
    if(ip->type == 0)
      panic("ilock: no type");
  }
}

// Unlock the given inode.
void
iunlock(struct inode *ip)
{
  if(ip == 0 || !(ip->flags & I_BUSY) || ip->ref < 1)
    panic("iunlock");

  acquire(&icache.lock);
  ip->flags &= ~I_BUSY;
  wakeup(ip);
  release(&icache.lock);
}

// Drop a reference to an in-memory inode.
// If that was the last reference, the inode cache entry can
// be recycled.
// If that was the last reference and the inode has no links
// to it, free the inode (and its content) on disk.
void
iput(struct inode *ip)
{
  acquire(&icache.lock);
  if(ip->ref == 1 && (ip->flags & I_VALID) && ip->nlink == 0){
    // inode has no links: truncate and free inode.
    if(ip->flags & I_BUSY)
      panic("iput busy");
    ip->flags |= I_BUSY;
    release(&icache.lock);

    itrunc(ip);
    ip->type = 0;
    iupdate(ip);

    acquire(&icache.lock);
    ip->flags = 0;
    wakeup(ip);
  }
  ip->ref--;
  release(&icache.lock);
}

// Common idiom: unlock, then put.
void
iunlockput(struct inode *ip)
{
  iunlock(ip);
  iput(ip);
}

//PAGEBREAK!
// Inode content
//
// The content (data) associated with each inode is stored
// in blocks on the disk. The first NDIRECT block numbers
// are listed in ip->addrs[].  The next NINDIRECT blocks are 
// listed in block ip->addrs[NDIRECT].

// Return the disk block address of the nth block in inode ip.
// If there is no such block, bmap allocates one.
//
// In other words, given ip and bn,
// return the content of ip->addrs[bn], which is the sector number
// (aka. address). If ip->adrs[bn] = 0, then call balloc to allocate
// a new one for it.
static uint
bmap(struct inode *ip, uint bn) {
  uint addr, *a;
  struct buf *bp;
  
  // case 1: direct: bn in [0,11)
  if (bn < NDIRECT) {
    if ((addr = ip->addrs[bn]) == 0) {
      ip->addrs[bn] = addr = balloc(ip->dev);
    }
    return addr;
  } 
  
  // case 2: singled-linked: original bn in [11, 139)
  bn -= NDIRECT;
  if (bn < NINDIRECT) {
    // if the single-linked lookup talbe doesn't exist
    // allocate it
    if (0 == (addr = ip->addrs[SINGLE_LINKED_INDIRECT_TABLE])) {
      ip->addrs[SINGLE_LINKED_INDIRECT_TABLE] = addr = balloc(ip->dev);
    }

    // read the single-linked lookup talbe
    bp = bread(ip->dev, addr);
    a = (uint*)bp->data;

    // indirect lookup
    if (0 == (addr = a[bn])) {
      // if doesn't exit, allocate one
      a[bn] = addr = balloc(ip->dev);
      log_write(bp);
    }

    //clean up
    brelse(bp);
    return addr;
  }

  // case 3: double-linked: original bn in [139, 16523)
  bn -= NINDIRECT;
  if (bn < NDINDIRECT) {
    if (0 == (addr = ip->addrs[DOUBLE_LINKED_INDIRECT_TABLE])) {
      ip->addrs[DOUBLE_LINKED_INDIRECT_TABLE] = addr = balloc(ip->dev);
    }

    bp = bread(ip->dev, addr);
    a = (uint*)bp->data;
    
    // first-level indirect lookup
    uint idx1 = bn / NINDIRECT;
    if (0 == (addr = a[idx1])) {
      a[idx1] = addr = balloc(ip->dev);
      log_write(bp);
    }
    brelse(bp);
  
    // second-level indirect lookup
    struct buf* bp2;
    uint idx2 = bn % NINDIRECT; 
    bp2 = bread(ip->dev, addr);
    a = (uint*)bp2->data;
    if (0 == (addr = a[idx2])) {
      a[idx2] = addr = balloc(ip->dev);
      log_write(bp2);
    }
    brelse(bp2);

    return addr;
  }
  panic("bmap: out of range");
}

// Truncate inode (discard contents).
// Only called when the inode has no links
// to it (no directory entries referring to it)
// and has no in-memory reference to it (is
// not an open file or current directory).
//
// What this function do is , in other words,
// for_each(ip->addrs, bfree)
static void
itrunc(struct inode *ip)
{
  int i, j, k;
  struct buf *bp, *bp2;
  uint *a, *b;
  
  // free direct block
  for (i = 0; i < NDIRECT; i++){
    if (ip->addrs[i]) {
      bfree(ip->dev, ip->addrs[i]);
      ip->addrs[i] = 0;
    }
  }
  
  // free single-linked indirect block
  if (ip->addrs[SINGLE_LINKED_INDIRECT_TABLE]) {
    bp = bread(ip->dev, ip->addrs[SINGLE_LINKED_INDIRECT_TABLE]);
    a = (uint*)bp->data;
    for (j = 0; j < NINDIRECT; ++j) {
      if (a[j]) {
        bfree(ip->dev, a[j]);
      }
    }
    brelse(bp);
    bfree(ip->dev, ip->addrs[SINGLE_LINKED_INDIRECT_TABLE]);
    ip->addrs[SINGLE_LINKED_INDIRECT_TABLE] = 0;
  }
  
  // free double-linked indirect block
  if (ip->addrs[DOUBLE_LINKED_INDIRECT_TABLE]) {
    bp = bread(ip->dev, ip->addrs[DOUBLE_LINKED_INDIRECT_TABLE]);
    a = (uint*)bp->data;
    for (j = 0; j < NINDIRECT; ++j) {
      if (a[j]) {
        bp2 = bread(ip->dev, a[j]);
        b = (uint*)bp2->data;
        for (k = 0; k < NINDIRECT; ++k) {
          if (b[k]) {
            bfree(ip->dev, b[k]);
          }
        }
        brelse(bp2);
        bfree(ip->dev, a[j]);
      }
    }
    brelse(bp);
    bfree(ip->dev, ip->addrs[DOUBLE_LINKED_INDIRECT_TABLE]);
    ip->addrs[DOUBLE_LINKED_INDIRECT_TABLE] = 0;
  }

  ip->size = 0;
  iupdate(ip);
}

// Copy stat information from inode.
void
stati(struct inode *ip, struct stat *st)
{
  st->dev = ip->dev;
  st->ino = ip->inum;
  st->type = ip->type;
  st->nlink = ip->nlink;
  st->size = ip->size;
}

//PAGEBREAK!
// Read data from inode.
int
readi(struct inode *ip, char *dst, uint off, uint n)
{
  uint tot, m;
  struct buf *bp;
  
  // for device r
  if (ip->type == T_DEV) {
    if (ip->major < 0 || ip->major >= NDEV || !devsw[ip->major].read)
      return -1;
    return devsw[ip->major].read(ip, dst, n);
  }
  
  // for disk file r
  if(off > ip->size || off + n < off)
    return -1;

  // update n
  if(off + n > ip->size)
    n = ip->size - off;

  // tot : total of current read
  for ( tot = 0; tot < n; tot += m, off += m, dst += m){
    // bmap return the sector number
    bp = bread(ip->dev, bmap(ip, off/BSIZE));

    // 
    m = min(n - tot, BSIZE - off % BSIZE);

    // move read data to dst
    // (bp->data + off % BSIZE) is the start address of
    // data corresponding to the current offset
    memmove(dst, bp->data + off % BSIZE, m);

    brelse(bp);
  }
  return n;
}

// PAGEBREAK!
// Write data to inode.
int
writei(struct inode *ip, char *src, uint off, uint n)
{
  uint tot, m;
  struct buf *bp;

  if(ip->type == T_DEV){
    if(ip->major < 0 || ip->major >= NDEV || !devsw[ip->major].write)
      return -1;
    return devsw[ip->major].write(ip, src, n);
  }

  if(off > ip->size || off + n < off)
    return -1;
  if(off + n > MAXFILE*BSIZE)
    return -1;

  for(tot=0; tot<n; tot+=m, off+=m, src+=m){
    // bmap will allocat new block
    bp = bread(ip->dev, bmap(ip, off/BSIZE));

    m = min(n - tot, BSIZE - off%BSIZE);

    // memmove: only update the cache ?
    memmove(bp->data + off%BSIZE, src, m);

    log_write(bp);
    brelse(bp);
  }

  if (n > 0 && off > ip->size){
    ip->size = off;
    iupdate(ip);
  }

  return n;
}

//PAGEBREAK!
// Directories

int
namecmp(const char *s, const char *t)
{
  return strncmp(s, t, DIRSIZ);
}

// Look for a directory entry in a directory.
// If found, set *poff to byte offset of entry.
//
// Look for file (directory is also a file) which name is *name in the directory
// which is represented by inode dp.
// Looking is performing by look through the content of the inode which
// is an array of struct dirent. 
// If found set the poff and return the inode pointer of the file
struct inode*
dirlookup(struct inode *dp, char *name, uint *poff)
{
  uint off, inum;
  struct dirent de;

  if(dp->type != T_DIR)
    panic("dirlookup not DIR");

  for(off = 0; off < dp->size; off += sizeof(de)){
    if(readi(dp, (char*)&de, off, sizeof(de)) != sizeof(de))
      panic("dirlink read");
    if(de.inum == 0)
      continue;
    if(namecmp(name, de.name) == 0){
      // entry matches path element
      if(poff)
        *poff = off;
      inum = de.inum;
      return iget(dp->dev, inum);
    }
  }
  return 0;
}

// Write a new directory entry (name, inum) into the directory dp.
//
// given a file whose name is name and its inode number is inum,
// add this file to the directory pointed by dp.
int
dirlink(struct inode *dp, char *name, uint inum)
{
  int off;
  struct dirent de;
  struct inode *ip;

  // Check that name is not present.
  if((ip = dirlookup(dp, name, 0)) != 0){
    // dirlookup if found will call iget to return, which will increase
    // the count, so we need ip to release
    iput(ip);
    return -1;
  }

  // Look for an empty dirent.
  for(off = 0; off < dp->size; off += sizeof(de)){
    if(readi(dp, (char*)&de, off, sizeof(de)) != sizeof(de))
      panic("dirlink read");

    // inoe number = 0 -> indicate it is a empty ?
    if(de.inum == 0)
      break;
  }

  strncpy(de.name, name, DIRSIZ);
  de.inum = inum;

  // write to the entry of directory
  if(writei(dp, (char*)&de, off, sizeof(de)) != sizeof(de))
    panic("dirlink");
  
  return 0;
}

//PAGEBREAK!
// Paths

// Copy the next path element from path into name.
// Return a pointer to the element following the copied one.
// The returned path has no leading slashes,
// so the caller can check *path=='\0' to see if the name is the last one.
// If no name to remove, return 0.
//
// Examples:
//   skipelem("a/bb/c", name) = "bb/c", setting name = "a"
//   skipelem("///a//bb", name) = "bb", setting name = "a"
//   skipelem("a", name) = "", setting name = "a"
//   skipelem("", name) = skipelem("////", name) = 0
//
// skipelem("/name1/name2/name3", name)
// skip the topest part of the path, that is "/name1/"
// and set name = "name1". return a pointer to the next part,
// that is "name2/name3"
static char*
skipelem(char *path, char *name)
{
  char *s;
  int len;
  
  // skip the leading slash
  while(*path == '/')
    path++;
  if(*path == 0)
    return 0;
  s = path;

  // get the first part of the path,
  // also skipping leading slash
  while(*path != '/' && *path != 0)
    path++;
  len = path - s;
  if(len >= DIRSIZ) {
    memmove(name, s, DIRSIZ);
  } else {
    memmove(name, s, len);
    name[len] = 0;
  }
  
  // return a pointer points to the start of the second part,
  // also skipping leading slash
  while(*path == '/')
    path++;
  return path;
}

// Look up and return the inode for a path name.
// If nameiparent != 0, return the inode for the parent and copy the final
// path element into name, which must have room for DIRSIZ bytes.
static struct inode*
namex(char *path, int nameiparent, char *name)
{
  struct inode *ip, *next;

  if(*path == '/') {
    ip = iget(ROOTDEV, ROOTINO);
  } else {
    ip = idup(proc->cwd);     // increase the ref-count of inode
  }


  // considering this exampe path = "./name1/name2", nameiparent = 0
  // before first iteration: path = "./name1/name2", ip -> "./" (aka current directory)
  while((path = skipelem(path, name)) != 0){
    // first-iteration:
    // after skipelem
    // path = "name2", name = "name1"
    
    // second-iteration:
    // after skipelem
    // path = "", name = "name2"

    ilock(ip);
  
    // if current inode is directory, if not loopup failed.
    if(ip->type != T_DIR){
      iunlockput(ip);
      return 0;
    }
    
    if(nameiparent && *path == '\0'){
      // if nameiparent is set
      // ip points to the parent directory of the file
      // ./name1/name2
      // so return the inode pointer of the parent directory
      iunlock(ip);
      return ip;
    }
    
    // first-iteration:
    // lookup name = "name1" in ip -> "./"
    // if found, next -> "./name1"

    // second-iteration:
    // lookup name = "name2" in ip -> "./name1"
    // if found, next -> "./name1/name2
    if((next = dirlookup(ip, name, 0)) == 0) {
      iunlockput(ip);
      return 0;
    }

    iunlockput(ip);

    ip = next;
  }
  
  // this for case such as "./name1", so return "./"
  if (nameiparent) {
    iput(ip);
    return 0;
  }
  
  // finally, we get the inode pointer points to the file
  // corresponding to path "./name1/name2"
  return ip;
}

// given a path to a file, return its
// corresponding inode pointer
struct inode*
namei(char *path)
{
  char name[DIRSIZ];
  return namex(path, 0, name);
}

// given a path to a file, return
// a inode pointer to its direct parent
struct inode*
nameiparent(char *path, char *name)
{
  return namex(path, 1, name);
}
