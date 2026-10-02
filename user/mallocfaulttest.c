#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int
main(void)
{
  char *p = malloc(32);
  if (p == 0) {
    printf("malloc failed\n");
    exit(1);
  }

  printf("malloc p=%p\n", p);

  p[0] = 1;
  printf("first write OK\n");

  if (pgclearpresent(p) < 0) {
    printf("pgclearpresent failed\n");
    exit(1);
  }

  printf("pte cleared; next malloc write should fault\n");

  p[0] = 2;

  printf("ERROR: write did not fault\n");
  exit(0);
}
