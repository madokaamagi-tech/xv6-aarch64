#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int 
main(void)
{
  char *p = sbrk(4906);
  if (p == (char*)-1) {
    printf("sbrk failed\n");
    exit(1);
  }

  printf("allocated p=%p\n", p);

  p[0] = 1;
  printf("first write ok\n");

  if (pgclearpresent(p) < 0) {
    printf("pgclearpresent failed\n");
    exit(1);
  }

  printf("pte cleared; next write should fault\n");

  p[0] = 2;

  printf("ERROR: write did not fault\n");
  exit(0);
}
