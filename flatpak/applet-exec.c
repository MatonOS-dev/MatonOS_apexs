#include <errno.h>
#include <stdio.h>
#include <unistd.h>

int
main (int argc, char **argv)
{
  if (argc < 3)
    {
      fprintf (stderr, "usage: applet-exec PATH APPLET [ARG...]\n");
      return 2;
    }

  execv (argv[1], &argv[2]);
  perror ("execv");
  return errno == ENOENT ? 127 : 126;
}
