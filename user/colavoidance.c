#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

#define PGSIZE 4096
#define MAX_VPA_ALLOCS 16
#define VPA_TAG_MIN 1
#define VPA_TAG_MAX 15

struct vpa_meta {
  void *raw;      // sbrk()で確保した元の仮想アドレス
  void *ptr;      // tag + VPN(raw) で作成したVPA
  uint64 nbytes;  // ユーザがvpa_malloc() に要求したサイズ
  uint64 npages;  // 確保したページ数
  uint64 used_tags;
  int tag;        // 今回のallocationに割り当てたタグ
  int active;     // 使用中かfree済みかを占める値
  int next_free;  // free list用のリンク
};

static struct vpa_meta metas[MAX_VPA_ALLOCS];
static int free_head = -1;  // -1はfree listが空ということを表す
static int next_tag = 1;

// タグ番号に対応するビットを返す
static uint64
tag_bit(int tag)
{
  return 1ULL << tag;
}

static int
tags_used(int idx, int tag)
{
  return (metas[idx].used_tags & tag_bit(tag)) != 0;
}

static int
all_tags_used(int idx)
{
  for (int tag = VPA_TAG_MIN; tag <= VPA_TAG_MAX; tag++) {
    if (!tag_used(idx, tag))
      return 0;
  }
  return 1;
}

static int
alloc_tag_for_meta(int idx)
{
  // 全てのタグを使い切った場合は再利用を許す
  // 即時再利用を避けるため，直前のタグは used として残しておく
  if (all_tags_used(idx)) {
    metas[idx].used_tags = 0;
    if (metas[idx].tag >= VPA_TAG_MIN && metas[idx].tag <= VPA_TAG_MAX)
      metas[idx].used_tags |= tag_bit(metas[idx].tag);
  }
  
  // 直近のタグの次から探す
  int start = metas[idx].tag;
  for (int n = 0; n < VPA_TAG_MAX - VPA_TAG_MIN; n++) {
    int tag = VPA_TAG_MIN + ((start - VPA_TAG_MIN + 1 + n) % (VPA_TAG_MAX - VPA_TAG_MIN + 1));

    if (!tag_used(idx, tag)) {
      metas[idx].used_tags |= tag_bit(tag);
      return tag;
    }
  }

  return -1;
}

static int 
alloc_tag(void)
{
  int tag = next_tag++;

  if (next_tag >= 16)
    next_tag = 1;

  return tag;
}

static uint64
roundup_pages(uint64 nbytes)
{
  return (nbytes + PGSIZE - 1) / PGSIZE;
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

// free list の中から要求されるページ数と同じallocationを探す
// (まずは分割や結合はせず．同じnpagesのものだけ再利用する)
static int
pop_free_meta(uint64 npages)
{
  int prev = -1;
  int cur = free_head;

  while (cur >= 0) {
    if (metas[cur].npages == npages) {
      if (prev < 0)
        free_head = metas[cur].next_free;
      else 
        metas[prev].next_free = metas[cur].next_free;

      metas[cur].next_free = -1;
      return cur;
    }

    prev = cur;
    cur = metas[cur].next_free;
  }

  return -1;
}


static void
print_meta(char *name, void *ptr)
{
  for (int i = 0; i < MAX_VPA_ALLOCS; i++) {
    if (metas[i].ptr == ptr) {
      printf("%s raw=%p tag=%d ptr=%p nbytes=%d npages=%d used_tags=%p active=%d\n",
       name,
       metas[i].raw,
       metas[i].tag,
       metas[i].ptr,
       metas[i].nbytes,
       metas[i].npages,
       metas[i].used_tags,
       metas[i].active);
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
  uint64 npages;

  if (nbytes == 0)
    return 0;

  npages = roundup_pages(nbytes);

  idx = pop_free_meta(npages);
  if (idx >= 0) {
    raw = metas[idx].raw;
  } else {
    idx = find_unused_meta();
    if (idx < 0)
      return 0;
    raw = sbrk(npages * PGSIZE);
    if (raw == (void *)-1)
      return 0;

    metas[idx].raw = raw;
    metas[idx].used_tags = 0;
    metas[idx].tag = 0;
  }

  metas[idx].tag = alloc_tag_for_meta(idx);
  if (metas[idx].tag < 0)
    return 0;

  // 各raw page に対してVPA mappingを割り当てる
  // raw が複数ページでも，タグが同じならVPA側でも連続した領域になる
  uint64 first_vpa = 0;
  for (uint64 i = 0; i < npages; i++) {
    void *page = (void *)((uint64)raw + i * PGSIZE);
    uint64 vpa = vpamap(page, metas[idx].tag);

    if (vpa == (uint64)-1)
      return 0;
    if (i == 0)
      first_vpa = vpa;
  }

  metas[idx].ptr = (void *)first_vpa;
  metas[idx].nbytes = nbytes;
  metas[idx].npages = npages;
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

  // allocationが使っているVPAの領域の全ページをinvalidにする
  for (uint64 i = 0; i < metas[idx].npages; i++) {
    void *page = (void *)((uint64)metas[idx].ptr + i * PGSIZE);
    if (pgclearpresent(page) < 0)
      return -1;
  }

  metas[idx].active = 0;
  metas[idx].next_free = free_head;
  free_head = idx;

  return 0;
}
  
int
main(void)
{
  void *a = vpa_malloc(32);
  void *b = vpa_malloc(PGSIZE + 16);

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

  void *c = vpa_malloc(PGSIZE + 16);
  void *d = vpa_malloc(32);

  if(c == 0 || d == 0){
    printf("reuse alloc failed\n");
    exit(1);
  }

  print_meta("c", c);
  print_meta("d", d);

  *(volatile char *)c = 3;
  *(volatile char *)((uint64)c + PGSIZE) = 4;
  *(volatile char *)d = 5;

  printf("c/d writes ok\n");
  printf("old b first page should fault now: %p\n", b);
  *(volatile char *)b = 6;

  printf("ERROR: old b did not fault\n");
  exit(0);
}
