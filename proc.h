// Per-CPU state
struct cpu {
  uchar apicid;                // Local APIC ID
  struct context *scheduler;   // swtch() here to enter scheduler
  struct taskstate ts;         // Used by x86 to find stack for interrupt
  struct segdesc gdt[NSEGS];   // x86 global descriptor table
  volatile uint started;       // Has the CPU started?
  int ncli;                    // Depth of pushcli nesting.
  int intena;                  // Were interrupts enabled before pushcli?
  struct proc *proc;           // The process running on this cpu or null
};

extern struct cpu cpus[NCPU];
extern int ncpu;

//PAGEBREAK: 17
// Saved registers for kernel context switches.
// Don't need to save all the segment registers (%cs, etc),
// because they are constant across kernel contexts.
// Don't need to save %eax, %ecx, %edx, because the
// x86 convention is that the caller has saved them.
// Contexts are stored at the bottom of the stack they
// describe; the stack pointer is the address of the context.
// The layout of the context matches the layout of the stack in swtch.S
// at the "Switch stacks" comment. Switch doesn't save eip explicitly,
// but it is on the stack and allocproc() manipulates it.
struct context {
  uint edi;
  uint esi;
  uint ebx;
  uint ebp;
  uint eip;
};

enum procstate { UNUSED, EMBRYO, SLEEPING, RUNNABLE, RUNNING, ZOMBIE };

// Per-process state
struct proc { // DONE Add PCB states for schedlog (quantum left)
  uint sz;                     // Size of process memory (bytes)
  pde_t* pgdir;                // Page table
  char *kstack;                // Bottom of kernel stack for this process
  enum procstate state;        // Process state
  int pid;                     // Process ID
  struct proc *parent;         // Parent process
  struct trapframe *tf;        // Trap frame for current syscall
  struct context *context;     // swtch() here to run process
  void *chan;                  // If non-zero, sleeping on chan
  int killed;                  // If non-zero, have been killed
  struct file *ofile[NOFILE];  // Open files
  struct inode *cwd;           // Current directory
  char name[16];               // Process name (debugging)

  // Phase 3 Modification
  int quantum_left;            // Quantum left for the process
  int starting_level;          // Original priority level for the process
};

// Process memory is laid out contiguously, low addresses first:
//   text
//   original data and bss
//   fixed-size stack
//   expandable heap

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

// Initialize the active and expired sets using this function
void InitSet(struct set * set, char * name);

void InitQueue(struct pq *Q);

// Check if process queue is empty
int IsEmptyQueue(struct pq *Q);

// Enqueue incoming process
void ENQUEUE(struct pq *Q, struct proc * x);

// Dequeue the process queue
void DEQUEUE(struct pq *Q, struct proc ** x);

// Remove outgoing process
void REMOVE(struct pq *Q, struct proc * x);

// Check for running process
int CHECK(struct pq *Q, struct proc ** x);

// Check for quantum of the current level
int QUANTUM(struct pq *Q);

int CHECKLEVEL(struct set *S, struct proc *p);