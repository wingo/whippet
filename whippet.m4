AC_DEFUN([WHIPPET_ENABLE_LTO],
 [AC_REQUIRE([AC_PROG_CC])
  AC_MSG_CHECKING([whether the compiler supports -flto])
  old_CFLAGS="$CFLAGS"
  LTO_CFLAGS="-flto"
  CFLAGS="$CFLAGS $LTO_CFLAGS"
  AC_LINK_IFELSE([AC_LANG_PROGRAM([int foo;], [])],, [LTO_CFLAGS=])
  CFLAGS="$old_CFLAGS"
  if test -n "$LTO_CFLAGS"; then
    AC_MSG_RESULT([yes])
  else
    AC_MSG_RESULT([no])
  fi

  AC_ARG_ENABLE(lto,
    [AS_HELP_STRING([--enable-lto]
                    [enable link-time optimization])],
    [],
    [if test -z "$LTO_CFLAGS"; then enable_lto=no; else enable_lto=yes; fi])
  case "$enable_lto" in
    yes | y)
      if test -z "$LTO_CFLAGS"; then
        AC_MSG_ERROR([--enable-lto=$enable_lto unsupported for $CC])
      fi
      CFLAGS="$CFLAGS $LTO_CFLAGS"
      AC_MSG_CHECKING([for lto-specific prefix for ar, nm, objcopy, ranlib])
      if test "$GCC" = yes; then
         TOOLCHAIN_PREFIX=gcc
      else
         # Assuming LLVM if not GCC.  Probably won't hurt.
         TOOLCHAIN_PREFIX=llvm
      fi
      AC_MSG_RESULT([$TOOLCHAIN_PREFIX])
      AC_CHECK_TOOLS([AR], [$TOOLCHAIN_PREFIX-ar ar])
      AC_CHECK_TOOLS([NM], [$TOOLCHAIN_PREFIX-nm nm])
      AC_CHECK_TOOLS([OBJCOPY], [$TOOLCHAIN_PREFIX-objcopy objcopy])
      AC_CHECK_TOOLS([RANLIB], [$TOOLCHAIN_PREFIX-ranlib ranlib])
      ;;
    no | n)
      ;;
    *)
      AC_MSG_ERROR([unexpected --enable-lto=$enable_lto])
      ;;
  esac])
 
AC_DEFUN([WHIPPET_PKG_PLATFORM],
 [# Detect the target system
  AC_MSG_CHECKING([which platform support library the garbage collector should use])
  case "$host_os" in
    *linux-gnu*)
      AC_MSG_RESULT(gnu-linux)
      whippet_platform=gnu-linux
      ;;
    *)
      AC_MSG_ERROR([unsupported host OS: $host_os])
      ;;
  esac
  AM_CONDITIONAL(WHIPPET_PLATFORM_GNU_LINUX, [test "$whippet_platform" = gnu-linux])])

AC_DEFUN([WHIPPET_PKG_TRACING],
 [WHIPPET_TRACING_DEFAULT="m4_default([$1], [auto])"
  AC_ARG_WITH(gc-lttng,
    AS_HELP_STRING([--with-gc-lttng],
                   [Compile GC library with LTTng tracing support (default: $WHIPPET_TRACING_DEFAULT)]),
    [whippet_with_lttng=$withval],
    [whippet_with_lttng=auto])
  PKG_CHECK_MODULES(WHIPPET_LTTNG, lttng-ust,
                    [whippet_have_lttng=yes], [whippet_have_lttng=no])
  AC_MSG_CHECKING(whether to compile GC library with LTTng tracing support)
  if test "$whippet_with_lttng" = auto; then
    if test "$whippet_have_lttng" = no; then
      whippet_use_lttng=no
    else
      whippet_use_lttng=yes
    fi
  else
    whippet_use_lttng=$whippet_with_lttng
  fi
  AC_MSG_RESULT($whippet_use_lttng)

  if test "$whippet_use_lttng" != no && test "$whippet_have_lttng" = no; then
    AC_MSG_ERROR([LTTng support explicitly required, but lttng not found])
  fi
  AM_CONDITIONAL(WHIPPET_USE_LTTNG, [test "$whippet_use_lttng" != no])
  AC_SUBST(WHIPPET_LTTNG_CFLAGS)
  AC_SUBST(WHIPPET_LTTNG_LIBS)])

AC_DEFUN([WHIPPET_PKG_BDW],
 [AC_MSG_CHECKING(for which bdw-gc pkg-config file to use)
  AC_ARG_WITH(bdw-gc,
    AS_HELP_STRING([--with-bdw-gc], [Name of BDW-GC pkg-config file]),
    [bdw_gc="$withval"], [bdw_gc=bdw-gc])
  AC_MSG_RESULT($bdw_gc)
  WHIPPET_BDW_GC=$bdw_gc])

AC_DEFUN([WHIPPET_PKG_COLLECTOR],
 [AC_REQUIRE(WHIPPET_PKG_BDW)
  PKG_CHECK_MODULES(WHIPPET_BDW, $WHIPPET_BDW_GC,
                    [whippet_have_bdw=yes], [whippet_have_bdw=no])
  AC_SUBST(WHIPPET_BDW_CFLAGS)
  AC_SUBST(WHIPPET_BDW_LIBS)

  WHIPPET_COLLECTOR_DEFAULT="m4_default([$1], [pcc])"
  AC_ARG_WITH(gc,
    AS_HELP_STRING([--with-gc],
                   [Select garbage collector implementation (see --with-gc=help)]),
    [whippet_collector=$withval],
    [whippet_collector=$WHIPPET_COLLECTOR_DEFAULT])

  WHIPPET_ALL_COLLECTORS=$(echo <<END
Available garbage collection implementations (--with-gc=GC values):
  semi                     serial copying
  pcc                      parallel copying
  generational-pcc         generational parallel copying
  bdw                      third-party BDW-GC parallel mark-sweep
  mmc                      serial immix
  generational-mmc         mmc + in-place generations
  parallel-mmc             mmc + parallel tracing
  stack-conservative-mmc   mmc + conservative stack root finding
  heap-conservative-mmc    stack-conservative-mmc + conservative heap edges
  stack-conservative-parallel-mmc
  heap-conservative-parallel-mmc
  stack-conservative-generational-mmc
  heap-conservative-generational-mmc
  parallel-generational-mmc
  stack-conservative-parallel-generational-mmc
  heap-conservative-parallel-generational-mmc
                           combinations of the above

The default collector is $WHIPPET_COLLECTOR_DEFAULT.
END
)

  if test "$whippet_collector" = help; then
    echo "$WHIPPET_ALL_COLLECTORS"
    exit 0
  fi

  WHIPPET_COLLECTOR_SEMI=false
  WHIPPET_COLLECTOR_PCC=false
  WHIPPET_COLLECTOR_BDW=false
  WHIPPET_COLLECTOR_MMC=false
  AC_MSG_CHECKING([for which garbage collector implementation to use])
  case "$whippet_collector" in
    semi)
      WHIPPET_COLLECTOR_SEMI=true
      ;;
    pcc | generational-pcc)
      WHIPPET_COLLECTOR_PCC=true
      ;;
    bdw)
      WHIPPET_COLLECTOR_BDW=true
      ;;
    mmc | generational-mmc | parallel-mmc | parallel-generational-mmc | \
    stack-conservative-mmc | stack-conservative-generational-mmc | \
    stack-conservative-parallel-mmc | stack-conservative-parallel-generational-mmc | \
    heap-conservative-mmc | heap-conservative-generational-mmc | \
    heap-conservative-parallel-mmc | heap-conservative-parallel-generational-mmc)
      WHIPPET_COLLECTOR_MMC=true
      ;;
    *)
      AC_MSG_RESULT([unrecognized collector: $whippet_collector; try --with-gc=help])
      exit 1
      ;;
  esac
  WHIPPET_COLLECTOR=$whippet_collector
  AC_MSG_RESULT($WHIPPET_COLLECTOR)
  AC_SUBST(WHIPPET_COLLECTOR)
  AM_CONDITIONAL(WHIPPET_COLLECTOR_SEMI, $WHIPPET_COLLECTOR_SEMI)
  AM_CONDITIONAL(WHIPPET_COLLECTOR_PCC, $WHIPPET_COLLECTOR_PCC)
  AM_CONDITIONAL(WHIPPET_COLLECTOR_BDW, $WHIPPET_COLLECTOR_BDW)
  AM_CONDITIONAL(WHIPPET_COLLECTOR_MMC, $WHIPPET_COLLECTOR_MMC)

  if $WHIPPET_COLLECTOR_BDW && test "$whippet_have_bdw" != yes; then
    AC_MSG_ERROR(BDW-GC collector selected but BDW library not found)
  fi])

AC_DEFUN([WHIPPET_PKG_DEBUG],
 [AC_ARG_WITH(whippet-debug,
    AS_HELP_STRING([--with-gc-debug],
                   [Compile GC library with debugging support (default: no)]),
    [whippet_with_debug=$withval],
    [whippet_with_debug=no])
  AM_CONDITIONAL(WHIPPET_ENABLE_DEBUG, [test "$whippet_with_debug" != no])])

AC_DEFUN([WHIPPET_PKG],
 [AC_REQUIRE([WHIPPET_PKG_PLATFORM])
  AC_REQUIRE([WHIPPET_PKG_TRACING])
  AC_REQUIRE([WHIPPET_PKG_COLLECTOR])
  AC_REQUIRE([WHIPPET_PKG_DEBUG])])
