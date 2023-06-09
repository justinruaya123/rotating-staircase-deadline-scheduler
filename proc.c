#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"

struct {
  struct spinlock lock;
  struct proc proc[NPROC];
} ptable;

// Process queue struct
struct pq
{
  int front;
  int rear;
  int quantum_left;
  struct proc * proc[NPROC];
};

// Struct set for ACTIVE and EXPIRED sets
struct set
{
  char name[16];
  struct pq pq[RSDL_LEVELS];
};

// active and expired sets
struct set active;
struct set expired;

// set up temporary set for swapping
struct set temp;

//======================================================================
//
//
// START OF CUSTOM STRUCTURES AND FUNCTIONS
//
//
//======================================================================

// Process Queue Implementation
int mod(int a, int b)
{
  int r = a % b;
  return r < 0 ? r + b : r;
}

// Initialize queue
void InitQueue(struct pq *Q){
  Q->front = 0;
  Q->rear = 0;
  Q->quantum_left = RSDL_LEVEL_QUANTUM;
}

// Initialize the active and expired sets using this function
void InitSet(struct set *S, char * name){
  safestrcpy(S->name, name, sizeof(S->name));
  for(int l = 0; l < RSDL_LEVELS; l++) {
    InitQueue(&S->pq[l]);
  }
}

// Empty check subroutines (do not put locks)

// Check if queue is empty
int IsEmptyQueue(struct pq *Q)
{
  // cprintf("IE: %d\n", Q->front == Q->rear);
  return(Q->front == Q->rear);
}

// Check if queue is full
int IsFullQueue(struct pq *Q)
{
  // cprintf("IE: %d\n", Q->front == Q->rear);
  return(Q->front == mod(Q->rear + 1, NPROC));
}

// Enqueue incoming process
void ENQUEUE(struct pq *Q, struct proc * x, int quantum)
{
  if (IsFullQueue(Q)) panic("queue overflow");
  x->quantum_left = quantum;
  Q->rear = mod((Q->rear + 1), NPROC);
  Q->proc[Q->rear] = x;
  // cprintf("EN: [%d]%s at %d\n", x->pid, x->name, Q->rear);
}

// Dequeue the process queue
void DEQUEUE(struct pq *Q, struct proc ** x)
{
  if (IsEmptyQueue(Q)) panic("queue underflow");
  Q->front = mod((Q->front + 1), NPROC);
  *x = Q->proc[Q->front];
  // cprintf("DE: [%d]%s\n", (*x)->pid, (*x)->name);
}

// Returns the level of the process in the active set.
int GETLEVEL(struct proc *p){
  for(int l = 0; l < RSDL_LEVELS; l++) {
    if(IsEmptyQueue(&active.pq[l])){
      continue;
    }
    int k = mod(active.pq[l].front + 1, NPROC);
    while(k != mod(active.pq[l].rear + 1, NPROC)){
      if(active.pq[l].proc[k]->pid == p->pid) return l;
      k = mod((k + 1), NPROC); 
    }
  }
  return -1; // the process should be in the expired set?
}

// Check for the quantum of the current level
int QUANTUM(struct pq *Q)
{
  if (Q->quantum_left == 0){
    return 0;
  }
  return 1;
}

// Remove outgoing process
void REMOVE(struct pq *Q, struct proc * x)
{
  int k = mod(Q->front + 1, NPROC);
  while(k != mod(Q->rear + 1, NPROC)){
    if(Q->proc[k]->pid == x->pid) break;
    k = mod((k + 1), NPROC); 
  }
  // pid found
  while(k != mod(Q->rear + 1, NPROC)){
    Q->proc[k] = Q->proc[mod((k + 1), NPROC)];
    k = mod((k + 1), NPROC); 
  }
  Q->rear = mod((Q->rear-1), NPROC);
  // cprintf("RE: [%d]%s\n", x->pid, x->name);
}

// Check for running process.
int CHECK(struct pq *Q, struct proc ** x)
{
  int k = mod(Q->front + 1, NPROC);
  while(k != mod((Q->rear + 1), NPROC)){
    if (Q->proc[k]->state == RUNNABLE){
      *x = Q->proc[k];
      // cprintf("CH: %s\n", Q->proc[k]->name);
      return 1;
    }
    k = mod(k+1, NPROC);
  }
  return 0;
}

// Check if set is empty
int IsEmptySet(struct set *S){
  struct proc * unused; // dummy proc pointer
  for(int l = 0; l < RSDL_LEVELS; l++) {
    if(CHECK(&S->pq[l], &unused)){
      return 0; // found nonempty running process
    }
  }
  return 1;
}

// trap.c functions for process queue

// Find next available level greater than or equal to the current process
// level in the active set for enqueue. Returns the value of
// RSDL_LEVELS if not found.
int NEWLEVEL(int level){
  for(int l = level; l < RSDL_LEVELS; l++) {
    if (active.pq[l].quantum_left != 0){
      return l;
    }
  }
  return RSDL_LEVELS;
}

// Empties the current level and enqueue to the next available level
void ALTERLEVEL(void){
  acquire(&ptable.lock);
  struct proc * pp; // pointer placeholder
  int level = GETLEVEL(myproc());
  int nextlevel = NEWLEVEL(level+1); // find the next available level for the dequeued process
  REMOVE(&active.pq[level], myproc()); // remove first the active process
  if(nextlevel < RSDL_LEVELS){
    while(!IsEmptyQueue(&active.pq[level])){
      DEQUEUE(&active.pq[level], &pp);
      ENQUEUE(&active.pq[nextlevel], pp, RSDL_PROC_QUANTUM);
    }
    ENQUEUE(&active.pq[nextlevel], myproc(), RSDL_PROC_QUANTUM);
    release(&ptable.lock);
    return;
  }
  while(!IsEmptyQueue(&active.pq[level])){
    DEQUEUE(&active.pq[level], &pp);
    ENQUEUE(&expired.pq[pp->starting_level], pp, RSDL_PROC_QUANTUM);
  }
  ENQUEUE(&expired.pq[myproc()->starting_level], myproc(), RSDL_PROC_QUANTUM);
  release(&ptable.lock);
  return;
}

// Dequeues the process from the current level and enqueue to the next available level
void ALTERPROC(void){
  acquire(&ptable.lock);
  int level = GETLEVEL(myproc());
  int nextlevel = NEWLEVEL(level+1); // find the next available level for the dequeued process
  REMOVE(&active.pq[level], myproc());
  if(nextlevel < RSDL_LEVELS){
    ENQUEUE(&active.pq[nextlevel], myproc(), RSDL_PROC_QUANTUM);
    release(&ptable.lock);
    return;
  }
  ENQUEUE(&expired.pq[myproc()->starting_level], myproc(), RSDL_PROC_QUANTUM);
  release(&ptable.lock);
  return;
}

// Decrement and return the process quantum.
int DEC_PQ(void){
  return --myproc()->quantum_left; // Process-local quanta
}

// Decrement and return level quantum of a process.
// Only happens in the active set.
int DEC_LQ(void){
  return --active.pq[GETLEVEL(myproc())].quantum_left; // Level quanta
}

//======================================================================
//
//
// END OF CUSTOM STRUCTURES AND FUNCTIONS
//
//
//======================================================================

static struct proc *initproc;

int nextpid = 1;
extern void forkret(void);
extern void trapret(void);

static void wakeup1(void *chan);

void
pinit(void)
{
  initlock(&ptable.lock, "ptable");
}

// Must be called with interrupts disabled
int
cpuid() {
  return mycpu()-cpus;
}

// Must be called with interrupts disabled to avoid the caller being
// rescheduled between reading lapicid and running through the loop.
struct cpu*
mycpu(void)
{
  int apicid, i;
  
  if(readeflags()&FL_IF)
    panic("mycpu called with interrupts enabled\n");
  
  apicid = lapicid();
  // APIC IDs are not guaranteed to be contiguous. Maybe we should have
  // a reverse map, or reserve a register to store &cpus[i].
  for (i = 0; i < ncpu; ++i) {
    if (cpus[i].apicid == apicid)
      return &cpus[i];
  }
  panic("unknown apicid\n");
}

// Disable interrupts so that we are not rescheduled
// while reading proc from the cpu structure
struct proc*
myproc(void) {
  struct cpu *c;
  struct proc *p;
  pushcli();
  c = mycpu();
  p = c->proc;
  popcli();
  return p;
}

//PAGEBREAK: 32
// Look in the process table for an UNUSED proc.
// If found, change state to EMBRYO and initialize
// state required to run in the kernel.
// Otherwise return 0.
static struct proc*
allocproc(void)
{
  struct proc *p;
  char *sp;

  acquire(&ptable.lock);

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == UNUSED)
      goto found;

  release(&ptable.lock);
  return 0;

found:
  p->state = EMBRYO;
  p->pid = nextpid++;

  release(&ptable.lock);

  // Allocate kernel stack.
  if((p->kstack = kalloc()) == 0){
    p->state = UNUSED;
    return 0;
  }
  sp = p->kstack + KSTACKSIZE;

  // Leave room for trap frame.
  sp -= sizeof *p->tf;
  p->tf = (struct trapframe*)sp;

  // Set up new context to start executing at forkret,
  // which returns to trapret.
  sp -= 4;
  *(uint*)sp = (uint)trapret;

  sp -= sizeof *p->context;
  p->context = (struct context*)sp;
  memset(p->context, 0, sizeof *p->context);
  p->context->eip = (uint)forkret;

  return p;
}

//PAGEBREAK: 32
// Set up first user process.
void
userinit(void)
{
  struct proc *p;
  extern char _binary_initcode_start[], _binary_initcode_size[];

  InitSet(&active, "active");
  InitSet(&expired, "expired");
  InitSet(&temp, "temp");

  p = allocproc();
  
  initproc = p;
  if((p->pgdir = setupkvm()) == 0)
    panic("userinit: out of memory?");
  inituvm(p->pgdir, _binary_initcode_start, (int)_binary_initcode_size);
  p->sz = PGSIZE;
  memset(p->tf, 0, sizeof(*p->tf));
  p->tf->cs = (SEG_UCODE << 3) | DPL_USER;
  p->tf->ds = (SEG_UDATA << 3) | DPL_USER;
  p->tf->es = p->tf->ds;
  p->tf->ss = p->tf->ds;
  p->tf->eflags = FL_IF;
  p->tf->esp = PGSIZE;
  p->tf->eip = 0;  // beginning of initcode.S

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  // this assignment to p->state lets other cores
  // run this process. the acquire forces the above
  // writes to be visible, and the lock is also needed
  // because the assignment might not be atomic.
  acquire(&ptable.lock);

  p->state = RUNNABLE;
  p->starting_level = RSDL_STARTING_LEVEL; // TODO fix this on priofork
  ENQUEUE(&active.pq[NEWLEVEL(p->starting_level)], p, RSDL_PROC_QUANTUM); // level quanta might deplete right before enqueue

  release(&ptable.lock);
}

// Grow current process's memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint sz;
  struct proc *curproc = myproc();

  sz = curproc->sz;
  if(n > 0){
    if((sz = allocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  } else if(n < 0){
    if((sz = deallocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  }
  curproc->sz = sz;
  switchuvm(curproc);
  return 0;
}

// Create a new process copying p as the parent.
// Sets up stack to return as if from system call.
// Caller must set state of returned proc to RUNNABLE.
int
priofork(int n)
{
  int i, pid;
  struct proc *np;
  struct proc *curproc = myproc();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // Copy process state from proc.
  if((np->pgdir = copyuvm(curproc->pgdir, curproc->sz)) == 0){
    kfree(np->kstack);
    np->kstack = 0;
    np->state = UNUSED;
    return -1;
  }
  np->sz = curproc->sz;
  np->parent = curproc;
  *np->tf = *curproc->tf;

  // Clear %eax so that fork returns 0 in the child.
  np->tf->eax = 0;

  for(i = 0; i < NOFILE; i++)
    if(curproc->ofile[i])
      np->ofile[i] = filedup(curproc->ofile[i]);
  np->cwd = idup(curproc->cwd);

  safestrcpy(np->name, curproc->name, sizeof(curproc->name));

  pid = np->pid;

  acquire(&ptable.lock);

  np->state = RUNNABLE;
  np->starting_level = n;
  ENQUEUE(&active.pq[NEWLEVEL(np->starting_level)], np, RSDL_PROC_QUANTUM);

  release(&ptable.lock);

  return pid;
}

int
fork(void) {
  return priofork(RSDL_STARTING_LEVEL);
}


// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait() to find out it exited.
void
exit(void)
{
  struct proc *curproc = myproc();
  struct proc *p;
  int fd;

  if(curproc == initproc)
    panic("init exiting");

  // Close all open files.
  for(fd = 0; fd < NOFILE; fd++){
    if(curproc->ofile[fd]){
      fileclose(curproc->ofile[fd]);
      curproc->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(curproc->cwd);
  end_op();
  curproc->cwd = 0;

  acquire(&ptable.lock);

  // Parent might be sleeping in wait().
  wakeup1(curproc->parent);

  // Pass abandoned children to init.
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->parent == curproc){
      p->parent = initproc;
      if(p->state == ZOMBIE)
        wakeup1(initproc);
    }
  }

  int level = GETLEVEL(curproc);
  REMOVE(&active.pq[level], curproc);

  // Jump into the scheduler, never to return.
  curproc->state = ZOMBIE;
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(void)
{
  struct proc *p;
  int havekids, pid;
  struct proc *curproc = myproc();
  
  acquire(&ptable.lock);
  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->parent != curproc)
        continue;
      havekids = 1;
      if(p->state == ZOMBIE){
        // Found one.
        pid = p->pid;
        kfree(p->kstack);
        p->kstack = 0;
        freevm(p->pgdir);
        p->pid = 0;
        p->parent = 0;
        p->name[0] = 0;
        p->killed = 0;
        p->state = UNUSED;
        release(&ptable.lock);
        return pid;
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || curproc->killed){
      release(&ptable.lock);
      return -1;
    }

    // Wait for children to exit.  (See wakeup1 call in proc_exit.)
    sleep(curproc, &ptable.lock);  //DOC: wait-sleep
  }
}

// Schedlog variables
int schedlog_active = 0;
int schedlog_lasttick = 0;

void schedlog(int n) {
  schedlog_active = 1;
  schedlog_lasttick = ticks + n;
}

//PAGEBREAK: 42
// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run
//  - swtch to start running that process
//  - eventually that process transfers control
//      via swtch back to the scheduler.
void
scheduler(void)
{
  struct proc *p;
  struct proc *pp;
  int k;
  struct cpu *c = mycpu();
  c->proc = 0;
  int swap = 1;
  
  for(;;){
    // Enable interrupts on this processor.
    sti();

    acquire(&ptable.lock);

    for(int l = 0; l < RSDL_LEVELS; l++) { // run processes by priority

      if(!CHECK(&active.pq[l], &p)) continue; // check for available running process

      swap = 0; // disable swapping

      // Switch to chosen process.  It is the process's job
      // to release ptable.lock and then reacquire it
      // before jumping back to us.
      c->proc = p;
      switchuvm(p);
      p->state = RUNNING;

      // Syscall modification
      if (schedlog_active) { // schedlog_active
        if (ticks > schedlog_lasttick) { // ticks > schedlog_lasttick
          schedlog_active = 0;
        } else {
          // Schedlog for active set, but with levels
          for(int l = 0; l < RSDL_LEVELS; l++) {
            cprintf("%d|%s|%d(%d)", ticks, active.name, l, active.pq[l].quantum_left); // <tick>|<set>|<level>(<quantum left>) for phase 4
            
            k = mod(active.pq[l].front+1, NPROC);
            while (k != mod((active.pq[l].rear + 1), NPROC)) {
              pp = active.pq[l].proc[k];
              cprintf(",[%d]%s:%d(%d)", pp->pid, pp->name, pp->state, pp->quantum_left); // ,[<PID>]<process name>:<state number>(<quantum left>) for phase 4
              k = mod((k + 1), NPROC);
            }
            cprintf("\n");
          }
          // Schedlog for expired set, but with levels
          for(int l = 0; l < RSDL_LEVELS; l++) {
            cprintf("%d|%s|%d(%d)", ticks, expired.name, l, expired.pq[l].quantum_left); // <tick>|<set>|<level>(<quantum left>) for phase 4
 
            k = mod(expired.pq[l].front+1, NPROC);
            while (k != mod((expired.pq[l].rear + 1), NPROC)) {
              pp = expired.pq[l].proc[k];
              cprintf(",[%d]%s:%d(%d)", pp->pid, pp->name, pp->state, pp->quantum_left); // ,[<PID>]<process name>:<state number>(<quantum left>) for phase 4
              k = mod((k + 1), NPROC);
            }
            cprintf("\n");
          }
        }
      }
      // ==================================
      swtch(&(c->scheduler), p->context);
      switchkvm();
      // Process is done running for now.
      // It should have changed its p->state before coming back.
      c->proc = 0;
      break;
    }

    // Loop over process queue looking for process to run.
    if(swap){
      // cprintf("\nperform swap\n\n");

      // refresh quanta for active and expired sets
      for(int l = 0; l < RSDL_LEVELS; l++) {
        active.pq[l].quantum_left = RSDL_LEVEL_QUANTUM;
        expired.pq[l].quantum_left = RSDL_LEVEL_QUANTUM;
      }

      // empty active set first before swap, then enqueue into temp
      for(int l = 0; l < RSDL_LEVELS; l++) {
        while(!IsEmptyQueue(&active.pq[l])){
          DEQUEUE(&active.pq[l], &pp);
          ENQUEUE(&temp.pq[l], pp, RSDL_PROC_QUANTUM);
        }
      }
      // empty expired set but put it to active
      for(int l = 0; l < RSDL_LEVELS; l++) {
        while(!IsEmptyQueue(&expired.pq[l])){
          DEQUEUE(&expired.pq[l], &pp);
          ENQUEUE(&active.pq[l], pp, RSDL_PROC_QUANTUM);
        }
      }
      // if temp (old active set) is nonempty, enqueue the elements into active set based on their starting levels
      for(int l = 0; l < RSDL_LEVELS; l++) {
        while(!IsEmptyQueue(&temp.pq[l])){
          DEQUEUE(&temp.pq[l], &pp);
          ENQUEUE(&active.pq[pp->starting_level], pp, RSDL_PROC_QUANTUM);
        }
      }
    }
    swap = 1;

    release(&ptable.lock);
  }
}

// Enter scheduler.  Must hold only ptable.lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->ncli, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if(!holding(&ptable.lock))
    panic("sched ptable.lock");
  if(mycpu()->ncli != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched running");
  if(readeflags()&FL_IF)
    panic("sched interruptible");
  intena = mycpu()->intena;
  swtch(&p->context, mycpu()->scheduler);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  acquire(&ptable.lock);  //DOC: yieldlock
  myproc()->state = RUNNABLE;
  sched();
  release(&ptable.lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch here.  "Return" to user space.
void
forkret(void)
{
  static int first = 1;
  // Still holding ptable.lock from scheduler.
  release(&ptable.lock);

  if (first) {
    // Some initialization functions must be run in the context
    // of a regular process (e.g., they call sleep), and thus cannot
    // be run from main().
    first = 0;
    iinit(ROOTDEV);
    initlog(ROOTDEV);
  }

  // Return to "caller", actually trapret (see allocproc).
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.)
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();
  
  if(p == 0)
    panic("sleep");

  if(lk == 0)
    panic("sleep without lk");

  // Must acquire ptable.lock in order to
  // change p->state and then call sched.
  // Once we hold ptable.lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup runs with ptable.lock locked),
  // so it's okay to release lk.
  if(lk != &ptable.lock){  //DOC: sleeplock0
    acquire(&ptable.lock);  //DOC: sleeplock1
    release(lk);
  }

  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  // Re-enqueue the process to sleep
  int level = GETLEVEL(p);
  REMOVE(&active.pq[level], p);
  ENQUEUE(&active.pq[NEWLEVEL(level)], p, p->quantum_left);

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  if(lk != &ptable.lock){  //DOC: sleeplock2
    release(&ptable.lock);
    acquire(lk);
  }
}

//PAGEBREAK!
// Wake up all processes sleeping on chan.
// The ptable lock must be held.
static void
wakeup1(void *chan)
{
  struct proc *p;

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == SLEEPING && p->chan == chan){
      p->state = RUNNABLE;
    }
}

// Wake up all processes sleeping on chan.
void
wakeup(void *chan)
{
  acquire(&ptable.lock);
  wakeup1(chan);
  release(&ptable.lock);
}

// Kill the process with the given pid.
// Process won't exit until it returns
// to user space (see trap in trap.c).
int
kill(int pid)
{
  struct proc *p;

  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->pid == pid){
      p->killed = 1;
      // Wake process from sleep if necessary.
      if(p->state == SLEEPING){
        p->state = RUNNABLE;
      }
      release(&ptable.lock);
      return 0;
    }
  }
  release(&ptable.lock);
  return -1;
}

//PAGEBREAK: 36
// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [EMBRYO]    "embryo",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  int i;
  struct proc *p;
  char *state;
  uint pc[10];

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    cprintf("%d %s %s", p->pid, state, p->name);
    if(p->state == SLEEPING){
      getcallerpcs((uint*)p->context->ebp+2, pc);
      for(i=0; i<10 && pc[i] != 0; i++)
        cprintf(" %p", pc[i]);
    }
    cprintf("\n");
  }
}
