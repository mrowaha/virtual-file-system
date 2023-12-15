#include <stdio.h>
#include <stdlib.h>
#include "vsfs.h"

int main(const int argc, char **argv)
{

  char buffer[256]; // Adjust the buffer size based on your needs
  int ret = vsmount("vdisk");
  if (ret == -1)
  {
    fprintf(stderr, "could not loud file\n");
  }

  int fd = vsopen("example.txt", MODE_READ);
  if (fd == -1)
  {
    printf("error opening file\n");
    return;
  }
  int status = vsread(fd, (void *)buffer, sizeof(buffer));
  if (status == -1)
  {
    printf("error creating file\n");
    return;
  }

  printf("%s", buffer);

  status = vsclose(fd);
  if (status == -1)
  {
    printf("error creating file\n");
    return;
  }

  vsumount();
  return 0;
}