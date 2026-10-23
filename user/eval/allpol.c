// VPA + PTE_V + MTE prototype allocator
//
// object granular validity :
//    ARM MTE logical / allocation tags, 16-byte granularity
// page granular validity :
//    VPA alias + PTE_V clear


#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/syscall.h"
#include "user/user.h"


// 1. Constants
#define PGSIZE 4096
#define MTE_GRANULE_SIZE 16                       // MTE uses one allocation tag per 16-byte granule
#define MTE_TAG_SHIFT 56                          // logical tag occupies bits [59:56]
#define MTE_TAG_MASK (0xfULL << MTE_TAG_SHIFT)

#define MAX_VPA_PAGES 4
#define MAX_OBJECTS 256                           // the number of live objects tracked by the allocator
#define MAX_LARGE_RANGES 256
#define MAX_GRANULES (PGSIZE / MTE_GRANULE_SIZE)  // one 4096-byte page contains 256 granules
#define GRANULE_BITMAP_WORDS (MAX_GRANULES / 64)
#define FREE_OBJECT_TAG 0

#define VPA_RAW_LIMIT (1ULL << 26)                // specifies the raw address limit (=VPA_TAG_STRIDE)
#define LARGE_OBJECT_THRESHOLD PGSIZE
#define USED_PAGE_TAGS_MASK 0xfffeU               // setting this mask prevents further reuse of a backing range


// 2. Metadata
struct vpa_page_meta {
  void *raw;          // page-aligned backing address within the sbrk() allocation
  void *vpa;          // alias mapping to the same physical page
  int page_tag;       // page-granular tag, selects the VPA region
  int active;
  int live_objects;
  int pending_map;    // backing memory without VPA mapping
  uint64 used_granules[GRANULE_BITMAP_WORDS];  // current occupancy
  uint64 used_object_tags[MAX_GRANULES];       // tag history, preserved when objects are freed
};

// describes one live allocation
struct object_meta {
  int page_idx;
  int start_granule;
  int ngrans;
  uint64 nbytes;
  int object_tag;     // MTE logical tag
  int active;
  void *ptr;          // tagged pointer returned to the caller
  int is_large;
  uint64 npages;
  void *large_raw;
  void *large_vpa;
  int large_page_tag;
  int large_range_idx;
  int large_free_retagged;
  uint64 large_free_next_page;
};

// remembers backing memory and its page-tag history across successive allocations
struct large_range_meta {
  void *raw;
  uint64 npages;
  uint16 used_page_tags;
  int active;
  void *tail_vpa;     // previous allocation's partially used final-page alias
};

static struct vpa_page_meta pages[MAX_VPA_PAGES];
static struct object_meta objects[MAX_OBJECTS];
static struct large_range_meta large_ranges[MAX_LARGE_RANGES];

// 3. Forward declarations
static uint64 mte_untagged_addr(uint64 ptr);
static void *mte_tagged_ptr(void *ptr, int object_tag);
static void mte_store_tag_range(void *tagged_ptr, uint64 nbytes);
static int find_free_object_meta(void);
static int object_tag_used_for_range(int page_idx, int start, int ngrans, int tag);
static int alloc_vpa_page(void);


// 4. MTE pointer and allocation-tag operations

// Remove the MTE logical tag while preserving the VPA address.
static uint64
mte_untagged_addr(uint64 ptr)
{
  return ptr & ~MTE_TAG_MASK;
}

// Construct a pointer carrying the requested MTE logical tag.
static void *
mte_tagged_ptr(void *ptr, int object_tag)
{
  uint64 tag_bits =
    ((uint64)object_tag & 0xf) << MTE_TAG_SHIFT;

  return (void *)(mte_untagged_addr((uint64)ptr) | tag_bits);
}

// Copy the pointer's logical tag into the addressed granule's allocation tag.
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

// Apply the same allocation tag to every granule covering the range.
static void
mte_store_tag_range(void *tagged_ptr, uint64 nbytes)
{
  uint64 size =
    (nbytes + MTE_GRANULE_SIZE - 1) & ~((uint64)MTE_GRANULE_SIZE - 1);
  for (uint64 off = 0; off < size; off += MTE_GRANULE_SIZE)
    mte_store_tag((void *)((uint64)tagged_ptr + off));
}


// 5. Granule occupancy management

// Locate granule gran within page's occupancy bitmap.
static int
granule_used(int page_idx, int gran)
{
  return (pages[page_idx].used_granules[gran / 64] & (1ULL << (gran % 64))) != 0;
}

// Set the index in used_granules.
static void
set_granule_used(int page_idx, int gran)
{
  pages[page_idx].used_granules[gran / 64] |= 1ULL << (gran % 64);
}

// Clear the index in used_granules.
static void
clear_granule_used(int page_idx, int gran)
{
  pages[page_idx].used_granules[gran / 64] &= ~(1ULL << (gran % 64));
}

// Check current occupancy only.
static int range_is_free(int page_idx, int start, int ngrans) {
  if(start + ngrans > MAX_GRANULES)
    return 0;

  for(int i = 0; i < ngrans; i++){
    if(granule_used(page_idx, start + i))
      return 0;
  }

  return 1;
}

static void
mark_range_used(int page_idx, int start, int ngrans)
{
  for(int i = 0; i < ngrans; i++)
    set_granule_used(page_idx, start + i);
}

// Release occupancy without clearing object-tag history.
static void
mark_range_free(int page_idx, int start, int ngrans)
{
  for(int i = 0; i < ngrans; i++)
    clear_granule_used(page_idx, start + i);
}

static int
roundup_granules(uint64 nbytes)
{
  return (nbytes + MTE_GRANULE_SIZE - 1) / MTE_GRANULE_SIZE;
}


// 6. Object-tag allocation and history

static uint64 tag_rng_state;

static uint32
tag_random_u32(void)
{
  uint64 previous = tag_rng_state;

  tag_rng_state = previous * 6364136223846793005ULL + 109ULL;

  uint32 mixed = (uint32)((previous ^ (previous >> 18)) >> 27);
  uint32 rotation = (uint32)(previous >> 59);

  if (rotation == 0)
    return mixed;

  return (mixed >> rotation) | (mixed << (32U - rotation));
}

static void
tag_rng_seed(uint32 seed)
{
  tag_rng_state = 0;
  (void)tag_random_u32();

  tag_rng_state += seed;
  (void)tag_random_u32();
}

static uint32
tag_random_below(uint32 bound)
{
  uint32 threshold = (uint32)((1ULL << 32) % bound);

  for (;;) {
    uint32 value = tag_random_u32();

    if (value >= threshold)
      return value % bound;
  }
}

// Check adjacent allocation boundaries in the VPA address space.
static int
adjacent_object_uses_tag(uint64 start, uint64 nbytes, int tag)
{
  uint64 size = (nbytes + MTE_GRANULE_SIZE - 1) & ~((uint64)MTE_GRANULE_SIZE - 1);
  uint64 end = start + size;

  for (int i = 0; i < MAX_OBJECTS; i++) {
    struct object_meta *object = &objects[i];

    if (!object->active || object->object_tag != tag)
      continue;

    uint64 other_start = mte_untagged_addr((uint64)object->ptr);
    uint64 other_size = (object->nbytes + MTE_GRANULE_SIZE - 1) & ~((uint64)MTE_GRANULE_SIZE - 1);

    if (other_start + other_size == start || end == other_start)
      return 1;
  }

  return 0;
}

// Convert a small-page granule range into its VPA byte range.
static int
neighbor_uses_tag(int page_idx, int start, int ngrans, int tag)
{
  uint64 vpa = (uint64)pages[page_idx].vpa + (uint64)start * MTE_GRANULE_SIZE;

  return adjacent_object_uses_tag(vpa, (uint64)ngrans * MTE_GRANULE_SIZE, tag);
}


// Choose pseudorandomly among tags allowed by history and neighbors.
static int
alloc_object_tag_for_range(int page_idx, int start, int ngrans)
{
  int candidates[15];
  int count = 0;

  for (int tag = 1; tag <= 15; tag++) {
    if (object_tag_used_for_range(page_idx, start, ngrans, tag))
      continue;

    if (neighbor_uses_tag(page_idx, start, ngrans, tag))
      continue;

    candidates[count++] = tag;
  }

  if (count == 0)
    return -1;

  return candidates[tag_random_below((uint32)count)];
}

// Choose pseudorandomly among tags different from live neighbors.
static int
alloc_large_object_tag(uint64 vpa, uint64 nbytes)
{
  int candidates[15];
  int count = 0;

  for (int tag = 1; tag <= 15; tag++) {
    if (!adjacent_object_uses_tag(vpa, nbytes, tag))
      candidates[count++] = tag;
  }

  return candidates[tag_random_below((uint32)count)];
}


// Convert a tag value into its history-bit position to create a mask
static int
object_tag_bit(int tag)
{
  return 1U << tag;
}

// A tag is unavailable if any granule in the range used it before.
static int
object_tag_used_for_range(int page_idx, int start, int ngrans, int tag)
{
  uint16 bit = object_tag_bit(tag);

  for (int i = 0; i < ngrans; i++) {
    if (pages[page_idx].used_object_tags[start + i] & bit)
      return 1;
  }

  return 0;
}

// Remember the chosen tag on every granule belonging to the object.
static void
mark_object_tag_used_for_range(int page_idx, int start, int ngrans, int tag)
{
  uint16 bit = object_tag_bit(tag);

  for(int i = 0; i < ngrans; i++)
    pages[page_idx].used_object_tags[start + i] |= bit;
}

// 7. Find a free range with an available object tag.
// ※ A usable range needs both :
//  - enough currently free granules;
//  - an object tag unused on every granule in that range;

// Find a free range and an available object tag.
// Return the starting granule and write the selected tag to tag_out.
static int
find_free_range_with_tag(int page_idx, int ngrans, int  *tag_out)
{
  for (int start = 0; start <= MAX_GRANULES - ngrans; start++) {
    if (!range_is_free(page_idx, start, ngrans))
      continue;

    int tag = alloc_object_tag_for_range(page_idx, start, ngrans);

    if (tag >= 0) {
      *tag_out = tag;
      return start;
    }
  }

  return -1;
}


// 8. Page and object metadata allocation

// Obtain page-aligned backing memory that fits inside the raw window.
// Return zero without growing the heap if the request cannot fit.
static void *
vpa_sbrk_pages(uint64 npages)
{
  void *current = sbrk(0);

  if (current == (void *)-1)
    return 0;

  uint64 base = (uint64)current;

  if (npages == 0 || base >= VPA_RAW_LIMIT)
    return 0;

  // make sure that sbrk() is aligned to PGSIZE
  uint64 padding = (PGSIZE - base % PGSIZE) % PGSIZE;
  uint64 available = VPA_RAW_LIMIT - base;

  if (padding > available || npages > (available - padding) / PGSIZE)
    return 0;

  uint64 total = padding + npages * PGSIZE;
  void *raw = sbrk((int)total);

  if (raw == (void *)-1)
    return 0;

  return (void *)((uint64)raw + padding);
}

static int
find_free_object_meta(void)
{
  for (int i = 0; i < MAX_OBJECTS; i++) {
    if (!objects[i].active)
      return i;
  }

  return -1;
}

// Undo a fresh, unpublished allocation at the current heap end.
static void
vpa_rollback_pages(uint64 old_break, void *raw, uint64 npages)
{
  uint64 end = (uint64)raw + npages * PGSIZE;
  void *current = sbrk(0);

  if (current == (void *)-1 || (uint64)current != end || old_break >= end || end - old_break > 0x7fffffffULL) {
//    printf("rollback: backing is not the latest heap growth\n");
    exit(1);
  }

  if (sbrk(-(int)(end - old_break)) == (void *)-1) {
//   printf("rollback: sbrk shrink failed\n");
    exit(1);
  }
}


// 9. Large-object allocation

// Never wrap to an old page tag on the same backing range.
static int unused_large_page_tag(uint16 used_tags)
{
  for (int tag = 1; tag <= 15; tag++) {
    if ((used_tags & object_tag_bit(tag)) == 0)
      return tag;
  }

  return -1;
}

// Find reusable backing memory with the same rounded page count
static int
find_large_range(uint64 npages, uint64 *rollback_break)
{
  *rollback_break = (uint64)-1;
  int spare = -1;

  for (int i = 0; i < MAX_LARGE_RANGES; i++) {
    struct large_range_meta *range = &large_ranges[i];

    if (range->active)
      continue;

    if (range->raw && range->npages == npages && unused_large_page_tag(range->used_page_tags) > 0)
      return i;

    if (spare < 0 || range->raw == 0)
      spare = i;
  }

  if (spare < 0)
    return -1;

  void *before = sbrk(0);
  if (before == (void *)-1)
    return -1;

  void *raw = vpa_sbrk_pages(npages);

  if (raw == 0)
    return -1;

  large_ranges[spare] = (struct large_range_meta){
    .raw = raw,
    .npages = npages,
  };

  *rollback_break = (uint64)before;

  return spare;
}

// Allocate a large object using a fresh or reusable backing range.
static void *
vpa_large_object_malloc(uint nbytes)
{
  int obj_idx = find_free_object_meta();

  if (obj_idx < 0)
    return 0;

  uint64 npages = ((uint64)nbytes + PGSIZE - 1) / PGSIZE;

  uint64 rollback_break;
  int range_idx = find_large_range(npages, &rollback_break);
  if (range_idx < 0)
    return 0;

  struct large_range_meta *range = &large_ranges[range_idx];
  void *raw = range->raw;

  // If the previous allocation's partial tail is still mapped,
  // invalidate that old alias before changing the allocation tag
  if (range->tail_vpa) {
    if (pgclearpresent(range->tail_vpa) < 0) {
      range->used_page_tags = USED_PAGE_TAGS_MASK;
      return 0;
    }

    range->tail_vpa = 0;
  }
  int page_tag = unused_large_page_tag(range->used_page_tags);

  // Consume the tag before mapping
  range->used_page_tags |= object_tag_bit(page_tag);

  uint64 first_vpa = 0;

  for (uint64 i = 0; i < npages; i++) {
    void *raw_page = (void *)((uint64)raw + i * PGSIZE);
    uint64 vpa = vpamap(raw_page, page_tag);

    if (vpa == (uint64)-1) {
      if (i > 0 && vpaunmap((void *)first_vpa, (int)i) < 0) {
//        printf("rollback: VPA unmap failed\n");
        exit(1);
      }

      if (rollback_break != (uint64)-1) {
        vpa_rollback_pages(rollback_break, raw, npages);
        *range = (struct large_range_meta) {0};
      }
      return 0;
    }

    if (i == 0)
      first_vpa = vpa;
  }

  // fresh backing needs known tags across all pages
  if (rollback_break != (uint64)-1) {
    void *free_ptr = mte_tagged_ptr((void *)first_vpa, FREE_OBJECT_TAG);
    mte_store_tag_range(free_ptr, npages * PGSIZE);
  }

  int object_tag = alloc_large_object_tag(first_vpa, nbytes);

  void *ptr = mte_tagged_ptr((void *)first_vpa, object_tag);

  mte_store_tag_range(ptr, nbytes);

  objects[obj_idx].active = 1;
  objects[obj_idx].ptr = ptr;
  objects[obj_idx].nbytes = nbytes;
  objects[obj_idx].object_tag = object_tag;

  objects[obj_idx].is_large = 1;
  objects[obj_idx].npages = npages;
  objects[obj_idx].large_raw = raw;
  objects[obj_idx].large_vpa = (void *)first_vpa;
  objects[obj_idx].large_page_tag = page_tag;
  objects[obj_idx].large_range_idx = range_idx;
  range->active = 1;
  objects[obj_idx].large_free_retagged = 0;
  objects[obj_idx].large_free_next_page = 0;

  objects[obj_idx].page_idx = -1;
  objects[obj_idx].start_granule = -1;
  objects[obj_idx].ngrans = 0;

  return ptr;
}


// 10. Small-object allocation and allocation entry point

static __attribute__((unused)) void *
vpa_object_malloc(uint nbytes)
{
  if (nbytes == 0)
    return 0;

  if(nbytes > LARGE_OBJECT_THRESHOLD)
  return vpa_large_object_malloc(nbytes);

  int obj_idx = find_free_object_meta();

  if (obj_idx < 0)
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

    if (start_granule >= 0) {
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

  mark_object_tag_used_for_range(page_idx, start_granule, ngrans, object_tag);

  mark_range_used(page_idx, start_granule, ngrans);
  pages[page_idx].live_objects++;

  uint64 vpa_addr = (uint64)pages[page_idx].vpa +
                      start_granule * MTE_GRANULE_SIZE;

  void *ptr = mte_tagged_ptr((void *)vpa_addr, object_tag);

  mte_store_tag_range(ptr, ngrans * MTE_GRANULE_SIZE);

  objects[obj_idx].page_idx = page_idx;
  objects[obj_idx].start_granule = start_granule;
  objects[obj_idx].ngrans = ngrans;
  objects[obj_idx].object_tag = object_tag;
  objects[obj_idx].active = 1;
  objects[obj_idx].ptr = ptr;
  objects[obj_idx].nbytes = nbytes;

  objects[obj_idx].is_large = 0;
  objects[obj_idx].npages = 0;
  objects[obj_idx].large_raw = 0;
  objects[obj_idx].large_vpa = 0;
  objects[obj_idx].large_page_tag = 0;
  objects[obj_idx].large_range_idx = -1;
  objects[obj_idx].large_free_retagged = 0;
  objects[obj_idx].large_free_next_page = 0;

  return ptr;
}


// 11. Object free and page invalidation
static __attribute__((unused)) int
vpa_object_free(void *ptr)
{
  for (int i = 0; i < MAX_OBJECTS; i++) {
    if (!objects[i].active || objects[i].ptr != ptr)
      continue;

    // 11.1 Large object : invalidate all of the dedicated pages
    if (objects[i].is_large) {
      struct object_meta *object = &objects[i];
      struct large_range_meta *range = &large_ranges[object->large_range_idx];

      uint64 base = mte_untagged_addr((uint64)object->large_vpa);

      uint64 full_pages = object->nbytes / PGSIZE;

      if (!object->large_free_retagged) {
        void *free_ptr = mte_tagged_ptr((void *)base, FREE_OBJECT_TAG);

        mte_store_tag_range(free_ptr, object->nbytes);
        object->large_free_retagged = 1;
      }

      while (object->large_free_next_page < full_pages) {
        uint64 p = object->large_free_next_page;
        void *vpa_page = (void *)(base + p * PGSIZE);

        if (pgclearpresent(vpa_page) < 0) {
          return -1;
        }

        object->large_free_next_page++;
      }

      range->tail_vpa = object->nbytes % PGSIZE ? (void *)(base + full_pages * PGSIZE) : 0;

      range->active = 0;
      object->active = 0;

      return 0;
    }

    // 11.2 Small object : retag while the page remains mapped
    int page_idx = objects[i].page_idx;

    void *free_ptr = mte_tagged_ptr(ptr, FREE_OBJECT_TAG);

    mte_store_tag_range(free_ptr, objects[i].ngrans * MTE_GRANULE_SIZE);

    int last_object = pages[page_idx].live_objects == 1;
    if (last_object && pgclearpresent(pages[page_idx].vpa) < 0)
      return -1;

    mark_range_free(page_idx, objects[i].start_granule, objects[i].ngrans);

    objects[i].active = 0;
    pages[page_idx].live_objects--;

    if (last_object)
      pages[page_idx].active = 0;

    return 0;
  }

  return -1;
}

// 12. Test MTE tag collision
static uint16 page_history[MAX_VPA_PAGES];
static int retired_pages;

static int 
unused_page_tag(uint16 history)
{
  int candidates[15], count = 0;
  for (int tag = 1; tag <= 15; tag++)
    if ((history & (1U << tag)) == 0)
      candidates[count++] = tag;
  if (count == 0)
    return -1;
  return candidates[tag_random_below(count)];
}

static int
alloc_vpa_page(void)
{
  for (int i = 0; i < MAX_VPA_PAGES; i++){
    struct vpa_page_meta *page = &pages[i];
    if (page->active)
      continue;

    void *raw = page->raw;
    uint16 history = page_history[i];
    int page_tag = unused_page_tag(history);
    int fresh = raw == 0 || page_tag < 0;
    int retiring = raw != 0 && page_tag < 0;
    uint64 old_break = 0;

    if (fresh){
      void *before = sbrk(0);
      if(before == (void *)-1)
        return -1;
      old_break = (uint64)before;
      raw = vpa_sbrk_pages(1);
      if (raw == 0)
        return -1;
      history = 0;
      page_tag = unused_page_tag(history);
    }

    uint64 vpa = vpamap(raw, page_tag);
    if (vpa == (uint64)-1) {
      if (fresh)
        vpa_rollback_pages(old_break, raw, 1);
      return -1;
    }

    page_history[i] = history | (1U << page_tag);
    if (retiring)
      retired_pages++;

    *page = (struct vpa_page_meta) {
      .raw = raw,
      .vpa = (void *)vpa,
      .page_tag = page_tag,
      .active = 1,
    };

    mte_store_tag_range(mte_tagged_ptr((void *)vpa, FREE_OBJECT_TAG), PGSIZE);
    return i;
  }
  return -1;
}

static void
allpol_check(int condition)
{
  if (!condition)
    exit(3);
}

static struct object_meta *
allpol_meta(void *ptr)
{
  for (int i = 0; i < MAX_OBJECTS; i++)
    if (objects[i].active && objects[i].ptr == ptr)
      return &objects[i];
  exit(3);
}

static int
allpol_tag(void *ptr)
{
  return ((uint64)ptr >> MTE_TAG_SHIFT) & 15;
}

static void
allpol_touch(void *ptr, uint nbytes)
{
  volatile uchar *p = ptr;
  p[0] = 0x5a;
  p[nbytes - 1] = 0xa5;
  allpol_check(p[0] == 0x5a && p[nbytes - 1] == 0xa5);
}

static void *
allpol_alloc(uint nbytes)
{
  void *p = vpa_object_malloc(nbytes);
  if (p == 0)
    exit(2);
  allpol_touch(p, nbytes);
  return p;
}

static void
allpol_free(void *ptr)
{ 
  if (vpa_object_free(ptr) < 0)
    exit(2);
}

static void *
allpol_exhaust_hole(void *left, void *first, void *right, uint nbytes)
{
  uint64 hole = mte_untagged_addr((uint64)first);
  int page = allpol_meta(left)->page_idx;
  int start = allpol_meta(first)->start_granule;
  int ngrans = roundup_granules(nbytes);
  int ltag = allpol_tag(left), rtag = allpol_tag(right);
  uint16 forbidden = (1U << ltag) | (1U << rtag);
  uint16 seen = 0;
  int count = 0;
  void *current = first;

  for (int step = 0; step < 16; step++){
    if (mte_untagged_addr((uint64)current) != hole){
      allpol_check(allpol_meta(current)->page_idx == page);
      allpol_check(count == 15 - (ltag == rtag ? 1 : 2));
      allpol_check(seen == (USED_PAGE_TAGS_MASK & ~forbidden));
      for (int g = 0; g < ngrans; g++)
        allpol_check(pages[page].used_object_tags[start + g] == seen);
      allpol_check(allpol_tag(current) != rtag);
      return current;
    }

    int tag = allpol_tag(current);
    allpol_check((seen & (1U << tag)) == 0 && tag != ltag && tag != rtag);
    seen |= 1U << tag;
    count++;
    allpol_free(current);
    for (int g = 0; g < ngrans; g++)
      allpol_check(pages[page].used_object_tags[start + g] == seen);
    allpol_touch(left, nbytes);
    allpol_touch(right, nbytes);
    current = allpol_alloc(nbytes);
  }
  exit(3);
}

static void *
allpol_page_cycles(int page, void *first_raw, int first_tag, uint nbytes)
{
  uint16 seen = 1U << first_tag;
  int old_retired = retired_pages;
  void *current = 0;
  int ngrans = roundup_granules(nbytes);

  for (int generation = 1; generation <= 15; generation++){
    current = allpol_alloc(nbytes);
    allpol_check(allpol_meta(current)->page_idx == page);
    struct vpa_page_meta *p = &pages[page];
    int object_tag = allpol_tag(current);

    if (generation < 15){
      allpol_check(p->raw == first_raw);
      allpol_check((seen & (1U << p->page_tag)) == 0);
      seen |= 1U << p->page_tag;
      allpol_check(page_history[page] == seen);
      allpol_check(retired_pages == old_retired);
    } else {
      allpol_check(seen == USED_PAGE_TAGS_MASK && p->raw != first_raw);
      allpol_check(page_history[page] == (1U << p->page_tag));
      allpol_check(retired_pages == old_retired + 1);
    }

    // Only the first object's tag may appear in this new generation.
    for (int g = 0; g < MAX_GRANULES; g++)
      allpol_check(p->used_object_tags[g] == (g < ngrans ? (1U << object_tag) : 0));
    if (generation < 15)
      allpol_free(current);
  }
  return current;
}

static void
allpol_trial(uint32 seed, uint nbytes, int which)
{
  tag_rng_seed(seed);
  uint size = roundup_granules(nbytes) * MTE_GRANULE_SIZE;
  void *left = allpol_alloc(nbytes);
  void *hole = allpol_alloc(nbytes);
  void *right = allpol_alloc(nbytes);
  int page = allpol_meta(left)->page_idx;
  void *first_raw = pages[page].raw;
  int first_page_tag = pages[page].page_tag;
  uint64 old_vpa = (uint64)pages[page].vpa;

  allpol_check(allpol_meta(hole)->page_idx == page && allpol_meta(right)->page_idx == page);
  allpol_check(mte_untagged_addr((uint64)hole) == mte_untagged_addr((uint64)left) + size);
  allpol_check(mte_untagged_addr((uint64)right) == mte_untagged_addr((uint64)hole) + size);
  allpol_check(allpol_tag(left) != allpol_tag(hole) && allpol_tag(right) != allpol_tag(hole));

  uint64 probe = 0;
  if(which == 1) probe = (uint64)left + size;
  if(which == 2) probe = (uint64)hole - 1;
  if(which == 3) probe = (uint64)hole + size;
  if(which == 4) probe = (uint64)right - 1;

  if(which == 5 || which == 6 || which == 7){
    void *stale = hole;
    allpol_free(hole);
    allpol_touch(left, nbytes);
    allpol_touch(right, nbytes);
    if(which != 6){
      hole = allpol_alloc(nbytes);
      allpol_check(mte_untagged_addr((uint64)hole) == mte_untagged_addr((uint64)stale));
      allpol_check(allpol_tag(hole) != allpol_tag(stale));
      allpol_check(allpol_tag(hole) != allpol_tag(left) && allpol_tag(hole) != allpol_tag(right));
    }
    probe = which == 5 ? (uint64)hole + size : (uint64)stale;
  }

  if(which == 0 || which == 8 || which == 11){
    void *stale = hole;
    hole = allpol_exhaust_hole(left, hole, right, nbytes);
    if(which == 8)
      probe = (uint64)stale;
  }

  if(which == 0 || which == 9 || which == 10 || which == 11){
    allpol_free(hole);
    allpol_free(right);
    allpol_free(left);
    allpol_check(!pages[page].active);
    if(which == 0 || which == 11)
      (void)allpol_page_cycles(page, first_raw, first_page_tag, nbytes);
    if(which == 10){
      void *current = allpol_alloc(nbytes);
      allpol_check(allpol_meta(current)->page_idx == page);
      allpol_check(pages[page].raw == first_raw && (uint64)pages[page].vpa != old_vpa);
      allpol_check(pages[page].page_tag != first_page_tag);
      for(int g = 0; g < MAX_GRANULES; g++)
        allpol_check(pages[page].used_object_tags[g] ==
          (g < roundup_granules(nbytes) ? (1U << allpol_tag(current)) : 0));
    }
    probe = (uint64)left;
  }

  if(which == 0)
    exit(0);
  if(which == 12) probe = (uint64)left + nbytes; // Padding control.
  if(which == 13) probe = (uint64)left + 2 * size; // Skip the middle object.

  // A nonadjacent equal-tag read, or padding read, must succeed.
  int must_succeed = which == 12 ||
                    (which == 13 && allpol_tag(left) == allpol_tag(right));
  if(!must_succeed && mtequiet(1) < 0)
    exit(2);
  uchar value = *(volatile uchar *)probe;
  (void)value;
  if(!must_succeed)
    exit(3);
  exit(0);
}

static int
allpol_case(uint32 seed, uint nbytes, int which)
{
  int pid = fork();
  if(pid < 0)
    return 2;
  if(pid == 0)
    allpol_trial(seed, nbytes, which);
  int status = 99;
  if(wait(&status) != pid){
    printf("FAIL: wait\n");
    exit(1);
  }
  return status;
}

int
main(int argc, char **argv)
{
  int sweeps = argc > 1 ? atoi(argv[1]) : 10;
  uint32 first_seed = argc > 2 ? (uint32)atoi(argv[2]) : 23456U;
  uint nbytes = argc > 3 ? (uint)atoi(argv[3]) : 16;
  if(sweeps < 1 || sweeps > 1000 || nbytes < 2 || nbytes > 128){
    printf("usage: allpol [sweeps:1..1000] [seed] [bytes:2..128]\n");
    exit(1);
  }
  char *names[14] = {
    "live_and_exhaustion", "left_to_hole", "hole_to_left", "hole_to_right",
    "right_to_hole", "refilled_hole_to_right", "stale_after_free",
    "stale_after_reuse", "stale_after_object_exhaustion", "stale_after_page_free",
    "stale_after_page_reuse", "stale_after_page_exhaustion", "padding", "skip_middle",
  };
  int completed[14] = {0}, mte[14] = {0}, translation[14] = {0};
  int setup_failed = 0, check_failed = 0, unexpected = 0;
  printf("ALLPOL sweeps=%d first_seed=%x bytes=%d\n", sweeps, first_seed, nbytes);

  for(int which = 0; which < 14; which++){
    if(which == 12 && nbytes % MTE_GRANULE_SIZE == 0)
      continue;
    for(int sweep = 0; sweep < sweeps; sweep++){
      uint32 seed = first_seed + (uint32)sweep;
      int status = allpol_case(seed, nbytes, which);
      if(status == 0) completed[which]++;
      else if(status == MTE_FAULT_EXIT_STATUS) mte[which]++;
      else if(status == TRANSLATION_FAULT_EXIT_STATUS) translation[which]++;
      else if(status == 2) setup_failed++;
      else if(status == 3) check_failed++;
      else unexpected++;
      int expected = which == 0 || which == 12 ? 0 :
                     which >= 9 && which <= 11 ? TRANSLATION_FAULT_EXIT_STATUS :
                     MTE_FAULT_EXIT_STATUS;
      if(which == 13 && (status == 0 || status == MTE_FAULT_EXIT_STATUS))
        continue;
      if(status != expected){
        printf("FAIL case=%d seed=%x expected=%d status=%d\n", which, seed, expected, status);
        if(status == 0 || status == MTE_FAULT_EXIT_STATUS ||
           status == TRANSLATION_FAULT_EXIT_STATUS)
          unexpected++;
      }
    }
    printf("%s completed=%d mte_faults=%d translation_faults=%d\n",
           names[which], completed[which], mte[which], translation[which]);
  }
  printf("setup_failures=%d check_failures=%d unexpected=%d\n",
         setup_failed, check_failed, unexpected);
  if(setup_failed || check_failed || unexpected){
    printf("FAIL: combined-policy evaluation\n");
    exit(1);
  }
  printf("PASS: combined-policy evaluation\n");
  exit(0);
}
