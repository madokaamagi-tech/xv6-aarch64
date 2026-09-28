#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "aarch64.h"
#include "defs.h"

#define  PSCI_CPUON   0xc4000003

volatile static int started = 0;
extern char end[];  // first address after kernel loaded from ELF file

void _entry(void);
void psci_call(uint64 fn, int cpuid, uint64 entry, uint64 ctxid);
void delay(uint32 c);

// kernel EL1でMTE tag checkとallocation tag accessを有効にする
static void
mte_enable_el1(void)
{
  // 現在のSCTLR_EL1の値を読み出す
  uint64 sctlr = r_sctlr_el1();
  // 次に新しいモードを入れるため，一度 tag check fault mode のビットをクリアする
  sctlr &= ~SCTLR_TCF_MASK;
  // tag mismatchが起きたらその命令でストップしてfaultを起こす
  sctlr |= SCTLR_TCF_SYNC;
  // EL1からallocation tagにアクセスできるようにしておく
  sctlr |= SCTLR_ATA;
  // 更新した値をSCTLR_EL1に書き込む
  w_sctlr_el1(sctlr);
}
/*
static void
mte_smoke_test(void)
{
  char *p = kalloc();
  if(p == 0)
    panic("mte_smoke_test: kalloc");

  uint64 tagged = mte_tagged_ptr((uint64)p, 5);

  printf("mte smoke: p=%p tagged=%p\n", p, tagged);

  mte_store_tag(tagged);

  *(volatile char *)tagged = 42;
  printf("mte smoke: tagged store ok\n");
  
  uint64 bad = mte_tagged_ptr((uint64)p, 6);

  printf("mte smoke: bad=%p, next store should fault\n", bad);
  *(volatile char *)bad = 99;

  printf("mte smoke: ERROR bad tagged store did not fault\n");
  
  kfree(p);
}
*/

static void
mte_smoke_test(void)
{
  char *p = (char*)P2V(PHYSTOP - PGSIZE);

  uint64 tagged = mte_tagged_ptr((uint64)p, 5);

  printf("mte smoke: p=%p tagged=%p\n", p, tagged);

  mte_store_tag(tagged);

  *(volatile char *)tagged = 42;
  printf("mte smoke: tagged store ok\n");

  uint64 bad = mte_tagged_ptr((uint64)p, 6);

  printf("mte smoke: bad=%p, next store should fault\n", bad);
  *(volatile char *)bad = 99;

  printf("mte smoke: ERROR bad tagged store did not fault\n");
}


// start() jumps here in EL1 on all CPUs.
void
main()
{
  if(cpuid() == 0){
    for(int i = 1; i < NCPU; i++)   // wakeup other processors
      psci_call(PSCI_CPUON, i, V2P(_entry), 0);
    isb();
    kinit1(end, (void*)(KERNLINK+2*1024*1024));  // physical page allocator
    kvminit();       // create kernel page table
    kvminithart();   // turn on paging
//    kinit2((void*)(KERNLINK+2*1024*1024), P2V(PHYSTOP));
    kinit2((void*)(KERNLINK+2*1024*1024), P2V(PHYSTOP - PGSIZE));
    consoleinit();
    printfinit();
    printf("\n");
    printf("xv6 kernel is booting\n");
    printf("\n");

    // print utilities
    uint64 pfr1 = r_id_aa64pfr1_el1();
    printf("ID_AA64PFR1_EL1 %p MTE %d\n", pfr1, (int)((pfr1 >> 8) & 0xf));

    procinit();      // process table
    trapinit();      // trap vectors
    trapinithart();  // install trap vector
    // test
    mte_enable_el1();
    mte_smoke_test();
    // test
    gicv3init();     // set up interrupt controller
    gicv3inithart();
    timerinit();
    binit();         // buffer cache
    iinit();         // inode table
    fileinit();      // file table
    virtio_disk_init(); // emulated hard disk
    userinit();      // first user process
    __sync_synchronize();
    started = 1;
  } else {
    while(started == 0)
      ;
    __sync_synchronize();
    kvminithart();    // turn on paging
    printf("hart %d starting\n", cpuid());
    trapinithart();   // install trap vector
    gicv3inithart();
    timerinit();
  }

  scheduler();
}
