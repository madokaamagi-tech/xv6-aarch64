#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "aarch64.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "syscall.h"

#define ESR_EC(esr)        (((esr) >> 26) & 0x3f)
#define ESR_ISS(esr)       ((esr) & 0x1ffffff)
#define ESR_DFSC(iss)      ((iss) & 0x3f)

#define EC_SVC64           0x15
#define EC_DATA_ABORT_EL0  0x24
#define DFSC_MTE_TAG_CHECK 0x11

static char *
data_abort_kind(uint64 iss)
{
  uint64 dfsc = ESR_DFSC(iss);

  if(dfsc == DFSC_MTE_TAG_CHECK)
    return "MTE tag check fault";

  if(dfsc == 0x04 || dfsc == 0x05 || dfsc == 0x06 || dfsc == 0x07)
    return "translation fault / non-present VPA page";

  if(dfsc == 0x09 || dfsc == 0x0a || dfsc == 0x0b)
    return "access flag fault";

  if(dfsc == 0x0d || dfsc == 0x0e || dfsc == 0x0f)
    return "permission fault";

  return "unknown data abort";
}

struct spinlock tickslock;
uint ticks;

// in trapvec.S, calls kerneltrap() or usertrap().
void alltraps();

extern int devintr();

void
trapinit(void)
{
  initlock(&tickslock, "time");
}

// set up to take exceptions and traps.
void
trapinithart(void)
{
  w_vbar_el1((uint64)alltraps);
}

//
// handle an interrupt, exception, or system call from user space.
// called from uservec.S
//

void
userirq(void)
{
  struct proc *p = myproc();

  int which_dev = devintr();
  // give up the CPU if this is a timer interrupt.
  if(which_dev == 2)
    yield();

  if(p->killed)
    exit(-1);

  intr_off();
}

void
usertrap(void)
{
  struct proc *p = myproc();

  // ESR_EL1, ELR_EL1, FAR_EL1 の値を読み出してそれぞれ変数に格納
  uint64 esr = r_esr_el1();
  uint64 elr = r_elr_el1();
  uint64 far = r_far_el1();
  
  uint64 ec = ESR_EC(esr);
  uint64 iss = ESR_ISS(esr);
  uint64 dfsc = ESR_DFSC(iss);

  w_esr_el1(0);
  if(ec == EC_SVC64){
    // system call

    if(p->killed)
      exit(-1);

    intr_on();

    syscall();
  } else {
    // Evaluation children report confirmed MTE faults through wait(), not logs.
    if (ec == EC_DATA_ABORT_EL0 && p->quiet_mte_faults) {
      if (dfsc == DFSC_MTE_TAG_CHECK)
        exit(MTE_FAULT_EXIT_STATUS);
      if (dfsc >= 0x04 && dfsc <= 0x07)
        exit(TRANSLATION_FAULT_EXIT_STATUS);
    }

    if (ec == EC_DATA_ABORT_EL0){
      printf("usertrap(): data abort from user pid=%d\n", p->pid);
      printf("             kind=%s dfsc=%p\n", data_abort_kind(iss), dfsc);
      printf("             esr=%p iss=%p elr=%p far=%p\n", esr, iss, elr, far);
    } else {
      printf("usertrap(): unexpected ec %p pid=%d\n", ec, p->pid);
      printf("             esr=%p iss=%p elr=%p far=%p\n", esr, iss, elr, far);
  }

  p->killed = 1;
    p->killed = 1;
  }

  if(p->killed)
    exit(-1);

  intr_off();
}


// interrupts and exceptions from kernel code go here via kernelvec,
// on whatever the current kernel stack is.
void 
kerneltrap()
{
  uint64 esr = r_esr_el1();
  w_esr_el1(0);
  
  if(intr_get() != 0)
    panic("kerneltrap: interrupts enabled");

  uint64 ec = (esr >> 26) & 0x3f;
  uint64 iss = esr & 0x1ffffff;
  printf("esr %p %p %p\n", esr, ec, iss);
  printf("elr=%p far=%p\n", r_elr_el1(), r_far_el1());
  panic("kerneltrap");
}

void
kernelirq()
{
  int which_dev = devintr();

  // give up the CPU if this is a timer interrupt.
  if(which_dev == 2 && myproc() != 0 && myproc()->state == RUNNING)
    yield();
}


void
clockintr()
{
  acquire(&tickslock);
  ticks++;
  wakeup(&ticks);
  release(&tickslock);
}

// check if it's an external interrupt and handle it.
// returns 2 if timer interrupt,
// 1 if other device,
// 0 if not recognized.
int
devintr()
{
  uint32 iar = gic_iar();
  uint32 irq = gic_iar_irq(iar);
  int dev = 0;

  // printf("devintr irq %d", irq);
  if(irq == UART0_IRQ){
    uartintr();
    dev = 1;
  } else if(irq == VIRTIO0_IRQ){
    virtio_disk_intr();
    dev = 1;
  } else if(irq == TIMER0_IRQ){
    if(cpuid() == 0){
      clockintr();
    }
    timerintr();
    dev = 2;
  } else if(irq == 1023){
    // do nothing
  } else if(irq){
    printf("unexpected interrupt irq=%d\n", irq);
  }

  if(dev) {
    gic_eoi(iar);
  }

  return dev;
}

