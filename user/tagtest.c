// VPA + PTE_V + MTE prototype allocator
//
// object granular validity :
//    ARM MTE logical / allocation tags, 16-byte granularity
// page granular validity :
//    VPA alias + PTE_V clear


#include "kernel/types.h"
#include "kernel/stat.h"
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

#define LARGE_OBJECT_THRESHOLD PGSIZE
#define USED_PAGE_TAGS_MASK 0xfffeU               // setting this mask prevents further reuse of a backing range


// 2. Metadata
struct vpa_page_meta {
  void *raw;          // original address returned by sbrk()
  void *vpa;          // alias mapping to the same physical page
  int page_tag;       // page-granular tag, selects the VPA region
  int active;         
  int live_objects;
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

// Choose and record a tag unused across the entire range.
// Return -1 when no tag is available
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

// Large-object MTE tags cycle independently of VPA page tags
//
// When a backing range is reused, it receives an unused page tag.
// Before retagging the memory, any old partial-tail alias is invalidated
// This prevents an old VPA pointer from becoming valid merely because
// its MTE object tag matches again.
static int
alloc_large_object_tag(void)
{
  static int next_large_object_tag = 1;
  int tag = next_large_object_tag++;

  if (next_large_object_tag > 15)
    next_large_object_tag = 1;

  return tag;
}


// 7. Find a free range with an available object tag.
// ※ A usable range needs both : 
//  - enough currently free granules;
//  - an object tag unused on every granule in that range;

// Records the selected tag in the range's history and returns the starting granule index.
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

static int
find_free_object_meta(void)
{
  for (int i = 0; i < MAX_OBJECTS; i++) {
    if (!objects[i].active)
      return i;
  }

  return -1;
}

// Reuse an inactive metadata slot, but obtain new memory with sbrk()
static int
alloc_vpa_page(void)
{
  for (int i = 0; i < MAX_VPA_PAGES; i++) {
    if (pages[i].active)
      continue;

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

    for (int g = 0; g < MAX_GRANULES; g++)
      pages[i].used_object_tags[g] = 0;

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
find_large_range(uint64 npages)
{
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

  if(spare < 0 || npages > 0x7fffffffUL / PGSIZE)
    return -1;

  void *raw = sbrk(npages * PGSIZE);

  if(raw == (void *)-1)
    return -1;  

  large_ranges[spare] = (struct large_range_meta){
    .raw = raw,
    .npages = npages,
  };

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
  
  int range_idx = find_large_range(npages);
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

    if (vpa == (uint64)-1)
      return 0;

    if (i == 0)
      first_vpa = vpa;
  }
  int object_tag = alloc_large_object_tag();

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

  objects[obj_idx].page_idx = -1;
  objects[obj_idx].start_granule = -1;
  objects[obj_idx].ngrans = 0;

  return ptr;
}


// 10. Small-object allocation and allocation entry point

static void *
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

  return ptr;
}


// 11. Object free and page invalidation
static int
vpa_object_free(void *ptr)
{
  for (int i = 0; i < MAX_OBJECTS; i++) {
    if (!objects[i].active || objects[i].ptr != ptr)
      continue;

    // 11.1 Large object : invalidate all of the dedicated pages
    if (objects[i].is_large) {
      uint64 base = mte_untagged_addr((uint64)objects[i].large_vpa);

      void *free_ptr = mte_tagged_ptr((void *)base, FREE_OBJECT_TAG);

      mte_store_tag_range(free_ptr, objects[i].nbytes);
      
      uint64 full_pages = objects[i].nbytes / PGSIZE;
      
      for (uint64 p = 0; p < full_pages; p++) {
        void *vpa_page = (void *)(base + p * PGSIZE);

        if (pgclearpresent(vpa_page) < 0)
          return -1;
      }

      struct large_range_meta *range = &large_ranges[objects[i].large_range_idx];

      range->tail_vpa = objects[i].nbytes % PGSIZE
                        ? (void *)(base + full_pages * PGSIZE)
                        : 0;

      range->active = 0;
      objects[i].active = 0;

      return 0;
    }

    // 11.2 Small object : invalidate only its granules first
    int page_idx = objects[i].page_idx;

    void *free_ptr = mte_tagged_ptr((void *)mte_untagged_addr((uint64)ptr),
                                        FREE_OBJECT_TAG);

    mte_store_tag_range(free_ptr, objects[i].ngrans * MTE_GRANULE_SIZE);
    mark_range_free(page_idx, objects[i].start_granule, objects[i].ngrans);

    objects[i].active = 0;
    pages[page_idx].live_objects--;

    // 11.3 End the page generation only after its last object dies
    if (pages[page_idx].live_objects == 0) {
      if (pgclearpresent(pages[page_idx].vpa) < 0)
        return -1;

      pages[page_idx].active = 0;
    }

    return 0;
  }

  return -1;
}

// 12. Test object-tag exhaustion and a fresh page generation
// 12. Test large-range reuse and page-tag exhaustion
int
main(void)
{
  // Two backing pages: one full page and a partial final page.
  uint nbytes = PGSIZE + 904;

  void *first_raw = 0;
  void *first_vpa = 0;
  void *first_tail = 0;
  int first_range_idx = -1;

  // Fifteen allocations consume page tags 1..15 on one backing range.
  // The sixteenth must obtain fresh backing memory.
  for(int i = 0; i < 16; i++){

    // 12.1. Allocate and locate the corresponding metadata.
    void *p = vpa_object_malloc(nbytes);

    if(p == 0){
      printf("FAIL: allocation at iter=%d\n", i);
      exit(1);
    }

    int obj_idx = -1;

    for(int j = 0; j < MAX_OBJECTS; j++){
      if(objects[j].active && objects[j].ptr == p){
        obj_idx = j;
        break;
      }
    }

    if(obj_idx < 0){
      printf("FAIL: live object metadata missing\n");
      exit(1);
    }

    struct object_meta *object = &objects[obj_idx];
    int range_idx = object->large_range_idx;

    if(!object->is_large || range_idx < 0 ||
       range_idx >= MAX_LARGE_RANGES){
      printf("FAIL: invalid large-range metadata\n");
      exit(1);
    }

    struct large_range_meta *range = &large_ranges[range_idx];

    // An allocated range must have a live owner.
    // Any previous tail alias should already have been invalidated
    // and removed from tail_vpa before this allocation was returned.
    if(!range->active || range->raw != object->large_raw ||
       range->npages != 2 || range->tail_vpa != 0){
      printf("FAIL: incorrect active range state\n");
      exit(1);
    }

    // Save the original backing range and its stale tail pointer.
    if(i == 0){
      first_raw = object->large_raw;
      first_vpa = object->large_vpa;
      first_tail = (void *)((uint64)p + PGSIZE);
      first_range_idx = range_idx;
    }

    // 12.2. Check backing reuse and page-tag selection.
    if(i < 15){
      // The same backing range should receive successive page tags.
      if(object->large_raw != first_raw ||
         range_idx != first_range_idx ||
         object->large_page_tag != i + 1){
        printf("FAIL: expected same backing range and page_tag=%d\n",
               i + 1);
        exit(1);
      }

      // A different page tag must produce a different VPA alias.
      if(i > 0 && object->large_vpa == first_vpa){
        printf("FAIL: old VPA alias reused\n");
        exit(1);
      }
    } else {
      // All fifteen tags on the original range are exhausted.
      // Fresh backing memory starts with unused page-tag history.
      if(object->large_raw == first_raw ||
         object->large_page_tag != 1){
        printf("FAIL: exhausted range should give way to fresh backing\n");
        exit(1);
      }
    }

    printf("iter=%d range=%d raw=%p vpa=%p page_tag=%d p=%p\n",
           i, range_idx, object->large_raw, object->large_vpa,
           object->large_page_tag, p);

    // 12.3. Verify accesses on both sides of the page boundary.
    volatile char *bytes = (volatile char *)p;
    char value = i + 1;

    bytes[0] = value;
    bytes[PGSIZE - 1] = value;
    bytes[PGSIZE] = value;
    bytes[nbytes - 1] = value;

    if(bytes[0] != value || bytes[PGSIZE - 1] != value ||
       bytes[PGSIZE] != value || bytes[nbytes - 1] != value){
      printf("FAIL: data access across page boundary\n");
      exit(1);
    }

    // Save the untagged tail alias before releasing the allocation.
    void *tail_vpa =
      (void *)((uint64)object->large_vpa + PGSIZE);

    // 12.4. Free the object and check persistent range metadata.
    if(vpa_object_free(p) < 0){
      printf("FAIL: free at iter=%d\n", i);
      exit(1);
    }

    // Ownership ends, but the partial tail alias is remembered.
    if(object->active || range->active ||
       range->tail_vpa != tail_vpa){
      printf("FAIL: incorrect freed range state\n");
      exit(1);
    }

    // Tag history must survive free.
    //
    // Iteration 0:  bits 1       -> 0x0002
    // Iteration 1:  bits 1..2    -> 0x0006
    // Iteration 14: bits 1..15   -> 0xfffe
    // Iteration 15: fresh range -> 0x0002
    uint16 expected_history =
      i < 15 ? (1U << (i + 2)) - 2 : 2;

    if(range->used_page_tags != expected_history){
      printf("FAIL: incorrect page-tag history\n");
      exit(1);
    }

    printf("iter=%d writes/free ok; used_page_tags=%x\n",
           i, (uint)range->used_page_tags);
  }

  printf("PASS: backing reuse, tag history, and exhaustion checks\n");

  // 12.5. Probe the FIRST allocation's old partial-tail alias.
  //
  // Immediately after its free, that tail remained mapped with tag 0.
  // Reusing its backing range invalidated that old alias.
  //
  // Expected now:
  //   translation fault / non-present VPA page
  printf("Old first tail should now produce a translation fault: %p\n",
         first_tail);

  *(volatile char *)first_tail = 99;

  printf("FAIL: old tail alias remained accessible\n");
  exit(1);
}
