#include "types.h"
#include "kernel.hh"
#include "spinlock.h"
#include "condvar.h"
#include "atomic.hh"
#include "proc.hh"
#include "cpu.hh"
#include "cpputil.hh"
#include "percpu.hh"
#include "gc.hh"
#include "major.h"
#include "uk/gcstat.h"

using std::atomic;

// A simple RCU implementation, but general:
// - processes can call sleep in an epoch
// - processes can migrate during an epoch
//
// The GC scheme is adopted from Fraser's epoch-based scheme:
// a machine has a global_epoch
// a process add to its core global_epoch's delayed freelist on delayed_free()
// a core maintains an epoch, min_epoch (>= global_epoch)
//   min_epoch is the lowest epoch # a process on that core is in
//   a process always ends an epoch on the core it started (even if the
//   process is migrated)
// one gc thread and state (e.g., NEPOCH delaylist) per core
// a gcc thread performs two jobs:
// 1. one gcc thread perform step 1: updates global_epoch
//   (costs linear in the number of cores, but a global operation)
// 2. in parallel gc threads free the elements on the delayed-free lists
//   (costs linear in the number of elements to be freed, but a local operation)

enum { gc_debug = 0 };

// Head of a delayed free list. Always read and updated while holding lock_
struct headinfo {
  rcu_freed* head;
  u64 epoch;
};

struct gc_state { 
  atomic<u64> nexttofree_epoch; // the lowest epoch # to free on this core
  atomic<u64> min_epoch;        // the lowest epoch # a process on this core is in

  struct spinlock lock_ __mpalign__;
  struct condvar cv;
  headinfo delayed[NEPOCH];     // NEPOCH delayed-free lists
  gc_handle proclist;           // list of process in an epoch on this core
  int woken;
public:
  gc_state();
  void dequeue(gc_handle *h);
  void enqueue(gc_handle *h);
  gc_free(rcu_freed *r, u64 epoch);
};

percpu<gc_state, percpu_safety::internal> gc_state;
percpu<gc_stat, percpu_safety::internal> stat;
int ngc_cpu;
int gc_batchsize;

// Increment of global_epoch acquires gc_lock, but processes cannot read it
// without locks
static struct gc_lock {
  struct spinlock l __mpalign__;
  gc_lock() : l("gc", LOCKSTAT_GC) { }
} gc_lock;
atomic<u64> global_epoch __mpalign__;

// Increment global_epoch if (1) each core has freed all epochs <= global-2
// and (2) each core has no processes in an epoch <= global - 2
// This operation is the only common global operation.  
// XXX implement global min in scalable manner
static void
gc_inc_global_epoch(void)
{
  int r = tryacquire(&gc_lock.l);
  if (r == 0) return;
  assert(r == 1);
  u64 t0 = rdtsc();
  u64 global = global_epoch;  // make "local" copy
  u64 minfree = global;
  u64 minepoch = global;
  for (int c = 0; c < ngc_cpu; c++) { 
    // is reading nexttofree_epoch and min_epoch a single cache miss?
    if (gc_state[c].nexttofree_epoch < minfree) {
      minfree = gc_state[c].nexttofree_epoch;
    }
    if (gc_state[c].min_epoch < minepoch) {
      minepoch = gc_state[c].min_epoch;
    }
  }
  if ((minfree < global-2) || (minepoch < global-2)) 
    goto done;
  if (minepoch > global-2) {
    if (gc_debug) cprintf("update global_epoch to: %lu\n", minepoch+1);
    global_epoch = global + 1; 
  }
done:
  release(&gc_lock.l);
  u64 t1 = rdtsc();
  stat[mycpu()->id].ncycles += (t1-t0);
  stat[mycpu()->id].nop++;
}

gc_state::gc_state() : 
  lock_("gc_state", LOCKSTAT_GC), cv(condvar("gc_cv"))
{ 
  proclist.next = &proclist;
  proclist.prev = &proclist;
  for (int i = 0; i < NEPOCH; i++) {
    delayed[i].epoch = i;
  }
}

// caller should hold lock_
void
gc_state::enqueue(gc_handle *h)
{
  h->next = &this->proclist;
  h->prev = this->proclist.prev;
  this->proclist.prev->next = h;
  this->proclist.prev = h;
}

// caller should hold lock_
void
gc_state::dequeue(gc_handle *h)
{
  u64 m = global_epoch;
  for (gc_handle* entry = this->proclist.next; entry != &this->proclist;
       entry = entry->next) {
    if (entry == h) {
      entry->next->prev = entry->prev;
      entry->prev->next = entry->next;
    } else {
      if ((entry->epoch >> 8) < m) 
        m = (entry->epoch >> 8);
    }
  }
  if (this->min_epoch != m) {
    this->min_epoch = m;
  }
}

// free the elements in delayed-free list r (from epoch epoch).  
// Runs without holding _lock
int
gc_state::gc_free(rcu_freed *r, u64 epoch)
{
  int nfree = 0;
  rcu_freed *nr;
  for (; r; r = nr) {
    if (r->_rcu_epoch > epoch) {
      cprintf("gc_free: r->epoch %ld > epoch %ld\n", r->_rcu_epoch, epoch);
#if RCU_TYPE_DEBUG
      cprintf("gc_free: name %s\n", r->_rcu_type);
#endif
      assert(0);
    }
    nr = r->_rcu_next;
    r->do_gc();
    nfree++;
  }
  return nfree;
}

static void
gc_worker(void *x)
{
  if (VERBOSE)
    cprintf("gc_worker: %d\n", mycpu()->id);

  acquire(&gc_state->lock_);
  for (;;) {
    u64 i;
    gc_state->cv.sleep_to(&gc_state->lock_, 
                          nsectime() + ((u64)GCINTERVAL)*1000000ull);

    stat->nrun++;

    // if no processes are running on this core, update min_epoch
    if (gc_state->proclist.next == &gc_state->proclist) {
      gc_state->min_epoch = global_epoch.load();
    }

    // free all delayed-free lists until min_epoch
    for (i = gc_state->nexttofree_epoch; i < gc_state->min_epoch; i++) {
      rcu_freed *head = gc_state->delayed[i%NEPOCH].head;

      // give up lock during free
      release(&gc_state->lock_);

      int nfree = gc_state->gc_free(head,i);

      acquire(&gc_state->lock_);
      gc_state->delayed[i%NEPOCH].head = nullptr;
      gc_state->delayed[i%NEPOCH].epoch += NEPOCH;
      stat->nfree += nfree;
      if (gc_debug && nfree > 0) {
	cprintf("%d: epoch %lu freed %d\n", mycpu()->id, i, nfree);
      }

    }
    gc_state->nexttofree_epoch = i;

    // try to increment global_epoch
    gc_inc_global_epoch();
  }
}

static int
readstat(struct inode *inode, char *dst, u32 off, u32 n)
{
  size_t sz = sizeof(gc_stat);
  int i = off / sz;

  if (n != sz)
    return -1;

  if (i >= NCPU) {
    for (int n = 0; n < NCPU; n++)
      memset((void *) &stat[n], 0, sz);
    return 0;
  }

  memcpy(dst, &stat[i], sz);
  
  return n;
}

static int
writectrl(struct inode *inode, const char *buf, u32 off, u32 n)
{
  if (n != 2*sizeof(int))
    return -1;
  memcpy(&ngc_cpu, buf, sizeof(int));
  memcpy(&gc_batchsize, buf + sizeof(int), sizeof(int));
  cprintf("ngc_cpu %d gc_batchsize %d\n", ngc_cpu, gc_batchsize);
  return n;
}

//
// Public interface:
//

void
initgc(void)
{
  ngc_cpu = NCPU;
  global_epoch = NEPOCH-2;
  gc_batchsize = 100000000;
  
  devsw[MAJ_GC].write = writectrl;
  devsw[MAJ_GC].read = readstat;

  for (int c = 0; c < ncpu; c++) {
    char namebuf[32];
    snprintf(namebuf, sizeof(namebuf), "gc_%u", c);
    threadpin(gc_worker, 0, namebuf, c);
  }
}

void
gc_delayed(rcu_freed *e)
{
  int c =  mycpu()->id;
  struct gc_state *gs = &gc_state[c];

  scoped_acquire x(&gs->lock_);

  u64 epoch = global_epoch;

  if (gc_debug) 
    cprintf("(%d, %d): gc_delayed: %lu ndelayed %d\n", c, myproc()->pid,
            epoch, stat[c].ndelay);

  if (epoch != gs->delayed[epoch % NEPOCH].epoch) {
    cprintf("%d: epoch %lu minepoch %lu my freeto %lu my min %lu\n", c, epoch, 
            gs->delayed[epoch % NEPOCH].epoch, gs->nexttofree_epoch.load(), 
            gs->min_epoch.load());
    if (c >= ngc_cpu) return;
    panic("gc_delayed_int");
  }
  stat[c].ndelay++;
  e->_rcu_epoch = epoch;
  e->_rcu_next = gs->delayed[epoch % NEPOCH].head;
  gs->delayed[epoch % NEPOCH].head = e;
}

void
gc_begin_epoch(void)
{
  if (myproc() == nullptr) return;
  u64 v = myproc()->gc->epoch++;
  if (v & 0xff) return;

  int c =  mycpu()->id;
  struct gc_state *gs = &gc_state[c];  

  scoped_acquire x(&gs->lock_);

  myproc()->gc->core = c;
  cmpxch(&myproc()->gc->epoch, v+1, (global_epoch.load()<<8)+1);
  // We effectively need an mfence here, and cmpxch provides one
  // by virtue of being a LOCK instuction.
  gs->enqueue(myproc()->gc);
}

void
gc_end_epoch(void)
{
  if (myproc() == nullptr) return;
  u64 e = --myproc()->gc->epoch;
  if ((e & 0xff) != 0)
    return;

  int c = myproc()->gc->core;
  assert (c != -1);
  struct gc_state *gs = &gc_state[c];  

  scoped_acquire x(&gs->lock_);
  
  gc_state[c].dequeue(myproc()->gc);
  myproc()->gc->core = -1;
  if (stat[c].ndelay > gc_batchsize) {
    gs->cv.wake_all();
  }
}

void
gc_wakeup(void)
{
  for (int i = 0; i < NCPU; i++) {
    gc_state[i].cv.wake_all();
  }
}
