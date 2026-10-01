#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

//MLFQ global variables
uint priority_quanta[]={2,4,8,16,24};
uint last_boost_tick;
extern uint ticks;
#define PRIORITY_BOOST_INTERVAL 70
#define QUEUE_NUM 5
struct proc *mlfq_heads[QUEUE_NUM];
struct proc *mlfq_tails[QUEUE_NUM];
struct spinlock mlfq_lock;

struct cpu cpus[NCPU];

struct proc proc[NPROC];

struct proc *initproc;

int nextpid = 1;
struct spinlock pid_lock;

extern void forkret(void);
static void freeproc(struct proc *p);

extern char trampoline[]; // trampoline.S

// helps ensure that wakeups of wait()ing
// parents are not lost. helps obey the
// memory model when using p->parent.
// must be acquired before any p->lock.
struct spinlock wait_lock;

// Allocate a page for each process's kernel stack.
// Map it high in memory, followed by an invalid
// guard page.
void
proc_mapstacks(pagetable_t kpgtbl)
{
  struct proc *p;
  
  for(p = proc; p < &proc[NPROC]; p++) {
    char *pa = kalloc();
    if(pa == 0)
      panic("kalloc");
    uint64 va = KSTACK((int) (p - proc));
    kvmmap(kpgtbl, va, (uint64)pa, PGSIZE, PTE_R | PTE_W);
  }
}

// initialize the proc table.
void
procinit(void)
{
  struct proc *p;
  
  initlock(&pid_lock, "nextpid");
  initlock(&wait_lock, "wait_lock");
  // queue lock must always be obtained before the process lock and only released
  // if the process lock has also been previously released to avoid deadlock issues
  initlock(&mlfq_lock, "mlfq_lock");
  for(p = proc; p < &proc[NPROC]; p++) {
      initlock(&p->lock, "proc");
      p->state = UNUSED;
      p->kstack = KSTACK((int) (p - proc));
  }
}

// Must be called with interrupts disabled,
// to prevent race with process being moved
// to a different CPU.
int
cpuid()
{
  int id = r_tp();
  return id;
}

// Return this CPU's cpu struct.
// Interrupts must be disabled.
struct cpu*
mycpu(void)
{
  int id = cpuid();
  struct cpu *c = &cpus[id];
  return c;
}

// Return the current struct proc *, or zero if none.
struct proc*
myproc(void)
{
  push_off();
  struct cpu *c = mycpu();
  struct proc *p = c->proc;
  pop_off();
  return p;
}

int
allocpid()
{
  int pid;
  
  acquire(&pid_lock);
  pid = nextpid;
  nextpid = nextpid + 1;
  release(&pid_lock);

  return pid;
}

// Look in the process table for an UNUSED proc.
// If found, initialize state required to run in the kernel,
// and return with p->lock held.
// If there are no free procs, or a memory allocation fails, return 0.
static struct proc*
allocproc(void)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if(p->state == UNUSED) {
      goto found;
    } else {
      release(&p->lock);
    }
  }
  return 0;

found:
  p->pid = allocpid();
  p->state = USED;
  
  //MLFQ fields
  p->priority = 0;           // New processes are added to the highest priority queue (Q0) 
  p->next = 0;               //pointer set to 0 to prevent garbage
  p->ticks_consumed = 0;    // Ticks consumed at current priority level
  p->total_ticks = 0;       //Ticks consumed in a lifetime of the process
  p->wait_ticks = 0;        // Ticks the process has been waiting in a queue

  // Allocate a trapframe page.
  if((p->trapframe = (struct trapframe *)kalloc()) == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // An empty user page table.
  p->pagetable = proc_pagetable(p);
  if(p->pagetable == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // Set up new context to start executing at forkret,
  // which returns to user space.
  memset(&p->context, 0, sizeof(p->context));
  p->context.ra = (uint64)forkret;
  p->context.sp = p->kstack + PGSIZE;

  return p;
}

// free a proc structure and the data hanging from it,
// including user pages.
// p->lock must be held.
static void
freeproc(struct proc *p)
{
  if(p->trapframe)
    kfree((void*)p->trapframe);
  p->trapframe = 0;
  if(p->pagetable)
    proc_freepagetable(p->pagetable, p->sz);
  p->pagetable = 0;
  p->sz = 0;
  p->pid = 0;
  p->parent = 0;
  p->name[0] = 0;
  p->chan = 0;
  p->killed = 0;
  p->xstate = 0;
  p->state = UNUSED;
  p->next = 0;
  p->priority = 0;
}

// Create a user page table for a given process, with no user memory,
// but with trampoline and trapframe pages.
pagetable_t
proc_pagetable(struct proc *p)
{
  pagetable_t pagetable;

  // An empty page table.
  pagetable = uvmcreate();
  if(pagetable == 0)
    return 0;

  // map the trampoline code (for system call return)
  // at the highest user virtual address.
  // only the supervisor uses it, on the way
  // to/from user space, so not PTE_U.
  if(mappages(pagetable, TRAMPOLINE, PGSIZE,
              (uint64)trampoline, PTE_R | PTE_X) < 0){
    uvmfree(pagetable, 0);
    return 0;
  }

  // map the trapframe page just below the trampoline page, for
  // trampoline.S.
  if(mappages(pagetable, TRAPFRAME, PGSIZE,
              (uint64)(p->trapframe), PTE_R | PTE_W) < 0){
    uvmunmap(pagetable, TRAMPOLINE, 1, 0);
    uvmfree(pagetable, 0);
    return 0;
  }

  return pagetable;
}

// Free a process's page table, and free the
// physical memory it refers to.
void
proc_freepagetable(pagetable_t pagetable, uint64 sz)
{
  uvmunmap(pagetable, TRAMPOLINE, 1, 0);
  uvmunmap(pagetable, TRAPFRAME, 1, 0);
  uvmfree(pagetable, sz);
}

// Set up first user process.
void
userinit(void)
{
 struct proc *p;
 p = allocproc();
 initproc = p;

 p->cwd = namei("/");
 p->state = RUNNABLE;
 p->priority = 0;
 p->ticks_consumed = 0;
 enqueue(p,0);
 release(&p->lock);
}

// Grow or shrink user memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint64 sz;
  struct proc *p = myproc();

  sz = p->sz;
  if(n > 0){
    if(sz + n > TRAPFRAME) {
      return -1;
    }
    if((sz = uvmalloc(p->pagetable, sz, sz + n, PTE_W)) == 0) {
      return -1;
    }
  } else if(n < 0){
    sz = uvmdealloc(p->pagetable, sz, sz + n);
  }
  p->sz = sz;
  return 0;
}

// Create a new process, copying the parent.
// Sets up child kernel stack to return as if from fork() system call.
int
kfork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *p = myproc();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // Copy user memory from parent to child.
  if(uvmcopy(p->pagetable, np->pagetable, p->sz) < 0){
    freeproc(np);
    release(&np->lock);
    return -1;
  }
  np->sz = p->sz;

  // copy saved user registers.
  *(np->trapframe) = *(p->trapframe);

  // Cause fork to return 0 in the child.
  np->trapframe->a0 = 0;

  // increment reference counts on open file descriptors.
  for(i = 0; i < NOFILE; i++)
    if(p->ofile[i])
      np->ofile[i] = filedup(p->ofile[i]);
  np->cwd = idup(p->cwd);

  safestrcpy(np->name, p->name, sizeof(p->name));

  pid = np->pid;

  acquire(&wait_lock);
  np->parent = p;
  release(&wait_lock);

  // set state to RUNNABLE while holding np->lock
  np->state = RUNNABLE;
  np->priority = 0;
  np->ticks_consumed = 0;

  // enqueue into Queue 0
  enqueue(np, 0);

  // release np->lock AFTER setting RUNNABLE & enqueuing
  release(&np->lock);

  return pid;
}

// Pass p's abandoned children to init.
// Caller must hold wait_lock.
void
reparent(struct proc *p)
{
  struct proc *pp;

  for(pp = proc; pp < &proc[NPROC]; pp++){
    if(pp->parent == p){
      pp->parent = initproc;
      wakeup(initproc);
    }
  }
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait().
void
kexit(int status)
{
  struct proc *p = myproc();

  if(p == initproc)
    panic("init exiting");

  // Close all open files.
  for(int fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd]){
      struct file *f = p->ofile[fd];
      fileclose(f);
      p->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(p->cwd);
  end_op();
  p->cwd = 0;

  acquire(&wait_lock);

  // Give any children to init.
  reparent(p);

  // Parent might be sleeping in wait().
  wakeup(p->parent);
  
  acquire(&p->lock);

  p->xstate = status;
  p->state = ZOMBIE;

  release(&wait_lock);

  // Jump into the scheduler, never to return.
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
kwait(uint64 addr)
{
  struct proc *pp;
  int havekids, pid;
  struct proc *p = myproc();

  acquire(&wait_lock);

  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(pp = proc; pp < &proc[NPROC]; pp++){
      if(pp->parent == p){
        // make sure the child isn't still in exit() or swtch().
        acquire(&pp->lock);

        havekids = 1;
        if(pp->state == ZOMBIE){
          // Found one.
          pid = pp->pid;
          if(addr != 0 && copyout(p->pagetable, addr, (char *)&pp->xstate,
                                  sizeof(pp->xstate)) < 0) {
            release(&pp->lock);
            release(&wait_lock);
            return -1;
          }
          freeproc(pp);
          release(&pp->lock);
          release(&wait_lock);
          return pid;
        }
        release(&pp->lock);
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || killed(p)){
      release(&wait_lock);
      return -1;
    }
    
    // Wait for a child to exit.
    sleep(p, &wait_lock);  //DOC: wait-sleep
  }
}

// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run.
//  - swtch to start running that process.
//  - eventually that process transfers control
//    via swtch back to the scheduler.

void
scheduler(void){
  struct proc *p;
  struct cpu *c = mycpu();

  c->proc = 0;
  for(;;){
    intr_on();

    acquire(&tickslock);
    if(ticks - last_boost_tick > PRIORITY_BOOST_INTERVAL){
      last_boost_tick = ticks;
      release(&tickslock);
      priority_boost();
    }else{
      release(&tickslock);
    }

    acquire(&mlfq_lock);
    p=0;
    
    for(int prio = 0; prio < QUEUE_NUM; prio++){
      if(mlfq_heads[prio] != 0){
        
        p = mlfq_heads[prio];
        mlfq_heads[prio] = p->next;
        if(mlfq_heads[prio] == 0)
          mlfq_tails[prio] = 0;

        p->next = 0;
        break;
      }
    }

    if( p != 0){
      release(&mlfq_lock);
      acquire(&p->lock);

      if(p->state == RUNNABLE){
        p->state = RUNNING;
        c->proc = p;

        swtch(&c->context, &p->context);

        c->proc = 0;
      }
      release(&p->lock);
    }else{
      release(&mlfq_lock);
    }
  }
}

//initializes last_boost_tick to current system time to always yield correct interval
//even after the system has been running for some time
void
boostinit(void){
  acquire(&tickslock);
  last_boost_tick = ticks;
  release(&tickslock);
}

void
update_wait_ticks(void){
  struct proc *p;
  for(p = proc; p < &proc[NPROC];p++){
    if(p->state == RUNNABLE){
      acquire(&p->lock);
      if(p->state == RUNNABLE){
        p->wait_ticks++;
      }
      release(&p->lock);
    }
  }
}

void enqueue(struct proc *p, int prio){
  if(p == 0) panic("enqueue: null proc");
  if(prio < 0 || prio >= QUEUE_NUM) panic("enqueu: invalid prio");
  
  acquire(&mlfq_lock);

  p->next = 0;
  p->priority = prio;
  
  if(mlfq_heads[prio] == 0){
    mlfq_heads[prio] = p;
    mlfq_tails[prio] = p;
  }else{
    mlfq_tails[prio]->next = p;
    mlfq_tails[prio] = p;
  }
  release(&mlfq_lock);
}

void dequeue(struct proc *p){
  if(p == 0) panic("dequeue: null proc");

  acquire(&mlfq_lock);

  int prio = p->priority;
  if(prio < 0 || prio >=  QUEUE_NUM) panic("dequeu: invalid prio");

  struct proc * prev = 0;
  struct proc *cur = mlfq_heads[prio];

  while(cur){
    if(cur == p){
      if(prev)
        prev->next = cur->next;
      else
        mlfq_heads[prio] = cur->next;

      if(mlfq_tails[prio] == cur) 
        mlfq_tails[prio] = prev;


      cur->next = 0;
      release(&mlfq_lock);
      return;
    }
    prev = cur;
    cur = cur->next;
  }
  panic("dequeu: proc not in queue");
}

void
priority_boost(void)
{
  acquire(&mlfq_lock);
  // clear queue heads and tails
  for(int i = 0; i < QUEUE_NUM; i++){
    mlfq_heads[i] = 0;
    mlfq_tails[i] = 0;
  }
  release(&mlfq_lock);

  // relink RUNNABLE processes adhering to lock order: p->lock -> mlfq_lock
  struct proc *p;
  for (p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);
    if(p->state == RUNNABLE){
      p->priority = 0;
      p->ticks_consumed = 0;
      enqueue(p, 0); // enqueue internally acquires mlfq_lock
    } else if(p->state == RUNNING){
      p->priority = 0;
      p->ticks_consumed = 0;
    }
    release(&p->lock);
  }
}

int
has_higher_priority(int current_prio)
{
  struct proc *p = myproc();
  int should_preempt = 0;

  acquire(&mlfq_lock);
  for(int i = 0; i < current_prio; i++){
    if(mlfq_heads[i] != 0){
      should_preempt = 1;
      break;
    }
  }
  release(&mlfq_lock);

  if(should_preempt && p != 0) 
    printf("Preempting PID %d (prio %d)\n", p->pid, p->priority);
  return should_preempt;
}
//helper to handle timer ticks centrally for both usertrap and kerneltrap
void
mlfq_timer_tick(void)
{
  if(cpuid() == 0) 
    update_wait_ticks();

  struct proc *p = myproc();
  if(p != 0 && p->state == RUNNING){
    p->ticks_consumed++;
    p->total_ticks++;

    int quantum_expired = (p->ticks_consumed >= priority_quanta[p->priority]);
    int preempt_needed = has_higher_priority(p->priority);

    if(quantum_expired || preempt_needed){
      if(quantum_expired){
        if(p->priority < QUEUE_NUM - 1) 
          p->priority++; // demote to lower priority level
        p->ticks_consumed = 0; //reset slice only upon quantum expiry
      }
      yield();
    }
  }
}
// Switch to scheduler.  Must hold only p->lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->noff, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if(!holding(&p->lock))
    panic("sched p->lock");
  if(mycpu()->noff != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched RUNNING");
  if(intr_get())
    panic("sched interruptible");

  intena = mycpu()->intena;
  swtch(&p->context, &mycpu()->context);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  struct proc *p = myproc();
  
  acquire(&p->lock);
  p->state = RUNNABLE;

  // enqueue safely under p->lock
  enqueue(p, p->priority);

  // p->lock is handed off to sched()
  sched();

  release(&p->lock);
}
// A fork child's very first scheduling by scheduler()
// will swtch to forkret.
void
forkret(void)
{
  extern char userret[];
  static int first = 1;
  struct proc *p = myproc();

  // Still holding p->lock from scheduler.
  release(&p->lock);

  if (first) {
    // File system initialization must be run in the context of a
    // regular process (e.g., because it calls sleep), and thus cannot
    // be run from main().
    fsinit(ROOTDEV);

    first = 0;
    // ensure other cores see first=0.
    __sync_synchronize();

    // We can invoke kexec() now that file system is initialized.
    // Put the return value (argc) of kexec into a0.
    p->trapframe->a0 = kexec("/init", (char *[]){ "/init", 0 });
    if (p->trapframe->a0 == -1) {
      panic("exec");
    }
  }

  // return to user space, mimicing usertrap()'s return.
  prepare_return();
  uint64 satp = MAKE_SATP(p->pagetable);
  uint64 trampoline_userret = TRAMPOLINE + (userret - trampoline);
  ((void (*)(uint64))trampoline_userret)(satp);
}

// Sleep on channel chan, releasing condition lock lk.
// Re-acquires lk when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();
  
  // Must acquire p->lock in order to
  // change p->state and then call sched.
  // Once we hold p->lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup locks p->lock),
  // so it's okay to release lk.

  acquire(&p->lock);  //DOC: sleeplock1
  release(lk);

  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  release(&p->lock);
  acquire(lk);
}

// Wake up all processes sleeping on channel chan.
// Caller should hold the condition lock.
void
wakeup(void *chan)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++) {
    if(p == myproc())
      continue;

    acquire(&p->lock);
    if(p->state == SLEEPING && p->chan == chan) {
      p->state = RUNNABLE;
      p->chan = 0;
      p->priority = 0;
      p->ticks_consumed = 0;
      p->wait_ticks = 0;

      acquire(&mlfq_lock);
      
      p->next = 0;
      if(mlfq_heads[0] == 0) {
        mlfq_heads[0] = p;
        mlfq_tails[0] = p;
      } else {
        mlfq_tails[0]->next = p;
        mlfq_tails[0] = p;
      }

      release(&mlfq_lock);
      release(&p->lock);
    } else {
      release(&p->lock);
    }
  }
}
// Kill the process with the given pid.
// The victim won't exit until it tries to return
// to user space (see usertrap() in trap.c).
int
kkill(int pid)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);
    if(p->pid == pid){
      p->killed = 1;
      if(p->state == SLEEPING){
        // Wake process from sleep().
        p->state = RUNNABLE;
      }
      release(&p->lock);
      return 0;
    }
    release(&p->lock);
  }
  return -1;
}

void
setkilled(struct proc *p)
{
  acquire(&p->lock);
  p->killed = 1;
  release(&p->lock);
}

int
killed(struct proc *p)
{
  int k;
  
  acquire(&p->lock);
  k = p->killed;
  release(&p->lock);
  return k;
}

// Copy to either a user address, or kernel address,
// depending on usr_dst.
// Returns 0 on success, -1 on error.
int
either_copyout(int user_dst, uint64 dst, void *src, uint64 len)
{
  struct proc *p = myproc();
  if(user_dst){
    return copyout(p->pagetable, dst, src, len);
  } else {
    memmove((char *)dst, src, len);
    return 0;
  }
}

// Copy from either a user address, or kernel address,
// depending on usr_src.
// Returns 0 on success, -1 on error.
int
either_copyin(void *dst, int user_src, uint64 src, uint64 len)
{
  struct proc *p = myproc();
  if(user_src){
    return copyin(p->pagetable, dst, src, len);
  } else {
    memmove(dst, (char*)src, len);
    return 0;
  }
}

// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
//Changed from ^P to ^T due to IDE shortcut conflicts
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [USED]      "used",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  struct proc *p;
  char *state;

  printf("\nPID\tSTATE\tNAME\tPRIO\tTICKS\tTOTAL\tWAIT\n");
  for(p = proc; p < &proc[NPROC]; p++){
    if(p->state == UNUSED){
      continue;
    }
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    printf("%d\t%s\t%s\t%d\t%d\t%d\t%d\n", p->pid, state, p->name, p->priority, p->ticks_consumed, p->total_ticks, p->wait_ticks);
  }
  printf("$ ");
}
