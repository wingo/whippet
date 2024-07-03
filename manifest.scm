(use-modules (guix packages))

(specifications->manifest
 '("bash"
   "coreutils"
   "gcc-toolchain"
   "glibc"
   "libgc"
   "make"
   "pkg-config"))
