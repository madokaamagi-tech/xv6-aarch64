#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

#define PGSIZE 4096

static uint64
pagedown(uint64 a)
{
  return a & ~(PGSIZE - 1);
}

int 
main(void)
{
  char *a = malloc(32);
  char *b = malloc(32);

  if (a == 0 || b == 0) {
    printf("malloc failed\n");
    exit(1);
  }

  printf("a=%p page=%p\n", a, pagedown((uint64)a));
  printf("b=%p page=%p\n", b, pagedown((uint64)b));

  a[0] = 1;
  b[0] = 1;
  printf("first write ok\n");

    if(pagedown((uint64)a) != pagedown((uint64)b)){
    printf("a and b are on different pages; try changing allocation sizes\n");
    exit(0);
  }

  if(pgclearpresent(a) < 0){
    printf("pgclearpresent failed\n");
    exit(1);
  }

  printf("cleared a's page; next write to b should fault if same page\n");

  b[0] = 2;

  printf("ERROR: b write did not fault\n");
  exit(0);
}
