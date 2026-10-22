#include "types.h"
#include "aarch64.h"
#include "defs.h"
#include "date.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"

uint64
sys_exit(void)
{
  int n;
  if(argint(0, &n) < 0)
    return -1;
  exit(n);
  return 0;  // not reached
}

uint64
sys_getpid(void)
{
  return myproc()->pid;
}

uint64
sys_fork(void)
{
  return fork();
}

uint64
sys_wait(void)
{
  uint64 p;
  if(argaddr(0, &p) < 0)
    return -1;
  return wait(p);
}

uint64
sys_sbrk(void)
{
  int addr;
  int n;

  if(argint(0, &n) < 0)
    return -1;
  addr = myproc()->sz;
  if(growproc(n) < 0)
    return -1;
  return addr;
}

uint64
sys_sleep(void)
{
  int n;
  uint ticks0;

  if(argint(0, &n) < 0)
    return -1;
  acquire(&tickslock);
  ticks0 = ticks;
  while(ticks - ticks0 < n){
    if(myproc()->killed){
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}

uint64
sys_kill(void)
{
  int pid;

  if(argint(0, &pid) < 0)
    return -1;
  return kill(pid);
}

// return how many clock tick interrupts have occurred
// since start.
uint64
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}

uint64
sys_pgclearpresent(void)
{
  uint64 va;

  if (argaddr(0, &va) < 0)
    return -1;

  va = mte_untagged_addr(va);

  return uvmclearpresent(myproc()->pagetable, va);
}

// タグに対するエイリアスの仮想アドレスをマッピングしてユーザに返す
uint64
sys_vpamap(void)
{
  uint64 srcva;
  int tag;

  argaddr(0, &srcva);
  argint(1, &tag);
  
  srcva = mte_untagged_addr(srcva);

  if(tag < 0 || tag > 15)
    return -1;

  if(vpa_map_tag(myproc()->pagetable, srcva, tag) < 0)
    return -1;

  return vpa_alias_addr(srcva, tag);
}

uint64
sys_vpaunmap(void)
{
  uint64 va;
  int npages;

  if (argaddr(0, &va) < 0 || argint(1, &npages) < 0 || npages <= 0)
    return -1;

  va = mte_untagged_addr(va);
  return vpa_unmap_range(myproc()->pagetable, va, (uint64)npages);
}

// Quiet only MTE diagnostics for this process. Other faults still report.
uint64
sys_mtequiet(void)
{
  int enabled;

  if(argint(0, &enabled) < 0 || (enabled != 0 && enabled != 1))
    return -1;

  myproc()->quiet_mte_faults = enabled;
  return 0;
}
