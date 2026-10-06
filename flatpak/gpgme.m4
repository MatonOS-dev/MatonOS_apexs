# Minimal compatibility macros for gpgme-lite's pkg-config package.
AC_DEFUN([AM_PATH_GPGME], [
  AC_REQUIRE([PKG_PROG_PKG_CONFIG])
  AC_ARG_VAR([GPGME_CFLAGS], [C compiler flags for GPGME])
  AC_ARG_VAR([GPGME_LIBS], [linker flags for GPGME])
  AS_IF([$PKG_CONFIG --atleast-version="$1" gpgme], [
    GPGME_CFLAGS=`$PKG_CONFIG --cflags gpgme`
    GPGME_LIBS=`$PKG_CONFIG --libs gpgme`
    m4_default([$2], [:])
  ], [m4_default([$3], [AC_MSG_ERROR([gpgme not found])])])
  AC_SUBST([GPGME_CFLAGS])
  AC_SUBST([GPGME_LIBS])
])
AC_DEFUN([AM_PATH_GPGME_PTHREAD], [
  AC_REQUIRE([PKG_PROG_PKG_CONFIG])
  AC_ARG_VAR([GPGME_PTHREAD_CFLAGS], [C compiler flags for threaded GPGME])
  AC_ARG_VAR([GPGME_PTHREAD_LIBS], [linker flags for threaded GPGME])
  AS_IF([$PKG_CONFIG --atleast-version="$1" gpgme], [
    GPGME_PTHREAD_CFLAGS=`$PKG_CONFIG --cflags gpgme`
    GPGME_PTHREAD_LIBS=`$PKG_CONFIG --libs gpgme`
    m4_default([$2], [:])
  ], [m4_default([$3], [AC_MSG_ERROR([gpgme not found])])])
  AC_SUBST([GPGME_PTHREAD_CFLAGS])
  AC_SUBST([GPGME_PTHREAD_LIBS])
])
