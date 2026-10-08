#ifndef __ASSEMBLER__

// which core is this?
static inline uint64
cpuid()
{
  uint64 x;
  asm volatile("mrs %0, mpidr_el1" : "=r" (x) );
  return x & 0xff;
}

// Vector Base Address Register in EL1
static inline void
w_vbar_el1(uint64 x)
{
  asm volatile("msr vbar_el1, %0" : : "r" (x) );
}

static inline uint64
r_vbar_el1()
{
  uint64 x;
  asm volatile("mrs %0, vbar_el1" : "=r" (x) );
  return x;
}

// TODO: fix comment
// supervisor address translation and protection;
// holds the address of the page table.
static inline void
w_ttbr0_el1(uint64 x)
{
  asm volatile("msr ttbr0_el1, %0" : : "r" (x) );
}

static inline uint64
r_ttbr0_el1()
{
  uint64 x;
  asm volatile("mrs %0, ttbr0_el1" : "=r" (x) );
  return x;
}

static inline void
w_ttbr1_el1(uint64 x)
{
  asm volatile("msr ttbr1_el1, %0" : : "r" (x));
}

static inline uint64
r_ttbr1_el1()
{
  uint64 x;
  asm volatile("mrs %0, ttbr1_el1" : "=r" (x) );
  return x;
}

static inline uint64
r_esr_el1()
{
  uint64 x;
  asm volatile("mrs %0, esr_el1" : "=r" (x) );
  return x;
}

static inline void
w_esr_el1(uint64 x)
{
  asm volatile("msr esr_el1, %0" : : "r" (x) );
}

static inline uint64
r_elr_el1()
{
  uint64 x;
  asm volatile("mrs %0, elr_el1" : "=r" (x) );
  return x;
}

static inline uint64
r_far_el1()
{
  uint64 x;
  asm volatile("mrs %0, far_el1" : "=r" (x) );
  return x;
}

// armv8 generic timer
static inline uint64
r_cntv_ctl_el0()
{
  uint64 x;
  asm volatile("mrs %0, cntv_ctl_el0" : "=r" (x) );
  return x;
}

static inline void
w_cntv_ctl_el0(uint64 x)
{
  asm volatile("msr cntv_ctl_el0, %0" : : "r" (x) );
}

static inline uint64
r_cntv_tval_el0()
{
  uint64 x;
  asm volatile("mrs %0, cntv_tval_el0" : "=r" (x) );
  return x;
}

static inline void
w_cntv_tval_el0(uint64 x)
{
  asm volatile("msr cntv_tval_el0, %0" : : "r" (x) );
}

static inline uint64
r_cntvct_el0()
{
  uint64 x;
  asm volatile("mrs %0, cntvct_el0" : "=r" (x) );
  return x;
}

static inline uint64
r_cntfrq_el0()
{
  uint64 x;
  asm volatile("mrs %0, cntfrq_el0" : "=r" (x) );
  return x;
}

// enable device interrupts(irq)
static inline void
intr_on()
{
  asm volatile("msr daifclr, #0xf" ::: "memory");
}

// disable device interrupts(irq)
static inline void
intr_off()
{
  asm volatile("msr daifset, #0xf" ::: "memory");
}

static inline uint64
daif()
{
  uint64 x;
  asm volatile("mrs %0, daif" : "=r" (x) );
  return x;
}

// are device interrupts(irq) enabled?
static inline int
intr_get()
{
  uint64 x = daif();
  return ((x >> 6) & 0x2) == 0;
}

static inline uint64
r_sp()
{
  uint64 x;
  asm volatile("mov %0, sp" : "=r" (x) );
  return x;
}

static inline uint64
r_ra()
{
  uint64 x;
  asm volatile("mov %0, x30" : "=r" (x) );
  return x;
}

static inline void
isb()
{
  asm volatile("isb");
}

static inline void
dsb()
{
  asm volatile("dsb sy");
}

// flush the TLB.
static inline void
flush_tlb()
{
  asm volatile("dsb ishst");
  asm volatile("tlbi vmalle1is");
  asm volatile("dsb ish");
  isb();
}

// print utilities
static inline uint64
r_id_aa64pfr1_el1(void)
{
  uint64 x;
  asm volatile("mrs %0, id_aa64pfr1_el1" : "=r"(x));
  return x;
}

// 仮想アドレス変換の設定レジスタであるTCR_EL1を読む
  // mrs : move to register from special register 
  //       システムレジスタや特殊レジスタから汎用レジスタへ値を読み込む
static inline uint64
r_tcr_el1(void)
{
 uint64 x;
 asm volatile("mrs %0, tcr_el1" : "=r"(x));
 return x;
}
// TCR_EL1に値を書き込む
   // msr : move to special register
   //       システムレジスタに値を書き込む
   // isb : instruction synchronization barrier
   //       armプロセッサのパイプラインをフラッシュし，それ以降の命令を新しく再fetchさせるバリア命令
static inline void
w_tcr_el1(uint64 x)
{
  asm volatile("msr tcr_el1, %0" : : "r"(x));
  isb();
}
// MMUやcache, MTE tag checkの有効化などを制御するレジスタであるSCTLR_EL1を読む
static inline uint64
r_sctlr_el1(void)
{
  uint64 x;
  asm volatile("mrs %0, sctlr_el1" : "=r"(x));
  return x;
}
// SCTLR_EL1に値を書き込む（MTEのtag checkを有効にするときに使用する）
static inline void
w_sctlr_el1(uint64 x)
{
  asm volatile("msr sctlr_el1, %0" : : "r"(x));
  isb();
}
// MTEのrandom tag generationに関係するレジスタであるGCR_EL1に値を書き込む
static inline void
w_gcr_el1(uint64 x)
{
  asm volatile("msr s3_0_c1_c0_6, %0" : : "r"(x));
  isb();
}
// tagを埋め込んだポインタを作成する (pointerの[59:56]にlogical tagを入れる)
static inline uint64
mte_tagged_ptr(uint64 ptr, uint64 tag)
{
  return (ptr & ~(0xfUL << 56)) | ((tag & 0xf) << 56);
}
// tagged pointerが指す16 byte granuleにSTG命令でallocation tagを書き込む
static inline void
mte_store_tag(uint64 tagged_ptr)
{
  asm volatile(
    ".arch_extension memtag\n"
    "stg %0, [%0]\n"
    :
    : "r"(tagged_ptr)
    : "memory");
}

typedef uint64 pte_t;
typedef uint64 *pagetable_t; // 512 PTEs

#endif  /* __ASSEMBLER__ */

#define PGSIZE 4096 // bytes per page
#define PGSHIFT 12  // bits of offset within a page

#define PGROUNDUP(sz)  (((sz)+PGSIZE-1) & ~(PGSIZE-1))
#define PGROUNDDOWN(a) (((a)) & ~(PGSIZE-1))

#define PTE_VALID 1  // level 0,1,2 descriptor: valid
#define PTE_TABLE 2  // level 0,1,2 descriptor: table
#define PTE_V     3  // level 3 descriptor: valid
// PTE_AF(Access Flag)
//
// 0 -- this block entry has not yet.
// 1 -- this block entry has been used.
#define PTE_AF  (1 << 10)
// PTE_AP(Access Permission) is 2bit field.
//        EL0   EL1
// 00 --   x    RW
// 01 --  RW    RW
// 10 --   x    RO
// 11 --  RO    RO
#define PTE_AP(ap)  (((ap) & 3) << 6)
#define PTE_U   PTE_AP(1)
#define PTE_RO  PTE_AP(2)
#define PTE_URO PTE_AP(3)
#define PTE_PXN (1UL << 53)   // Privileged eXecute Never
#define PTE_UXN (1UL << 54)   // Unprivileged(user) eXecute Never
#define PTE_XN (PTE_PXN|PTE_UXN)  // eXecute Never

// attribute index
// index is set by mair_el1
#define AI_DEVICE_nGnRnE_IDX  0x0
#define AI_NORMAL_NC_IDX      0x1
// MAIR_EL1のインデックス2のスロットのエントリをnormal-tagged memoryとして使用する
#define AI_NORMAL_TAGGED_IDX  0x2

// memory type
#define MT_DEVICE_nGnRnE  0x0
#define MT_NORMAL_NC      0x44
// memory attributeの対応表を保持するレジスタであるMAIR_EL1に値を登録し，
// PTE側で対応するattribute indexを選ぶことで，そのページをnormal-tagged memoryとして扱わせる
#define MT_NORMAL_TAGGED  0xf0

#define PTE_INDX(i) (((i) & 7) << 2)
#define PTE_DEVICE  PTE_INDX(AI_DEVICE_nGnRnE_IDX)
#define PTE_NORMAL  PTE_INDX(AI_NORMAL_NC_IDX)
// mappages()でページをmapするときに使用するflag (vm.cでPTE_TAGGEDを使えるようにする)
#define PTE_TAGGED  PTE_INDX(AI_NORMAL_TAGGED_IDX)
// shift a physical address to the right place for a PTE.
#define PA2PTE(pa)  ((uint64)(pa) & 0xfffffffff000)
#define PTE2PA(pte) ((uint64)(pte) & 0xfffffffff000)

#define PTE_FLAGS(pte)  ((pte) & (0x600000000003ff))

//
//  39bit(=512GB) Virtual Address
//
//     63   39 38    30 29    21 20    12 11       0
//    +-------+--------+--------+--------+----------+
//    | TTBRn | level1 | level2 | level3 | page off |
//    |       | (PUD)  | (PMD)  | (PTE)  |          |
//    +-------+--------+--------+--------+----------+
//

// extract the three 9-bit page table indices from a virtual address.
#define PXMASK          0x1FF // 9 bits
#define PXSHIFT(level)  (39-(level)*9)
#define PX(level, va) ((((uint64)(va)) >> PXSHIFT(level)) & PXMASK)

// translation control register
#define TCR_T0SZ(n)   ((n) & 0x3f)
#define TCR_TG0(n)    (((n) & 0x3) << 14)
#define TCR_T1SZ(n)   (((n) & 0x3f) << 16)
#define TCR_TG1(n)    (((n) & 0x3) << 30)
#define TCR_IPS(n)    (((n) & 0x7) << 32)
// カーネルアドレス側でTBIを有効にする
#define TCR_TBI1        (1UL << 38)
// tag check fault modeをマスクする
#define SCTLR_TCF_MASK  (3UL << 40)
// tag mismatchをsynchronous faultとして発生させる設定
#define SCTLR_TCF_SYNC  (1UL << 40)
// EL1からallocation tagへアクセスできるように設定するビット
#define SCTLR_ATA       (1UL << 43)

// ユーザ側で同様の設定
#define TCR_TBI0        (1UL << 37)
#define SCTLR_TCF0_MASK (3UL << 38)
#define SCTLR_TCF0_SYNC (1UL << 38)
#define SCTLR_ATA0      (1UL << 42)

// MTEの論理タグを取り除くためのヘルパ
#define MTE_TAG_SHIFT 56
#define MTE_TAG_MASK  (0xfUL << MTE_TAG_SHIFT)


#ifndef __ASSEMBLER__
static inline uint64
mte_untagged_addr(uint64 va)
{
  return va & ~MTE_TAG_MASK;
}
#endif
