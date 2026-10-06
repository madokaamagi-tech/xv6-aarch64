#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

#define PGSIZE 4096
#define VPA_BASE (1ULL << 30)
#define VPA_TAG_STRIDE (1ULL << 26)

struct vpa_page {
  void *raw;  // sbrk()で確保した元の通常VA
  void *ptr;  // tag + VPN で作成した VPA
  int tag;    // この allocation に割り当てたタグ
};

static int next_tag = 1;

// 1~15 の間でタグを割り当てる
static int
alloc_tag(void)
{
  int tag = next_tag;

  next_tag++;
  if (next_tag >= 16)
    next_tag = 1;

  return tag;
}


static struct vpa_page
vpa_alloc_page(void)
{
  struct vpa_page r;

  r.raw = sbrk(PGSIZE);
  if(r.raw == (void *)-1){
    r.raw = 0;
    r.ptr = 0;
    r.tag = 0;
    return r;
  }

  r.tag = alloc_tag();

  // 1 page 分のメモリを確保する
  uint64 vpa = vpamap(r.raw, r.tag);
  if (vpa == (uint64)-1) {
    r.ptr = 0;
    return r;
  }

  // VPA を元ページと同じ物理ページへマッピングする
  r.ptr = (void *)vpa;
  return r;
}

static int
vpa_free_page(struct vpa_page *p)
{
  if (p == 0 || p->ptr == 0)
    return -1;

  return pgclearpresent(p->ptr);
}

// 既存の rawpage を新しいタグで再マップする関数
static struct vpa_page
vpa_reuse_page(void *raw)
{
  struct vpa_page r;

  r.raw = raw;
  r.tag = alloc_tag();

  uint64 vpa = vpamap(r.raw, r.tag);
  if (vpa == (uint64)-1){
    r.ptr = 0;
    return r;
  }

  r.ptr = (void *)vpa;
  return r;
}

int
main(void)
{
  struct vpa_page a = vpa_alloc_page();
  printf("a raw=%p tag=%d ptr%p\n", a.raw, a.tag, a.ptr);

  *(volatile char *)a.ptr = 1;
  printf("a write ok\n");

  if (vpa_free_page(&a) < 0) {
    printf("vpa_free_page failed\n");
    exit(1);
  }

  printf("a freed\n");

  struct vpa_page c = vpa_reuse_page(a.raw);
  if(c.ptr == 0){
    printf("vpa_reuse_page failed\n");
    exit(1);
  }

  printf("c raw=%p tag=%d ptr=%p\n", c.raw, c.tag, c.ptr);

  *(volatile char *)  c.ptr = 2;
  printf("c write ok\n");

  printf("old a.ptr should fault now: %p\n", a.ptr);
  *(volatile char *)a.ptr = 3;
}
