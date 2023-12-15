#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include "vsfs.h"

int main(const int argc, char **argv)
{

  FILE *file = fopen("sometext.txt", "r");

  // Check if the file was opened successfully
  if (file == NULL)
  {
    perror("Error opening file");
    return 1;
  }

  // Read and print the contents of the file
  char buffer[256]; // Adjust the buffer size based on your needs

  while (fgets(buffer, sizeof(buffer), file) != NULL)
  {
    printf("%s", buffer);
  }

  int ret = vsmount("vdisk");
  if (ret == -1)
  {
    fprintf(stderr, "could not loud file\n");
  }

  int status = vscreate("example.txt");
  if (status == -1)
  {
    printf("error creating file\n");
    return;
  }
  int fd = vsopen("example.txt", MODE_APPEND);
  if (fd == -1)
  {
    printf("error opening file\n");
    return;
  }
  status = vsappend(fd, (void *)buffer, sizeof(buffer));
  if (status == -1)
  {
    printf("error creating file\n");
    return;
  }

  status = vsclose(fd);
  if (status == -1)
  {
    printf("error creating file\n");
    return;
  }

  status = vsumount();
  if (status == -1)
  {
    printf("error unmounting\n");
    return;
  }

  // Close the file
  fclose(file);
  return 0;
}