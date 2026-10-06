#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

#define PGSIZE 4096
#define MAX_VPA_ALLOCS 16

struct vpa_meta {
  void *raw;      // sbrk()で確保した元の仮想アドレス
  void *ptr;      // tag + VPN(raw) で作成したVPA
  int tag;        // 今回のallocationに割り当てたタグ
  int active;     // 使用中かfree済みかを占める値
  int next_free;  // free list用のリンク
};

static struct vpa_meta metas[MAX_VPA_ALLOCS];
static int free_head = -1;  // -1はfree listが空ということを表す
static int next_tag = 1;

static int 
alloc_tag(void)
{
  int tag = next_tag++;

  if (next_tag >= 16)
    next_tag = 1;

  return tag;
}

//  まだsbrk()でページを割り当てていないエントリを探す
//  （free list がからの時に使用する）
static int
find_unused_meta(void)
{
  for (int i = 0; i < MAX_VPA_ALLOCS; i++) {
    if (metas[i].raw == 0)
      return i;
  }
  return -1;
}

// 引数ptrと一致するmetadataを探す
// vpa_free(ptr)でどのポインタをfreeするか調べるために使用する
static int
find_active_by_ptr(void *ptr)
{
  for (int i = 0; i < MAX_VPA_ALLOCS; i++) {
    if (metas[i].active && metas[i].ptr == ptr)
      return i;
  }
  return -1;
}

static void*
vpa_alloc(void)
{
  int idx;
  void *raw;
  
  // まずfree済みのページがあるか確認し，ある場合それを再利用する
  // なければelse文で新しくsbrk()して作成する
  if (free_head >= 0) {
    idx = free_head;
    free_head = metas[idx].next_free;
    // 以前使用していたfree済みページを新しいタグで再利用する
    raw = metas[idx].raw;
  } else {
    idx = find_unused_meta();

    if (idx < 0)
      return 0;

    raw = sbrk(PGSIZE);
    if (raw == (void *)-1)
      return 0;

    metas[idx].raw = raw;
  }

  metas[idx].tag = alloc_tag();

  uint64 vpa = vpamap(raw, metas[idx].tag);
  if (vpa == (uint64)-1)
    return 0;

  metas[idx].ptr = (void *)vpa;
  metas[idx].active = 1;
  metas[idx].next_free = -1;

  return metas[idx].ptr;
}

// VPA pointerをfreeする時，物理ページをOSに返すのではなく，
// そのVPAのPTE_Vを落として，あとでシア利用できるようにする
static int
vpa_free(void *ptr)
{
  int idx = find_active_by_ptr(ptr);
  if(idx < 0)
    return -1;

  if (pgclearpresent(ptr) < 0)
    return -1;
  metas[idx].active = 0;
  // この枠をfreelistの先頭に追加する
  metas[idx].next_free = free_head;
  free_head = idx;

  return 0;
}

int
main(void)
{
  void *a = vpa_alloc();
  if(a == 0){
    printf("vpa_alloc a failed\n");
    exit(1);
  }

  printf("a ptr=%p\n", a);

  *(volatile char *)a = 1;
  printf("a write ok\n");

  if(vpa_free(a) < 0){
    printf("vpa_free a failed\n");
    exit(1);
  }

  printf("a freed\n");

  void *c = vpa_alloc();
  if(c == 0){
    printf("vpa_alloc c failed\n");
    exit(1);
  }

  printf("c ptr=%p\n", c);

  *(volatile char *)c = 2;
  printf("c write ok\n");

  printf("old a ptr should fault now: %p\n", a);
  *(volatile char *)a = 3;

  printf("ERROR: old a ptr did not fault\n");
  exit(0);
}
