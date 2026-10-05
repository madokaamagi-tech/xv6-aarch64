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
  char *p1 = sbrk(PGSIZE);
  char *p2 = sbrk(PGSIZE);

  if(p1 == (char*)-1 || p2 == (char*)-1){
    printf("sbrk failed\n");
    exit(1);
  }

  p1[0] = 1;
  p2[0] = 2;

  printf("p1=%p\n", p1);
  printf("p2=%p\n", p2);

  uint64 p1_t5 = vpamap(p1, 5);
  uint64 p2_t5 = vpamap(p2, 5);

  if(p1_t5 == (uint64)-1 || p2_t5 == (uint64)-1){
    printf("vpamap tag 5 failed\n");
    exit(1);
  }

  uint64 p1_t6 = vpamap(p1, 6);
  uint64 p2_t6 = vpamap(p2, 6);

  if(p1_t6 == (uint64)-1 || p2_t6 == (uint64)-1){
    printf("vpamap tag 6 failed\n");
    exit(1);
  }

  if(pgclearpresent((void *)p1_t6) < 0){
    printf("pgclearpresent p1_t6 failed\n");
    exit(1);
  }

  if(pgclearpresent((void *)p2_t6) < 0){
    printf("pgclearpresent p2_t6 failed\n");
    exit(1);
  }

  printf("p1 tag5=%p\n", p1_t5);
  printf("p2 tag5=%p\n", p2_t5);
  printf("p1 tag6=%p\n", p1_t6);
  printf("p2 tag6=%p\n", p2_t6);

  *(volatile char *)p1_t5 = 11;
  printf("p1 tag5 write ok\n");

  *(volatile char *)p2_t5 = 22;
  printf("p2 tag5 write ok\n");

  printf("p1 tag6 should fault now: %p\n", p1_t6);
  *(volatile char *)p1_t6 = 33;

  printf("ERROR: p1 tag6 write did not fault\n"); 
  exit(0);
}
