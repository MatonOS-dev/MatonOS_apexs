#include <libgen.h>
#include <stdio.h>
#include <string.h>

extern int flatpak_main(int argc, char **argv);
extern int ostree_main(int argc, char **argv);
extern int bwrap_main(int argc, char **argv);

static int
usage(void)
{
  fprintf(stderr, "Usage: matonos-flatpak {flatpak|ostree|bwrap} [args...]\n");
  return 2;
}

static int
dispatch(const char *tool, int argc, char **argv)
{
  if (strcmp(tool, "flatpak") == 0)
    return flatpak_main(argc, argv);
  if (strcmp(tool, "ostree") == 0)
    return ostree_main(argc, argv);
  if (strcmp(tool, "bwrap") == 0)
    return bwrap_main(argc, argv);
  return usage();
}

int
main(int argc, char **argv)
{
  const char *invoked_as = basename(argv[0]);

  if (strcmp(invoked_as, "matonos-flatpak") != 0)
    return dispatch(invoked_as, argc, argv);

  if (argc < 2)
    return usage();

  return dispatch(argv[1], argc - 1, argv + 1);
}
