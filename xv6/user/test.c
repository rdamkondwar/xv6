#include "types.h"
#include "stat.h"
#include "user.h"
#include <pstat.h>

int
main(int argc, char *argv[])
{
  printf(1, "%s", "Begin!\n");
  struct pstat pstat;
  int ret = getpinfo(&pstat);
  int i;
  for (i=0; i< NPROC; i++) {
    printf(1, "inuse=%d,pid=%d,priority=%d,state=%d\n",
	   pstat.inuse[i],
	   pstat.pid[i],
	   pstat.priority[i],
	   pstat.state[i],
	   pstat.ticks[i][pstat.priority[i]]);
  }
  printf(1, "ret = %d\n", ret);
  exit();
}
