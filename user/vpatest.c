#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

#define PGSIZE 4096
#define VPA_BASE (1ULL << 30)

static uint64
vpa(uint64 tag)
{
  return VPA_BASE + tag * PGSIZE;
}

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
    printf("vpamap failed\n");
    exit(1);
  }

  printf("good vpa=%p\n", good);

  *(volatile char *)good = 2;
  printf("good vpa write ok\n");

  uint64 bad = vpa(6);
  printf("bad vpa=%p; next write should fault\n", bad);

  *(volatile char *)bad = 3;

  printf("ERROR: bad vpa write did not fault\n");
  exit(0);
}
