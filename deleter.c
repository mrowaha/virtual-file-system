#include <stdio.h>
#include "vsfs.h"

int main()
{

  int status = vsmount("vdisk");
  if (status == -1)
  {
    fprintf(stderr, "failed to mount file\n");
    return 1;
  }

  status = vsdelete("example.txt");
  if (status == -1)
  {
    fprintf(stderr, "failed to delete file\n");
  }

  vsumount();
  return 0;
}