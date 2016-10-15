#include "types.h"
#include "defs.h"
#include "param.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"
#include <pstat.h>

#define  MAX_LEVEL_3_ticks 20

struct {
  struct spinlock lock;
  struct proc proc[NPROC];
} ptable;

struct proc *mlfq[4][NPROC];
/* struct proc mlfq1[NPROC]; */
/* struct proc mlfq2[NPROC]; */
/* struct proc mlfq3[NPROC]; */

int head[4] = {0, 0, 0, 0};
/* int head1 = 0; */
/* int head2 = 0; */
/* int head3 = 0; */
int tail3 = 0;
int ticksCount = 0 ;

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
 
  // cprintf("Inserted new pid: %d at index: %d\n",p->pid,
  insertIntoPQueue(0, p);
  release(&ptable.lock);

  // Allocate kernel stack if possible.
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

  // Init priority level and ticks count for each level
  p->prioritylevel = 0;
  p->ticks[0] = 0;
  p->ticks[1] = 0;
  p->ticks[2] = 0;
  p->ticks[3] = 0;

  p->wasRunInLastBoostCycle = 0;
  p->timeslice[0] = 0;
  p->timeslice[1] = 0;
  p->timeslice[2] = 0;
  p->timeslice[3] = 0;
  return p;
}

// Set up first user process.
void
userinit(void)
{
  struct proc *p;
  extern char _binary_initcode_start[], _binary_initcode_size[];
  
  p = allocproc();
  acquire(&ptable.lock);
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

  p->state = RUNNABLE;
  release(&ptable.lock);
}

// Grow current process's memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint sz;
  
  sz = proc->sz;
  if(n > 0){
    if((sz = allocuvm(proc->pgdir, sz, sz + n)) == 0)
      return -1;
  } else if(n < 0){
    if((sz = deallocuvm(proc->pgdir, sz, sz + n)) == 0)
      return -1;
  }
  proc->sz = sz;
  switchuvm(proc);
  return 0;
}

// Create a new process copying p as the parent.
// Sets up stack to return as if from system call.
// Caller must set state of returned proc to RUNNABLE.
int
fork(void)
{
  int i, pid;
  struct proc *np;

  // Allocate process.
  if((np = allocproc()) == 0)
    return -1;

  // Copy process state from p.
  if((np->pgdir = copyuvm(proc->pgdir, proc->sz)) == 0){
    kfree(np->kstack);
    np->kstack = 0;
    np->state = UNUSED;
    return -1;
  }
  np->sz = proc->sz;
  np->parent = proc;
  *np->tf = *proc->tf;

  // Clear %eax so that fork returns 0 in the child.
  np->tf->eax = 0;

  for(i = 0; i < NOFILE; i++)
    if(proc->ofile[i])
      np->ofile[i] = filedup(proc->ofile[i]);
  np->cwd = idup(proc->cwd);
 
  pid = np->pid;
  np->state = RUNNABLE;
  safestrcpy(np->name, proc->name, sizeof(proc->name));
  
  return pid;
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait() to find out it exited.
void
exit(void)
{
  struct proc *p;
  int fd;

  if(proc == initproc)
    panic("init exiting");

  // Close all open files.
  for(fd = 0; fd < NOFILE; fd++){
    if(proc->ofile[fd]){
      fileclose(proc->ofile[fd]);
      proc->ofile[fd] = 0;
    }
  }

  iput(proc->cwd);
  proc->cwd = 0;

  acquire(&ptable.lock);

  // Parent might be sleeping in wait().
  wakeup1(proc->parent);

  // Pass abandoned children to init.
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->parent == proc){
      p->parent = initproc;
      if(p->state == ZOMBIE)
        wakeup1(initproc);
    }
  }

  // Jump into the scheduler, never to return.
  proc->state = ZOMBIE;
  //TODO: Delete from mflq
  //mlfq[proc->prioritylevel][head[proc->prioritylevel]] = NULL;
  deleteFromQueue(proc->prioritylevel, proc);
  //cprintf("deleted pid = %d\n", proc->pid);
  // head[proc->prioritylevel] = (head[proc->prioritylevel] + 1) % NPROC;
  
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

  acquire(&ptable.lock);
  for(;;){
    // Scan through table looking for zombie children.
    havekids = 0;
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->parent != proc)
        continue;
      havekids = 1;
      if(p->state == ZOMBIE){
        // Found one.
        pid = p->pid;
        kfree(p->kstack);
        p->kstack = 0;
        freevm(p->pgdir);
        p->state = UNUSED;
        p->pid = 0;
        p->parent = 0;
        p->name[0] = 0;
        p->killed = 0;
        release(&ptable.lock);
        return pid;
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || proc->killed){
      release(&ptable.lock);
      return -1;
    }

    // Wait for children to exit.  (See wakeup1 call in proc_exit.)
    sleep(proc, &ptable.lock);  //DOC: wait-sleep
  }
}

// int currentProcIndex[4] = {0,0,0,0};
/* struct proc * getHighestPriorityProcForLevel(int plevel) { */
/*   struct proc *p; */
/*   //Get highest priority job */
/*   int i = 0; */

/*   for (; i < NPROC; i++) { */
/*     p = ptable.proc + currentProcIndex[plevel]; */
/*     if (p->prioritylevel == plevel && p->state==RUNNABLE) { */
/*       p->wasRunInLastBoostCycle = 1; */
/*       // cprintf("Returning process name: %s id: %d procindex=%d\n", p->name, p->pid, currentProcIndex[plevel]); */
/*       return p; */
/*     } */
/*     currentProcIndex[plevel] = ( currentProcIndex[plevel] + 1 ) % NPROC; */
/*     //p++; */
/*   } */
  
/*   return NULL; */
/* } */

struct proc * getHighestPriorityProcForLevel(int plevel) {
  struct proc *p;
  //Get highest priority job
  int i = 0;
  // head[plevel] = 0;
  if (plevel < 3) {
    for (; i < NPROC; i++) {
      // p = mlfq[plevel][head[plevel]];
      //p = NULL;
      p = mlfq[plevel][i];
      if (NULL != p) {
	if (p->state == RUNNABLE) {
	  p->wasRunInLastBoostCycle = 1;
	  // cprintf("Returning process name: %s id: %d procindex=%d prio:%d ticks=%d\n", p->name, p->pid, i,p->prioritylevel,p->ticks[p->prioritylevel]);
	  return p;
	}
      }
      //head[plevel] = (head[plevel] + 1) % NPROC;
    }
  }
  else {
    i = 0;
    for (; i < NPROC; i++) {
      // p = mlfq[plevel][head[plevel]];
      //p = NULL;
      p = mlfq[plevel][head[3]];
      if (NULL != p) {
	if (p->state == RUNNABLE) {
	  p->wasRunInLastBoostCycle = 1;
	  // cprintf("Returning process name: %s id: %d procindex=%d prio:%d ticks=%d\n", p->name, p->pid, i,p->prioritylevel,p->ticks[p->prioritylevel]);
	  return p;
	}
      }
      head[plevel] = (head[plevel] + 1) % NPROC;
    }
  }
  // cprintf("Returning NULL for plevel %d\n", plevel);
    //} /* else { */
    /* //RR for level 3 jobs */
    /* for (; i < NPROC; i++) { */
    /*   //p = mlfq[plevel][head[plevel]]; */
    /*   p = mlfq[plevel][i]; */
    /*   if (NULL != p) { */
    /* 	if (p->state==RUNNABLE) { */
    /* 	  /\* if (p->timeslice[3] == MAX_LEVEL_3_ticks) { *\/ */
    /* 	  /\*   //reset ticks for expired time slice. *\/ */
    /* 	  /\*   // p->ticks[3] = 0; *\/ */
    /* 	  /\*   insertIntoPQueue(3, p); *\/ */
    /* 	  /\*   deleteHeadFromPQueue(3); *\/ */
    /* 	  /\*   p->timeslice[3] = 0; *\/ */
    /* 	  /\* } else { *\/ */
    /* 	    p->wasRunInLastBoostCycle = 1; */
    /* 	    // cprintf("Returning process name: %s id: %d procindex=%d\n", p->name, p->pid, currentProcIndex[plevel]); */
    /* 	    return p; */
    /* 	    // } */
    /* 	} */
    /*   } */
    /*   head[plevel] = (head[plevel] + 1) % NPROC; */
    //}
  // }
  
  return NULL;
}

struct proc * getHighestPriorityProc() {
  // cprintf("Debug1\n");
  int plevel = 0;
  struct proc *p;
  for (; plevel < 4; plevel++) {
    //  cprintf("Debug2\n");
    p = getHighestPriorityProcForLevel(plevel);

    if (NULL != p) {
      // cprintf("Selected process from level %d\n", plevel);
      return p; 
    } 
  }

  return NULL;
}
int deleteFromQueue(int plevel, struct proc *toDelete) {
  int i = 0;
  // struct proc *p = ptable.proc;
  struct proc *p;
  
  for ( ; i < NPROC; i++ ) {
    p = mlfq[plevel][i];
    
    if ( NULL != p && p->pid == toDelete->pid ) {
      // cprintf("deleting node %s pid %d\n", p->name, p->pid);
      mlfq[plevel][i] = NULL;
      return i;
    }
    // p++;
  }
  return -1;
}

void deleteHeadFromPQueue(int plevel) {
  //TODO: Error checking...
  mlfq[plevel][head[plevel]] = NULL;
  head[plevel] = (head[plevel] + 1) % NPROC;
} 

void resetTicks(struct proc *p) {
  // p->ticks[p->prioritylevel] = 0;
  p->timeslice[0] = 0;
  p->timeslice[1] = 0;
  p->timeslice[2] = 0;
  p->timeslice[3] = 0;
  /* p->ticks[1] = 0; */
  /* p->ticks[2] = 0; */
  /* p->ticks[3] = 0; */
}

void boostPriority(void) {
  int i = 0;
  // ptable.proc + currentProcIndex[plevel]; */
  struct proc *p = ptable.proc;
  int num = 0;
  for (; i < NPROC; i++) {
    if (NULL != p && p->state == RUNNABLE && 0 == p->wasRunInLastBoostCycle) {
      if (p->prioritylevel > 0) {
        
	deleteFromQueue(p->prioritylevel, p);
	p->prioritylevel--;
	insertIntoPQueue(p->prioritylevel, p);
	// cprintf("Increasing prioritylevel for pid: %d to %d\n", p->pid, p->prioritylevel);
      }
      resetTicks(p);
      num++;
    } else {
      p->wasRunInLastBoostCycle = 0;
    }
    p++;
  }

  // cprintf("Num of p boosted: %d\n", num);
  /* currentProcIndex[0] = 0; */
  /* currentProcIndex[1] = 0; */
  /* currentProcIndex[2] = 0; */
  /* currentProcIndex[3] = 0; */
}

int insertIntoPQueue(int plevel, struct proc *p) {
  //int qIndexPtr = head[plevel];
  // int qIndexPtr = 0;
  int i = 0;
  for (; i< NPROC; i++) {
    if (mlfq[plevel][i] == NULL) {
      // cprintf("pid=%d plevel = %d qIndexPtr = %d\n", p->pid, plevel, i);
      mlfq[plevel][i] = p;
      return i;
    }
    //qIndexPtr = (qIndexPtr + 1) % NPROC;
  }
  if ( i == NPROC ) {
    cprintf("Error: 65th Active process: %s\n", p->name);
    return -1;
  }
  return i;
}

void printProcTable(uint ticksCountLocal) {
  int i = 0;
  // struct proc *p = ptable.proc;
  struct proc *p = mlfq[0][0];
  int plevel = 0;
  for (; plevel < 4; plevel++ )
    for (i=0; i< NPROC; i++) { {
	p = mlfq[plevel][i];
	if (NULL != p && p->state != UNUSED && p->pid > 2) {
	  cprintf(" %d \t %d \t %d \t ticks:%d, %d, %d, %d\n", ticksCountLocal, p->pid, p->prioritylevel, p->ticks[0], p->ticks[1], p->ticks[2], p->ticks[3]);
	}
	//p++;
      }
    }
}
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
  uint prevTicksCount = getTicksCount();
  //uint prevTicksCount = 0;
      
  for(;;){
    // Enable interrupts on this processor.
    sti();
    
    // Loop over process table looking for process to run.
    
    // for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    // if(p->state != RUNNABLE)
    //    continue;
    acquire(&ptable.lock);
      // Switch to chosen process.  It is the process's job
      // to release ptable.lock and then reacquire it
      // before jumping back to us.
    
    uint ticksCountLocal = getTicksCount();
    //uint ticksCount++;
    if (ticksCountLocal % 1 == 0 && ticksCountLocal > prevTicksCount) {
      //Boost priorities of process not scheduled in last second
      // printProcTable(ticksCountLocal);
      prevTicksCount = ticksCountLocal;
      // boostPriority();
    }
    // cprintf("Selecting process!\n");
    p = getHighestPriorityProc();
    
    if (NULL == p) {
      // cprintf("yes got null!\n");
      //printProcTable();
      release(&ptable.lock);
      continue;
    }
    //printProcTable();
    // cprintf("Selected process: %d: %s\n", p->pid, p->name);
    // cprintf("%d \t %d \t %d \t ticks:%d, %d, %d, %d\n", ticksCountLocal, p->pid, p->prioritylevel, p->ticks[0], p->ticks[1], p->ticks[2], p->ticks[3]);
    proc = p;
    switchuvm(p);
    p->state = RUNNING;
    // cprintf("debug: p.name=%s p.id=%d p.prioritylevel=%d p.tickscount=%d \n", p->name, p->pid, p->prioritylevel, p->ticks[p->prioritylevel]);

    swtch(&cpu->scheduler, proc->context);
    switchkvm();

      // Process is done running for now.
      // It should have changed its p->state before coming back.
    proc = 0;
      // }
    release(&ptable.lock);

  }
}

// Enter scheduler.  Must hold only ptable.lock
// and have changed proc->state.
void
sched(void)
{
  int intena;

  if(!holding(&ptable.lock))
    panic("sched ptable.lock");
  if(cpu->ncli != 1)
    panic("sched locks");
  if(proc->state == RUNNING)
    panic("sched running");
  if(readeflags()&FL_IF)
    panic("sched interruptible");
  intena = cpu->intena;
  swtch(&proc->context, cpu->scheduler);
  cpu->intena = intena;
}

void
handlePriorityLevel(void) {
  acquire(&ptable.lock);  //DOC: yieldlock
  //Increment the tick count for the process
  proc->ticks[proc->prioritylevel]++;
  proc->timeslice[proc->prioritylevel]++;

  ticksCount++;
  if (ticksCount == 100) {
    // cprintf("Boost cycle\n");
     boostPriority();
    ticksCount = 0;
  }
			 
  switch (proc->prioritylevel) {
  case 0:
  case 1:
    if (proc->timeslice[proc->prioritylevel] == 5) {
      // cprintf("Demoting process name: %s id: %d prio=%d ticks=%d\n", proc->name, proc->pid, proc->prioritylevel,  proc->ticks[proc->prioritylevel]);
       //deleteHeadFromPQueue(proc->prioritylevel);
       deleteFromQueue(proc->prioritylevel, proc);
       proc->prioritylevel++;
       proc->timeslice[proc->prioritylevel] = 0;
       insertIntoPQueue(proc->prioritylevel, proc);
      // proc->timeSliceComplete = 1;
    } 
    break;
  case 2:
    if (proc->timeslice[2] == 10) {
      // cprintf("Demoting process name: %s id: %d prio=%d ticks=%d\n", proc->name, proc->pid, proc->prioritylevel,  proc->ticks[proc->prioritylevel]);
      //deleteHeadFromPQueue(proc->prioritylevel);
       deleteFromQueue(proc->prioritylevel, proc);
       proc->prioritylevel++;
       proc->timeslice[proc->prioritylevel] = 0;
       insertIntoPQueue(proc->prioritylevel, proc);
      // insertIntoPQueue(proc->prioritylevel, proc);
      // proc->timeSliceComplete = 1;
    }
    break;
  default:
    //level 3
    //Cannot go below this level
    if (proc->timeslice[3] == 20) {
      //deleteHeadFromPQueue(3);
      // cprintf("Demoting process name: %s id: %d prio=%d ticks=%d\n", proc->name, proc->pid, proc->prioritylevel,  proc->ticks[proc->prioritylevel]);
      //cprintf("deleted pid %d from index %d\n", proc->pid, deleteFromQueue(3, proc));
      //cprintf("inserted pid %d at index %d\n", proc->pid, insertIntoPQueue(3, proc));
      proc->timeslice[3] = 0;
      head[3] = ( head[3] + 1 ) % NPROC;
     }
    break;
  }
  release(&ptable.lock);
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  acquire(&ptable.lock);  //DOC: yieldlock
  // handlePriorityLevel();
  proc->state = RUNNABLE;
  
  //  cprintf("done!\n");
  sched();
  release(&ptable.lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch here.  "Return" to user space.
void
forkret(void)
{
  // Still holding ptable.lock from scheduler.
  release(&ptable.lock);
  
  // Return to "caller", actually trapret (see allocproc).
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  if(proc == 0)
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
  proc->chan = chan;
  proc->state = SLEEPING;
  sched();

  // Tidy up.
  proc->chan = 0;

  // Reacquire original lock.
  if(lk != &ptable.lock){  //DOC: sleeplock2
    release(&ptable.lock);
    acquire(lk);
  }
}

// Wake up all processes sleeping on chan.
// The ptable lock must be held.
static void
wakeup1(void *chan)
{
  struct proc *p;

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == SLEEPING && p->chan == chan)
      p->state = RUNNABLE;
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
      if(p->state == SLEEPING)
        p->state = RUNNABLE;
      release(&ptable.lock);
      return 0;
    }
  }
  release(&ptable.lock);
  return -1;
}

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


int
getprocessinfo(struct pstat *pstats) {
  acquire(&ptable.lock);
  struct proc *p;
  int i = 0;
  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
    // cprintf("%d: first pid = %d\n",i, p->pid);
    // Copy pid
    pstats->pid[i] = p->pid;
    // Copy inuse bit
    if (p->state == UNUSED) {
      pstats->inuse[i] = 0;
    } else {
      pstats->inuse[i] = 1;
    }
    // Copy priority
    pstats->priority[i] = p->prioritylevel;
    // Copy state
    pstats->state[i] = p->state;
    // Copy ticks
    pstats->ticks[i][0] = p->ticks[0];
    pstats->ticks[i][1] = p->ticks[1];
    pstats->ticks[i][2] = p->ticks[2];
    pstats->ticks[i][3] = p->ticks[3];
    i++;
  }
  release(&ptable.lock);
  return 0;
}
