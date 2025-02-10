(use-modules (guix packages))

(specifications->manifest
 '("bash"
   "coreutils"
   "gcc-toolchain"
   "lttng-ust"
   "glibc"
   "libgc"
   "make"
   "pkg-config"))
