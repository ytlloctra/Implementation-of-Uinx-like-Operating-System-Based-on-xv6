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

//PAGEBREAK: 32
// Look in the process table for an UNUSED proc.
// If found, change state to EMBRYO and initialize
// state required to run in the kernel.
// Otherwise return 0.
// Must hold ptable.lock.
/**
 * 函数功能：分配一个进程控制块，并对其进行初始化操作
 * @return {proc*} 返回分配的proc的指针，分配失败则返回0
 */
static struct proc*
allocproc(void)
{
  struct proc *p;
  char *sp;
  int i;

  // 遍历进程表中的每个进程控制块，如果找到一个未被分配的则跳转到found
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == UNUSED)
      goto found;
  return 0;

found:
  // 进程状态设置为婴儿状态
  p->state = EMBRYO;
  // 分配pid
  p->pid = nextpid++;

  //vm变量初始化
  for(i = 0; i < 10; i++){
	  p->vm[i].next = -1;
	  p->vm[i].length = 0;
  }
    p->vm[0].next = 0;

  p->mqmask = 0;  //初始化mqmask

  p->slot = SLOT;
  p->priority = 10;
  p->shm = KERNBASE;  			//初始化shm，刚开始shm与KERNBASE重合
  p->shmkeymask = 0;  			// 初始化shmkeymask

  // Allocate kernel stack.
  // 分配内核栈，这里即调用kalloc方法分配一个页的空间（4kB）
  if((p->kstack = kalloc()) == 0){
    p->state = UNUSED;
    return 0;
  }
  // 栈指针放到栈顶
  sp = p->kstack + KSTACKSIZE;

  // Leave room for trap frame.
  // 同上面的注释，为陷阱帧保留空间，即在栈上分配空间给陷阱帧
  sp -= sizeof *p->tf;
  p->tf = (struct trapframe*)sp;

  // Set up new context to start executing at forkret,
  // which returns to trapret.
  sp -= 4;
  *(uint*)sp = (uint)trapret;

  // 同理分配栈空间给context
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

  acquire(&ptable.lock);

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



int 
mygrowproc(int n){                 // 实现首次最佳适应算法
	struct vma *vm = proc->vm;     // 遍历寻找合适的空间
	int start = proc->sz;          // 寻找合适的分配起点
	int index;
	int prev = 0;
	int i;

	for(index = vm[0].next; index != 0; index = vm[index].next){
		if(start + n < vm[index].address)
			break;
		start = vm[index].address + vm[index].length;
		prev = index;
	}
	
	for(i = 1; i < 10; i++) {            // 寻找一块没有用的 vma 记录新的内存块
		if(vm[i].next == -1){
			vm[i].next = index;			
			vm[i].address = start;
			vm[i].length = n;

			vm[prev].next = i;
			
			myallocuvm(proc->pgdir, start, start + n);
			switchuvm(proc);
			return start;   // 返回分配的地址
		}
	}
	switchuvm(proc);
	return 0;
}


int
myreduceproc(int address){  // 释放 address 开头的内存块
	int prev = 0;
	int index;
	for(index = proc->vm[0].next; index != 0; index = proc->vm[index].next) {
		if(proc->vm[index].address == address && proc->vm[index].length > 0) {
			mydeallocuvm(proc->pgdir, proc->vm[index].address, proc->vm[index].address + proc->vm[index].length);			
			proc->vm[prev].next = proc->vm[index].next;
			proc->vm[index].next = -1;
			proc->vm[index].length = 0;
			break;
		}
		prev = index;
	}
	switchuvm(proc);
	return 0;
}

// Create a new process copying p as the parent.
// Sets up stack to return as if from system call.
// Caller must set state of returned proc to RUNNABLE.
/**
 * 根据父进程的proc创建子进程的进程控制块
 * @return {int} -1 操作失败 : else 子进程 pid
 */
int
fork(void)
{
  int i, pid;
  struct proc *np;

  // 对进程表结构体（内部包含进程数组）进行加锁
  acquire(&ptable.lock);

  // Allocate process.
  // 分配进程控制块PCB
  if((np = allocproc()) == 0){
    // 分配失败则返回，一般当进程数超过64时会失败
    release(&ptable.lock);
    return -1;
  }

  // Copy process state from p.
  // 同原注释，复制主进程的proc属性到子进程的proc（下面的np）中
  // 首先是调用copyuvm函数复制页表
  if((np->pgdir = copyuvm(proc->pgdir, proc->sz)) == 0){
    kfree(np->kstack);
    np->kstack = 0;
    np->state = UNUSED;
    release(&ptable.lock);
    return -1;
  }

  shmaddcount(proc->shmkeymask);  		//fork出新进程，所以引用数加1
  np->shm = proc->shm;             		//复制父进程的shm
  np->shmkeymask = proc->shmkeymask; 		//复制父进程的shmkeymask
  for(i=0;i<8;++i){          		        //复制父进程的shmva数组
    if(shmkeyused(i,np->shmkeymask)){		//只拷贝已启用的共享内存区
      np->shmva[i] = proc->shmva[i];
    }
  }

  np->sz = proc->sz;
  // 设置子进程proc的parent为父进程的proc
  np->parent = proc;
  *np->tf = *proc->tf;

  // Clear %eax so that fork returns 0 in the child.
  np->tf->eax = 0;

  // 继续复制剩余内容
  for(i = 0; i < NOFILE; i++)
    if(proc->ofile[i])
      np->ofile[i] = filedup(proc->ofile[i]);
  np->cwd = idup(proc->cwd);

  addmqcount(proc->mqmask);    //父进程持有的消息队列引用数全部加1
  np->mqmask = proc->mqmask;   //复制父进程的mqmask

  safestrcpy(np->name, proc->name, sizeof(proc->name));

  // 父进程调用proc生成子进程，需要返回子进程的pid
  pid = np->pid;

  // 子进程state由EMBRYO修改为RUNNABLE，进而子进程会在ptable中被调度器调度执行
  np->state = RUNNABLE;

  //释放进程表锁，保证整个创建过程的原子性
  release(&ptable.lock);

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

  begin_op();
  iput(proc->cwd);
  end_op();
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

        releasemq2(p->mqmask);   //回收消息队列
        p->mqmask = 0;          //重置进程的mqmask
        
        shmrelease(p->pgdir, p->shm, p->shmkeymask);  	// 解除共享内存映射
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
    if(!havekids || proc->killed){
      release(&ptable.lock);
      return -1;
    }

    // Wait for children to exit.  (See wakeup1 call in proc_exit.)
    sleep(proc, &ptable.lock);  //DOC: wait-sleep
  }
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
  struct proc *p,*temp;
  int priority;


  for(;;){
    // Enable interrupts on this processor.
    sti();

    // Loop over process table looking for process to run.
    acquire(&ptable.lock);

    priority = 19;

     for(temp  = ptable.proc; temp < &ptable.proc[NPROC]; temp++) //获取当前可运行的最高当前优先级
    {
      if(temp->state == RUNNABLE&&temp->priority < priority)
        priority = temp->priority;
    }


    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    {
      if(p->state != RUNNABLE)
        continue;      
      if(p->priority > priority)
        continue;
      else
      {
         priority = p->priority ;
      }


        // Switch to chosen process.  It is the process's job
        // to release ptable.lock and then reacquire it
        // before jumping back to us.
        proc = p;
        switchuvm(p);
        p->state = RUNNING;
        swtch(&cpu->scheduler, p->context);
        switchkvm();

        // Process is done running for now.
        // It should have changed its p->state before coming back.
        proc = 0;
      }//endif for proc_num

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

// Give up the CPU for one scheduling round.
void
yield(void)
{
  acquire(&ptable.lock);  //DOC: yieldlock
  proc->state = RUNNABLE;
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

//PAGEBREAK!
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

void wakeup1p(void *chan) {
  acquire(&ptable.lock);
  struct proc *p;
  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
    if (p->state == SLEEPING && p->chan == chan) {
      p->state = RUNNABLE;
      break;
    }
  }
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
  [ZOMBIE]    "zombie"   //僵尸
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
//    cprintf("ticks: %d, pid: %d, state: %s, prio: %d, name: %s", p->slot, p->pid, state, p->priority, p->name);
    cprintf("\n pid : %d, state : %s, name : %s\n", p->pid, state, p->name);
    for(int i = p->vm[0].next; i != 0; i = p->vm[i].next) {
      cprintf("start: %d, length: %d\n", p->vm[i].address, p->vm[i].length);
    }

    if(p->state == SLEEPING){
      getcallerpcs((uint*)p->context->ebp+2, pc);
      for(i=0; i<10 && pc[i] != 0; i++)
        cprintf(" %p", pc[i]);
    }
    cprintf("\n");
  }
}

int getcpuid() {
  return cpunum();
}

int chpri(int pid, int priority) {
  struct proc *p;
  acquire(&ptable.lock);

  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
    if (p->pid == pid) {
      p->priority = priority;
      break;
    }
  }

  release(&ptable.lock);
  return pid;
}


//--------------------------------------------------------------------------------------
//--------------------------------------------------------------------------------------
//--------------------------------------------------------------------------------------

int clone(void (*fcn)(void *), void *arg, void *stack) {
  struct proc *curproc = proc;
  struct proc *np;
  if ((np = allocproc()) == 0)
    return -1;

  np->pgdir = curproc->pgdir;
  np->sz = curproc->sz;
  np->pthread = curproc;
  np->ustack = stack;
  np->parent = 0;
  *np->tf = *curproc->tf;
  int *sp = stack + 4096 - 8;
  np->tf->eip = (int)fcn;
  np->tf->esp = (int)sp;
  np->tf->ebp = (int)sp;
  np->tf->eax = 0;

  *(sp + 1) = (int)arg;
  *sp = 0xffffffff;
  for (int i = 0; i < NOFILE; i++)
    if (curproc->ofile[i])
      np->ofile[i] = filedup(curproc->ofile[i]);
  np->cwd = idup(curproc->cwd);

  safestrcpy(np->name, curproc->name, sizeof(curproc->name));
  int pid = np->pid;

  acquire(&ptable.lock);
  np->state = RUNNABLE;
  release(&ptable.lock);

  return pid;
}

int join(void **stack) {
  struct proc *curproc = proc;
  struct proc *p;
  int havekids;
  acquire(&ptable.lock);
  for (;;) {
    havekids = 0;
    for (p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
      if (p->pthread != curproc)
        continue;

      havekids = 1;
      if (p->state == ZOMBIE) {
        *stack = p->ustack;
        int pid = p->pid;
        kfree(p->kstack);
        p->kstack = 0;
        p->state = UNUSED;
        p->pid = 0;
        p->parent = 0;
        p->pthread = 0;
        p->name[0] = 0;
        p->killed = 0;
        release(&ptable.lock);
        return pid;
      }
    }
    if (!havekids || curproc->killed) {
      release(&ptable.lock);
      return -1;
    }
    sleep(curproc, &ptable.lock);
  }
  return 0;
}

