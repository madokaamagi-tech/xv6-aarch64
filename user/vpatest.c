#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

#define PGSIZE 4096
#define VPA_BASE (1ULL << 30)
#define VPA_TAG_STRIDE (1ULL << 26)
/*
static uint64
vpa(void *src, uint64 tag)
{
  uint64 srcva = (uint64)src;
  uint64 page = srcva & ~(PGSIZE - 1);
  uint64 off = srcva - page;

  return VPA_BASE + tag * VPA_TAG_STRIDE + page + off;
}
*/
int
main(void)
{
  char *p = sbrk(PGSIZE);
  if(p == (char*)-1){
    printf("sbrk failed\n");
    exit(1);
  }

  p[0] = 1;
  printf("original p=%p write ok\n", p);

  uint64 good = vpamap(p, 5);
  if(good == (uint64)-1){
    printf("vpamap good failed\n");
    exit(1);
  }

  uint64 bad = vpamap(p, 6);
  if (bad == (uint64)-1){
    printf("vpamap bad failed\n");
    exit(1);
  }

  if (pgclearpresent((void *) bad) < 0) {
    printf("pgclearpresent bad failed\n");
    exit(1);
  }

  printf("good vpa=%p\n", good);
  *(volatile char *)good = 2;
  printf("good vpa write ok\n");

  
  printf("bad vpa=%p; next write should fault\n", bad);
  *(volatile char *)bad = 3;
  printf("ERROR: bad vpa write did not fault\n");
  exit(0);
}
