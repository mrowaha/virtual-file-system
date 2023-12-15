#include <criterion/criterion.h>
#include <criterion/new/assert.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../src/vsfs.h"

char *vdiskname;
bool vsformatenable;
void setup(void)
{
  vdiskname = (char *)malloc(sizeof("vdisk.bin"));
  strcpy(vdiskname, "vdisk.bin");
}

void teardown(void)
{
  free(vdiskname);
}

TestSuite(vsfs, .init = setup, .fini = teardown);

Test(vsfs, vsformat, .disabled = false)
{
  int m = 23;
  int result = vsformat(vdiskname, m);
  printf("result %d\n", result);
}

Test(vsfs, vsmount, .disabled = false)
{
  int result = vsmount(vdiskname);
}