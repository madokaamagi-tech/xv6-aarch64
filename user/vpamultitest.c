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

static void
print_meta(char *name, void *ptr)
{
  for (int i = 0; i < MAX_VPA_ALLOCS; i++) {
    if (metas[i].ptr == ptr) {
      printf("%s raw=%p tag=%d ptr=%p active=%d\n",
              name, metas[i].raw, metas[i].tag, metas[i].ptr, metas[i].active);
      return;
    }
  }

  printf("%s ptr=%p metadata not found\n", name, ptr);
}

static void*
vpa_malloc(uint nbytes)
{
  int idx;
  void *raw;

  // 1 allocation = 1 page
  if (nbytes == 0 || nbytes > PGSIZE)
    return 0;

  if (free_head >= 0) {
    idx = free_head;
    free_head = metas[idx].next_free;
    
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

  // allocationごとに新しいページタグを割り当てる
  // 同じraw pagwを再利用しても古いポインタとは別のVPAになる
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
// そのVPAのPTE_Vを落として，あとで再利用できるようにする
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
  void *a = vpa_malloc(32);
  void *b = vpa_malloc(64);

  if(a == 0 || b == 0){
    printf("initial alloc failed\n");
    exit(1);
  }

  print_meta("a", a);
  print_meta("b", b);

  *(volatile char *)a = 1;
  *(volatile char *)b = 2;
  printf("a/b writes ok\n");

  if(vpa_free(a) < 0){
    printf("free a failed\n");
    exit(1);
  }

  if(vpa_free(b) < 0){
    printf("free b failed\n");
    exit(1);
  }

  printf("a/b freed\n");

  void *c = vpa_malloc(128);
  void *d = vpa_malloc(256);

  if(c == 0 || d == 0){
    printf("reuse alloc failed\n");
    exit(1);
  }

  print_meta("c", c);
  print_meta("d", d);

  *(volatile char *)c = 3;
  *(volatile char *)d = 4;
  printf("c/d writes ok\n");

  printf("old b should fault now: %p\n", b);
  *(volatile char *)b = 5;

  printf("ERROR: old b did not fault\n");
  exit(0);
}
