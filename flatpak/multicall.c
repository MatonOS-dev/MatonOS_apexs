#include <libgen.h>
#include <stdio.h>
#include <string.h>

extern int flatpak_main(int argc, char **argv);
extern int ostree_cli_main(int argc, char **argv);
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
    return ostree_cli_main(argc, argv);
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

  /* Flatpak's build-update-repo worker re-execs /proc/self/exe with argv[0]
   * set to this real filename and argv[1] set to its Flatpak command. Keep
   * explicit applet selectors for build-time smoke checks, then route normal
   * self-reexecs to Flatpak's command table. */
  if (strcmp(argv[1], "flatpak") == 0 ||
      strcmp(argv[1], "ostree") == 0 ||
      strcmp(argv[1], "bwrap") == 0)
    return dispatch(argv[1], argc - 1, argv + 1);

  return flatpak_main(argc, argv);
}
