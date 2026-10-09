#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

#define PGSIZE 4096
#define MTE_GRANULE_SIZE 16
#define MTE_TAG_SHIFT 56
#define MTE_TAG_MASK (0xfULL << MTE_TAG_SHIFT)

#define MAX_VPA_PAGES 4
#define MAX_OBJECTS 32
#define MAX_GRANULES (PGSIZE / MTE_GRANULE_SIZE)
#define GRANULE_BITMAP_WORDS (MAX_GRANULES / 64)
#define FREE_OBJECT_TAG 0

struct vpa_page_meta {
  void *raw;
  void *vpa;
  int page_tag;
  int active;
  int live_objects;
  uint64 used_granules[GRANULE_BITMAP_WORDS];
  uint64 used_object_tags[MAX_GRANULES];
};

struct object_meta {
  int page_idx;
  int start_granule;
  int ngrans;
  uint64 nbytes;
  int object_tag;
  int active;
  void *ptr;
};

static struct vpa_page_meta pages[MAX_VPA_PAGES];
static struct object_meta objects[MAX_OBJECTS];

static int next_page_tag = 1;

static int 
granule_used(int page_idx, int gran)
{
  return (pages[page_idx].used_granules[gran / 64] & (1ULL << (gran % 64))) != 0;
}

static void set_granule_used(int page_idx, int gran)
{
  pages[page_idx].used_granules[gran / 64] |= 1ULL << (gran % 64);
}

static void
clear_granule_used(int page_idx, int gran)
{
  pages[page_idx].used_granules[gran / 64] &= ~(1ULL << (gran % 64));
}

static int 
range_is_free(int page_idx, int start, int ngrans)
{
  if (start + ngrans > MAX_GRANULES)
    return 0;

  for (int i = 0; i < ngrans; i++) {
    if (granule_used(page_idx, start + i))
      return 0;
  }

  return 1;
}

static void
mark_range_used(int page_idx, int start, int ngrans)
{
  for (int i = 0; i < ngrans; i++)
    set_granule_used(page_idx, start + i);
}

static int alloc_object_tag_for_range(int page_idx, int start, int ngrans);

static int
find_free_range_with_tag(int page_idx, int ngrans, int *tag_out)
{
  for(int start = 0; start <= MAX_GRANULES - ngrans; start++){
    if(!range_is_free(page_idx, start, ngrans))
      continue;

    int tag = alloc_object_tag_for_range(page_idx, start, ngrans);
    if(tag >= 0){
      *tag_out = tag;
      return start;
    }
  }

  return -1;
}

static void 
mark_range_free(int page_idx, int start, int ngrans)
{
  for (int i = 0; i < ngrans; i++)
    clear_granule_used(page_idx, start + i);
}

static int
roundup_granules(uint64 nbytes)
{
  return (nbytes + MTE_GRANULE_SIZE - 1) / MTE_GRANULE_SIZE;
}

static uint16
object_tag_bit(int tag)
{
  return 1U << tag;
}

static int
object_tag_used_for_range(int page_idx, int start, int ngrans, int tag)
{
  uint16 bit = object_tag_bit(tag);

  for(int i = 0; i < ngrans; i++){
    if(pages[page_idx].used_object_tags[start + i] & bit)
      return 1;
  }

  return 0;
}

static void
mark_object_tag_used_for_range(int page_idx, int start, int ngrans, int tag)
{
  uint16 bit = object_tag_bit(tag);

  for(int i = 0; i < ngrans; i++)
    pages[page_idx].used_object_tags[start + i] |= bit;
}

static int
alloc_object_tag_for_range(int page_idx, int start, int ngrans)
{
  for(int tag = 1; tag <= 15; tag++){
    if(!object_tag_used_for_range(page_idx, start, ngrans, tag)){
      mark_object_tag_used_for_range(page_idx, start, ngrans, tag);
      return tag;
    }
  }

  return -1;
}

static uint64
mte_untagged_addr(uint64 ptr)
{
  return ptr & ~MTE_TAG_MASK;
}

static void *
mte_tagged_ptr(void *ptr, int object_tag)
{
  uint64 tag_bits = ((uint64)object_tag & 0xf) << MTE_TAG_SHIFT;
  return (void *)(mte_untagged_addr((uint64)ptr) | tag_bits);
}

static void
mte_store_tag(void *tagged_ptr)
{
  asm volatile (
    ".arch_extension memtag\n"
    "stg %0, [%0]\n"
    :
    : "r"(tagged_ptr)
    : "memory");
}

static void
mte_store_tag_range(void *tagged_ptr, uint64 nbytes)
{
  uint64 size = (nbytes + MTE_GRANULE_SIZE - 1) & ~(MTE_GRANULE_SIZE - 1);
  for (uint64 off = 0; off < size; off += MTE_GRANULE_SIZE)
    mte_store_tag((void *)((uint64)tagged_ptr + off));
}

static int
alloc_vpa_page(void)
{
  for (int i = 0; i < MAX_VPA_PAGES; i++) {
    if (!pages[i].active) {
      void *raw = sbrk(PGSIZE);
      if (raw == (void *)-1)
        return -1;

      int page_tag = next_page_tag++;
      if (next_page_tag > 15)
        next_page_tag = 1;

      uint64 vpa = vpamap(raw, page_tag);
      if (vpa == (uint64)-1)
        return -1;

      pages[i].raw = raw;
      pages[i].vpa = (void *)vpa;
      pages[i].page_tag = page_tag;
      pages[i].active = 1;
      pages[i].live_objects = 0;

      for (int w = 0; w < GRANULE_BITMAP_WORDS; w++)
        pages[i].used_granules[w] = 0;

      return i;
    }
  }

  return -1;
}

static void *
vpa_object_malloc(uint nbytes)
{
  if (nbytes == 0)
    return 0;

  int ngrans = roundup_granules(nbytes);
  if (ngrans > MAX_GRANULES)
    return 0;

  int page_idx = -1;
  int start_granule = -1;
  int object_tag = -1;

  for (int i = 0; i < MAX_VPA_PAGES; i++) {
    if (!pages[i].active)
      continue;

    start_granule = find_free_range_with_tag(i, ngrans, &object_tag);
    if (start_granule>= 0) {
      page_idx = i;
      break;
    }
  }

  if (page_idx < 0) {
    page_idx = alloc_vpa_page();
    if (page_idx < 0)
      return 0;
    start_granule = 0;
    object_tag = alloc_object_tag_for_range(page_idx, start_granule, ngrans);
    if (object_tag < 0)
      return 0;
  }

  int obj_idx = -1;
  for (int i = 0; i < MAX_OBJECTS; i++) {
    if (!objects[i].active) {
      obj_idx = i;
      break;
      }
  }

  if (obj_idx < 0)
    return 0;

  mark_range_used(page_idx, start_granule, ngrans);
  pages[page_idx].live_objects++;

  uint64 vpa_addr = 
    (uint64)pages[page_idx].vpa + start_granule * MTE_GRANULE_SIZE;
  
  void *ptr = mte_tagged_ptr((void *)vpa_addr, object_tag);
  mte_store_tag_range(ptr, ngrans * MTE_GRANULE_SIZE);

  objects[obj_idx].page_idx = page_idx;
  objects[obj_idx].start_granule = start_granule;
  objects[obj_idx].ngrans = ngrans;
  objects[obj_idx].object_tag = object_tag;
  objects[obj_idx].active = 1;
  objects[obj_idx].ptr = ptr;

  return ptr;
}

static int 
vpa_object_free(void *ptr)
{
  for (int i = 0; i < MAX_OBJECTS; i++) {
    if (objects[i].active && objects[i].ptr == ptr) {
      int page_idx = objects[i].page_idx;

      void *free_ptr = 
        mte_tagged_ptr((void *)mte_untagged_addr((uint64)ptr),
                          FREE_OBJECT_TAG);

      // 対象のオブジェクトだけタグを変更する
      mte_store_tag_range(free_ptr, objects[i].ngrans * MTE_GRANULE_SIZE);
      mark_range_free(page_idx, objects[i].start_granule, objects[i].ngrans);

      objects[i].active = 0;
      pages[page_idx].live_objects--;

      if (pages[page_idx].live_objects == 0) {
        if (pgclearpresent(pages[page_idx].vpa) <0)
          return -1;
        pages[page_idx].active = 0;
      }

      return 0;
    }
  }
  return -1;
}

int
main(void)
{
  void *keeper = vpa_object_malloc(16);
  void *old = 0;
  void *p;

  if(keeper == 0){
    printf("keeper allocation failed\n");
    exit(1);
  }

  printf("keeper=%p\n", keeper);

  *(volatile char *)keeper = 1;
  printf("keeper write ok\n");

  for(int i = 0; i < 18; i++){
    p = vpa_object_malloc(32);
    if(p == 0){
      printf("alloc failed at iter=%d\n", i);
      exit(1);
    }

    printf("iter=%d p=%p\n", i, p);

    *(volatile char *)p = i;
    *(volatile char *)((uint64)p + 31) = i;
    printf("iter=%d write ok\n", i);

    if(i == 0)
      old = p;

    if(vpa_object_free(p) < 0){
      printf("free failed at iter=%d\n", i);
      exit(1);
    }

    printf("iter=%d freed\n", i);

    *(volatile char *)keeper = i;
    printf("iter=%d keeper still ok\n", i);
  }

  printf("old pointer should MTE fault now: %p\n", old);
  *(volatile char *)old = 99;

  printf("ERROR: old pointer did not fault\n");
  exit(1);
}
