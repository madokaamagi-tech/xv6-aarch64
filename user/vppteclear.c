#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

#define PGSIZE 4096
#define MAX_VPA_PAGES 4
#define MAX_OBJECTS 32
#define OBJECT_SIZE 32
#define FREE_OBJECT_TAG 0

#define MTE_GRANULE_SIZE 16
#define MTE_TAG_SHIFT 56
#define MTE_TAG_MASK (0xfULL << MTE_TAG_SHIFT)

struct vpa_page_meta {
  void *raw;         
  void *vpa;         
  int page_tag;      
  int active;
  int live_objects;
  uint64 used_slots;  
};

struct object_meta {
  int page_idx;
  int slot;
  uint64 nbytes;
  int object_tag;     
  int active;
  void *ptr;          
};

static struct vpa_page_meta pages[MAX_VPA_PAGES];
static struct object_meta objects[MAX_OBJECTS];
static int next_page_tag = 1;
static int next_object_tag = 1;
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
  asm volatile(
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

  for(uint64 off = 0; off < size; off += MTE_GRANULE_SIZE)
    mte_store_tag((void *)((uint64)tagged_ptr + off));
}

static int
alloc_object_tag(void)
{
  int tag = next_object_tag++;
  if(next_object_tag > 15)
    next_object_tag = 1;
  return tag;
}

static int
alloc_vpa_page(void)
{
  for(int i = 0; i < MAX_VPA_PAGES; i++){
    if(!pages[i].active){
      void *raw = sbrk(PGSIZE);
      if(raw == (void *)-1)
        return -1;

      int page_tag = next_page_tag++;
      if(next_page_tag > 15)
        next_page_tag = 1;

      uint64 vpa = vpamap(raw, page_tag);
      if(vpa == (uint64)-1)
        return -1;

      pages[i].raw = raw;
      pages[i].vpa = (void *)vpa;
      pages[i].page_tag = page_tag;
      pages[i].active = 1;
      pages[i].live_objects = 0;
      pages[i].used_slots = 0;
      return i;
    }
  }

  return -1;
}

static void *
vpa_object_malloc(uint nbytes)
{
  if(nbytes == 0 || nbytes > OBJECT_SIZE)
    return 0;

  int page_idx = -1;
  int slot = -1;

  // 空いているオブジェクトスロットのあるVPA ページを見つける
  for(int i = 0; i < MAX_VPA_PAGES; i++){
    if(!pages[i].active)
      continue;

    for(int s = 0; s < PGSIZE / OBJECT_SIZE; s++){
      if((pages[i].used_slots & (1ULL << s)) == 0){
        page_idx = i;
        slot = s;
        goto found_slot;
      }
    }
  }

  page_idx = alloc_vpa_page();
  if(page_idx < 0)
    return 0;
  slot = 0;

found_slot:
  int obj_idx = -1;
  for(int i = 0; i < MAX_OBJECTS; i++){
    if(!objects[i].active){
      obj_idx = i;
      break;
    }
  }
  if(obj_idx < 0)
    return 0;

  pages[page_idx].used_slots |= 1ULL << slot;
  pages[page_idx].live_objects++;

  uint64 vpa_addr = (uint64)pages[page_idx].vpa + slot * OBJECT_SIZE;
  int object_tag = alloc_object_tag();
  void *ptr = mte_tagged_ptr((void *)vpa_addr, object_tag);

  mte_store_tag_range(ptr, nbytes);

  objects[obj_idx].page_idx = page_idx;
  objects[obj_idx].slot = slot;
  objects[obj_idx].nbytes = nbytes;
  objects[obj_idx].object_tag = object_tag;
  objects[obj_idx].active = 1;
  objects[obj_idx].ptr = ptr;

  return ptr;
}

// オブジェクトのfreeではPTE_Vを落とさない
static int
vpa_object_free(void *ptr)
{
  for(int i = 0; i < MAX_OBJECTS; i++){
    if(objects[i].active && objects[i].ptr == ptr){
      int page_idx = objects[i].page_idx;
      int slot = objects[i].slot;

      void *free_ptr =
        mte_tagged_ptr((void *)mte_untagged_addr((uint64)ptr),
                       FREE_OBJECT_TAG);
      mte_store_tag_range(free_ptr, objects[i].nbytes);

      objects[i].active = 0;
      pages[page_idx].used_slots &= ~(1ULL << slot);
      pages[page_idx].live_objects--;

      // ページにlive_objectがなくなったらPTE_Vを落とす
      if(pages[page_idx].live_objects == 0){
        if(pgclearpresent(pages[page_idx].vpa) < 0)
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
  void *a = vpa_object_malloc(32);
  void *b = vpa_object_malloc(32);

  printf("a=%p b=%p\n", a, b);

  *(volatile char *)a = 1;
  *(volatile char *)b = 2;
  printf("a/b writes ok\n");

  vpa_object_free(a);
  printf("freed a\n");

  *(volatile char *)b = 3;
  printf("b still works after freeing a\n");

  vpa_object_free(b);
  printf("freed b; page should now have PTE_V cleared\n");

  printf("old b should page fault now: %p\n", b);
  *(volatile char *)b = 4;

  printf("ERROR: old b did not fault\n");
  exit(1);
}
