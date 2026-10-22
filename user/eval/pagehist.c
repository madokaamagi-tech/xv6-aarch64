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

#define PAGE_TAG_COUNT 16
#define PAGE_HISTORY_FULL 0xffffU
#define HISTORY_GENERATIONS (PAGE_TAG_COUNT + 1)
#define HISTORY_PROBES (PAGE_TAG_COUNT * (PAGE_TAG_COUNT + 1) / 2)

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

// small-object pages use this cyclic page-tag counter
// large backing ranges select unused page tags from their own history
static int next_page_tag = 1;


// 3. Forward declarations
static uint64 mte_untagged_addr(uint64 ptr);
static void *mte_tagged_ptr(void *ptr, int object_tag);
static void mte_store_tag_range(void *tagged_ptr, uint64 nbytes);
static int find_free_object_meta(void);
static int object_tag_used_for_range(int page_idx, int start, int ngrans, int tag);


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

// Reuse an inactive metadata slot, but obtain new memory with sbrk()
static int
alloc_vpa_page(void)
{
  for (int i = 0; i < MAX_VPA_PAGES; i++) {
    struct vpa_page_meta *page = &pages[i];

    if (page->active)
      continue;

    void *before = sbrk(0);
    if (before == (void *)-1)
      return -1;

    void *raw = vpa_sbrk_pages(1);
    if (raw == 0)
      return -1;

    int page_tag = next_page_tag++;
    if (next_page_tag > 15)
      next_page_tag = 1;

    uint64 vpa = vpamap(raw, page_tag);
    if (vpa == (uint64)-1) {
      vpa_rollback_pages((uint64)before, raw, 1);
      return -1;
    }
    // clear allocation tags left by a previous physical-page owner
    void *free_ptr = mte_tagged_ptr((void *)vpa, FREE_OBJECT_TAG);
    mte_store_tag_range(free_ptr, PGSIZE);

    page->raw = raw;
    page->vpa = (void *)vpa;
    page->page_tag = page_tag;
    page->live_objects = 0;

    for (int w = 0; w < GRANULE_BITMAP_WORDS; w++)
      pages[i].used_granules[w] = 0;

    for (int g = 0; g < MAX_GRANULES; g++)
      pages[i].used_object_tags[g] = 0;

    page->active = 1;

    return i;
  }

  return -1;
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

// 12. Check if page reusement policy works corectly
static int
random_unused_page_tag(uint16 history)
{
  int candidates[PAGE_TAG_COUNT], count = 0;
  for (int tag = 0; tag < PAGE_TAG_COUNT; tag++)
    if ((history & (1U << tag)) == 0)
      candidates[count++] = tag;

  if (count == 0)
    return -1;
  return candidates[tag_random_below(count)];
}

static void
history_trial(uint32 seed, int last, int victim, int fixed_object)
{
  tag_rng_seed(seed);
  int shared_object_tag = 1 + tag_random_below(15);
  void *raw[2] = {vpa_sbrk_pages(1), 0};
  uint16 history[2] = {0, 0};
  void *saved[HISTORY_GENERATIONS];
  uint64 current_vpa = 0;
  int range = 0;
  
  if (raw[0] == 0)
    exit(2);

  for (int generation = 0; generation <= last; generation++) {
    if (generation > 0) {
      void *freed = mte_tagged_ptr((void *)current_vpa, FREE_OBJECT_TAG);
      mte_store_tag_range(freed, MTE_GRANULE_SIZE);
      if (vpaunmap((void *)current_vpa, 1) < 0)
        exit(2);
    }

    int page_tag = random_unused_page_tag(history[range]);
    if (page_tag < 0) {
      if (generation != PAGE_TAG_COUNT || range != 0 || history[0] != PAGE_HISTORY_FULL)
        exit(3);

      range = 1;
      raw[1] = vpa_sbrk_pages(1);
      if (raw[1] == 0)
        exit(2);
      if (raw[1] == raw[0])
        exit(3);
      page_tag = random_unused_page_tag(history[1]);
    }

    if (page_tag < 0 || page_tag >= PAGE_TAG_COUNT || (history[range] & (1U << page_tag)))
      exit(3);

    current_vpa = vpamap(raw[range], page_tag);
    if (current_vpa == (uint64)-1)
      exit(2);
    history[range] |= 1U << page_tag;

    int object_tag = fixed_object ? shared_object_tag : 1 + tag_random_below(15);
    void *current = mte_tagged_ptr((void *)current_vpa, object_tag);
    mte_store_tag_range(current, MTE_GRANULE_SIZE);
    *(volatile uchar *)current = 0x5a;
    if (*(volatile uchar *)current != 0x5a)
      exit(3);
    saved[generation] = current;
  }
    
  if (last == PAGE_TAG_COUNT - 1 && history[0] != PAGE_HISTORY_FULL)
    exit(3);
  if(last == PAGE_TAG_COUNT && (range != 1 || history[0] != PAGE_HISTORY_FULL))
    exit(3);

  if (victim < 0)
    exit(0);

  if (mtequiet(1) < 0)
    exit(2);

  uchar value = *(volatile uchar *)saved[victim];
  (void)value;
  exit(0);
}

static int
history_case(uint32 seed, int last, int victim, int fixed_object)
{
  int pid = fork();
  if(pid < 0)
    return 2;
  if(pid == 0)
    history_trial(seed, last, victim, fixed_object);

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
  int sweeps = argc > 1 ? atoi(argv[1]) : 1;
  uint32 first_seed = argc > 2 ? (uint32)atoi(argv[2]) : 12345U;
  int fixed_object = argc > 3 ? atoi(argv[3]) : 0;
  if (sweeps < 1 || sweeps > 100 || (fixed_object != 0 && fixed_object != 1)) {
    printf("usage: pagehist [sweeps:1..100] [seed] [fixed_object:0|1]\n");
    exit(1);
  }

  int controls = 0, blocked = 0, escaped = 0;
  int mte_faults = 0, setup_failed = 0, policy_failed = 0, unexpected = 0;

  for(int sweep = 0; sweep < sweeps; sweep++){
    uint32 seed = first_seed + (uint32)sweep;
    //printf("sweep=%d seed=%x fixed_object=%d\n", sweep, seed, fixed_object);

    int status = history_case(seed, PAGE_TAG_COUNT, -1, fixed_object);
    if(status == 0)
      controls++;
    else if(status == 2)
      setup_failed++;
    else if(status == 3)
      policy_failed++;
    else
      unexpected++;
    for(int last = 1; last < HISTORY_GENERATIONS; last++){
      for(int victim = 0; victim < last; victim++){
        status = history_case(seed, last, victim, fixed_object);
        if(status == TRANSLATION_FAULT_EXIT_STATUS)
          blocked++;
        else if(status == 0)
          escaped++;
        else if(status == MTE_FAULT_EXIT_STATUS)
          mte_faults++;
        else if(status == 2)
          setup_failed++;
        else if(status == 3)
          policy_failed++;
        else
          unexpected++;

        if(status != TRANSLATION_FAULT_EXIT_STATUS)
          printf("FAIL seed=%x last=%d victim=%d status=%d\n",
                 seed, last, victim, status);
      }
    }
  }

  printf("exhaustion_controls=%d expected=%d\n", controls, sweeps);
  printf("stale_probes=%d translation_faults=%d escaped=%d mte_faults=%d\n",
         HISTORY_PROBES * sweeps, blocked, escaped, mte_faults);
  printf("setup_failures=%d policy_failures=%d unexpected=%d\n",
         setup_failed, policy_failed, unexpected);

  if(controls != sweeps || blocked != HISTORY_PROBES * sweeps || escaped ||
     mte_faults || setup_failed || policy_failed || unexpected){
    printf("FAIL: page-history evaluation\n");
    exit(1);
  }
  printf("PASS: page history, stale blocking, and exhaustion\n");
  exit(0);
}
