# Everything that used to be an AC_CHECK_* in configure.ac and ends up in
# config.h.  The results feed cmake/config.h.in.
#
# Only the checks that can actually differ on the platforms this port targets
# (Linux/amd64, glibc or musl) are real probes.  The rest are set from the
# options in MonoOptions.cmake or from the target triple.

include(CheckIncludeFile)
include(CheckFunctionExists)
include(CheckSymbolExists)
include(CheckStructHasMember)
include(CheckTypeSize)
include(CheckCSourceCompiles)
include(CheckCSourceRuns)
include(CheckCCompilerFlag)

# Probes must see the same feature-test macros the runtime is compiled with,
# otherwise glibc hides half of what we are looking for.  The Windows CRT has
# no such macros, and naming a library it does not carry -- libm is part of the
# CRT there -- makes every check_function_exists fail for the wrong reason.
if(MONO_HOST_WINDOWS)
  set(CMAKE_REQUIRED_DEFINITIONS "")
  set(CMAKE_REQUIRED_LIBRARIES ws2_32)
else()
  set(CMAKE_REQUIRED_DEFINITIONS -D_GNU_SOURCE -D_REENTRANT)
  set(CMAKE_REQUIRED_LIBRARIES ${CMAKE_DL_LIBS} m)
endif()
set(CMAKE_REQUIRED_QUIET TRUE)
if(CMAKE_USE_PTHREADS_INIT)
  list(APPEND CMAKE_REQUIRED_LIBRARIES Threads::Threads)
endif()

# The headers a socket symbol is declared in.  Everything below asks through
# this list rather than naming netdb.h, so one platform answer covers the set.
if(MONO_HOST_WINDOWS)
  set(_mono_socket_headers winsock2.h ws2tcpip.h)
else()
  set(_mono_socket_headers sys/socket.h netdb.h netinet/in.h arpa/inet.h)
endif()

# Headers
#
# name -> config.h macro, in the autoconf spelling (HAVE_<upper>_<H>).
set(_mono_headers
  alloca.h android/api-level.h android/legacy_signal_inlines.h
  android/ndk-version.h android/versioning.h arpa/inet.h asm/sigcontext.h
  attr/xattr.h checklist.h CommonCrypto/CommonDigest.h complex.h copyfile.h
  crt_externs.h curses.h dirent.h dlfcn.h elf.h execinfo.h fstab.h getopt.h
  gnu/lib-names.h grp.h iconv.h inttypes.h libproc.h link.h linux/if_packet.h
  linux/magic.h linux/netlink.h linux/rtc.h linux/rtnetlink.h linux/serial.h
  machine/endian.h net/if.h netdb.h netinet/in.h netinet/tcp.h pathconf.h
  poll.h pthread.h pthread_np.h pwd.h semaphore.h signal.h stdint.h stdio.h
  stdlib.h strings.h string.h sys/auxv.h sys/cdefs.h sys/endian.h sys/epoll.h
  sys/errno.h sys/event.h sys/extattr.h sys/filio.h sys/inotify.h sys/ioctl.h
  sys/ipc.h sys/mkdev.h sys/mman.h sys/mount.h sys/param.h sys/poll.h
  sys/prctl.h sys/random.h sys/resource.h sys/sdt.h sys/select.h sys/sendfile.h
  sys/socket.h sys/sockio.h sys/statfs.h sys/statvfs.h sys/stat.h sys/syscall.h
  sys/sysctl.h sys/time.h sys/types.h sys/uio.h sys/un.h sys/user.h
  sys/utime.h sys/utsname.h sys/vfstab.h sys/wait.h sys/xattr.h syslog.h
  term.h termios.h ucontext.h unistd.h unwind.h utime.h wchar.h winternl.h
)
foreach(_h IN LISTS _mono_headers)
  string(TOUPPER "HAVE_${_h}" _var)
  string(REGEX REPLACE "[-/.]" "_" _var "${_var}")
  check_include_file("${_h}" ${_var})
endforeach()

# linux/in.h needs sys/socket.h in front of it to compile at all.
check_c_source_compiles("#include <sys/socket.h>\n#include <linux/in.h>\nint main(void){return 0;}"
                        HAVE_LINUX_IN_H)

# The runtime distinguishes "there is a malloc.h" from "glibc's malloc.h",
# because only the latter declares the mallinfo/malloc_usable_size it wants.
check_c_source_compiles("#include <malloc.h>\nint main(void){return 0;}"
                        HAVE_USR_INCLUDE_MALLOC_H)

# Functions
set(_mono_functions
  accept4 arc4random arc4random_buf atexit backtrace_symbols chmod
  clock_nanosleep closelog confstr dl_iterate_phdr dladdr endgrent endpwent
  endusershell epoll_create1 epoll_ctl execv execve execvp fcopyfile fgetgrent
  fgetpwent fork fstatat fstatfs fstatvfs ftruncate ftruncate64 futimens
  futimes getdomainname getdtablesize getentropy getfsstat getgrent getgrgid_r
  getgrnam_r gethostid gethostname gethrtime getifaddrs getlogin_r getmntinfo
  getpeereid getpeername getpriority getpwent getpwnam_r getpwuid_r getrandom
  getresuid getrlimit getrusage inet_aton inet_pton inotify_add_watch
  inotify_init inotify_rm_watch ioctl kill kqueue localtime_r lockf lseek64
  lstat lutimes mach_absolute_time mach_timebase_info madvise mincore mkdtemp
  mknodat mkstemp mkstemps mlock mmap mmap64 mremap msync munlock nl_langinfo
  openlog poll popen posix_fadvise posix_fadvise64 posix_fallocate
  posix_madvise prctl preadv psignal pthread_attr_get_np pthread_attr_getstack
  pthread_attr_getstacksize pthread_attr_setstacksize
  pthread_cond_timedwait_relative_np pthread_get_stackaddr_np
  pthread_get_stacksize_np pthread_getattr_np pthread_getname_np
  pthread_jit_write_protect_np pthread_kill pthread_mutex_timedlock
  pthread_setname_np pwritev Qp2getifaddrs read_real_time readlink readlinkat
  readv remap_file_pages rewinddir sched_getaffinity sched_getcpu
  sched_setaffinity seekdir sendfile setdomainname setgrent setgroups
  sethostid sethostname setpgid setpriority setpwent setresuid setusershell
  shm_open sigaction signal stime stpcpy strcpy_s strlcpy
  strtok_r swab sysconf system tcgetattr tcsetattr telldir ttyname_r uname
  utime utimensat utimes vasprintf vsnprintf vsyslog waitpid writev
)
foreach(_f IN LISTS _mono_functions)
  string(TOUPPER "HAVE_${_f}" _var)
  check_function_exists("${_f}" ${_var})
endforeach()

# clock_gettime lives in librt on old glibc. CMAKE_REQUIRED_LIBRARIES already
# has what Threads pulled in, so a plain check is enough on anything current.
check_function_exists(clock_gettime HAVE_CLOCK_GETTIME)

# statfs/statvfs need their headers to be declared at all on some libcs.
check_symbol_exists(statfs  "sys/vfs.h;sys/statfs.h"   HAVE_STATFS)
check_symbol_exists(statvfs "sys/statvfs.h"            HAVE_STATVFS)
check_symbol_exists(access  "unistd.h"                 HAVE_ACCESS)
check_symbol_exists(pipe2   "unistd.h"                 HAVE_PIPE2)
check_symbol_exists(readdir_r "dirent.h"               HAVE_READDIR_R)
check_symbol_exists(getaddrinfo "${_mono_socket_headers}" HAVE_GETADDRINFO)
check_symbol_exists(getnameinfo "${_mono_socket_headers}" HAVE_GETNAMEINFO)
check_symbol_exists(gethostbyname  "${_mono_socket_headers}" HAVE_GETHOSTBYNAME)
check_symbol_exists(gethostbyname2 "${_mono_socket_headers}" HAVE_GETHOSTBYNAME2)
check_symbol_exists(gethostbyname2_r "${_mono_socket_headers}" HAVE_GETHOSTBYNAME2_R)
check_symbol_exists(getprotobyname   "${_mono_socket_headers}" HAVE_GETPROTOBYNAME)
check_symbol_exists(getprotobyname_r "${_mono_socket_headers}" HAVE_GETPROTOBYNAME_R)
check_symbol_exists(inet_ntop     "${_mono_socket_headers}" HAVE_INET_NTOP)
check_symbol_exists(if_nametoindex "${_mono_socket_headers};net/if.h" HAVE_IF_NAMETOINDEX)
check_symbol_exists(nl_langinfo   "langinfo.h"         HAVE_NL_LANGINFO)
check_symbol_exists(lsetxattr     "sys/xattr.h"        HAVE_LSETXATTR)
check_symbol_exists(pthread_mutexattr_setprotocol "pthread.h"
                    HAVE_DECL_PTHREAD_MUTEXATTR_SETPROTOCOL)

if(HAVE_EPOLL_CREATE1)
  set(HAVE_EPOLL 1)
endif()
if(HAVE_INOTIFY_INIT AND HAVE_INOTIFY_ADD_WATCH AND HAVE_INOTIFY_RM_WATCH)
  set(HAVE_INOTIFY 1)
endif()
if(HAVE_POSIX_FADVISE)
  set(HAVE_POSIX_ADVISE 1)
endif()

# strerror_r: which of the two incompatible flavours do we have?
check_symbol_exists(strerror_r "string.h" HAVE_DECL_STRERROR_R)
if(HAVE_DECL_STRERROR_R)
  set(HAVE_STRERROR_R 1)
  check_c_source_compiles("
    #include <string.h>
    int main(void) { char buf[64]; char *p = strerror_r (0, buf, sizeof buf); return p == buf; }"
    STRERROR_R_CHAR_P)
  if(STRERROR_R_CHAR_P)
    set(HAVE_GNU_STRERROR_R 1)
  endif()
endif()

# Constants, macros and enum members
check_symbol_exists(CLOCK_MONOTONIC        "time.h"    HAVE_CLOCK_MONOTONIC)
check_symbol_exists(CLOCK_MONOTONIC_COARSE "time.h"    HAVE_CLOCK_MONOTONIC_COARSE)
check_symbol_exists(CLOCK_REALTIME         "time.h"    HAVE_CLOCK_REALTIME)
check_symbol_exists(O_CLOEXEC              "fcntl.h"   HAVE_O_CLOEXEC)
check_symbol_exists(F_DUPFD_CLOEXEC        "fcntl.h"   HAVE_F_DUPFD_CLOEXEC)
check_symbol_exists(TCSANOW                "termios.h" HAVE_TCSANOW)
check_symbol_exists(ECHO                   "termios.h" HAVE_ECHO)
check_symbol_exists(ICANON                 "termios.h" HAVE_ICANON)
check_symbol_exists(TIOCGWINSZ             "sys/ioctl.h" HAVE_TIOCGWINSZ)
check_symbol_exists(SIOCGIFCONF            "sys/ioctl.h;net/if.h" HAVE_SIOCGIFCONF)
check_symbol_exists(IN_EXCL_UNLINK         "sys/inotify.h" HAVE_IN_EXCL_UNLINK)
check_symbol_exists(SOL_IP    "${_mono_socket_headers}" HAVE_SOL_IP)
check_symbol_exists(SOL_IPV6  "${_mono_socket_headers}" HAVE_SOL_IPV6)
check_symbol_exists(SOL_TCP   "${_mono_socket_headers}" HAVE_SOL_TCP)
check_symbol_exists(IPPROTO_IP   "${_mono_socket_headers}" HAVE_IPPROTO_IP)
check_symbol_exists(IPPROTO_IPV6 "${_mono_socket_headers}" HAVE_IPPROTO_IPV6)
check_symbol_exists(IPPROTO_TCP  "${_mono_socket_headers}" HAVE_IPPROTO_TCP)
check_symbol_exists(IP_MTU_DISCOVER "${_mono_socket_headers}" HAVE_IP_MTU_DISCOVER)
check_symbol_exists(IP_PMTUDISC_DO  "${_mono_socket_headers}" HAVE_IP_PMTUDISC_DO)
check_symbol_exists(IP_DONTFRAG     "${_mono_socket_headers}" HAVE_IP_DONTFRAG)
check_symbol_exists(IP_DONTFRAGMENT "${_mono_socket_headers}" HAVE_IP_DONTFRAGMENT)
check_symbol_exists(MAP_ANONYMOUS   "sys/mman.h"   HAVE_MAP_ANONYMOUS)
check_symbol_exists(major "sys/sysmacros.h" MAJOR_IN_SYSMACROS)
if(NOT MAJOR_IN_SYSMACROS)
  check_symbol_exists(major "sys/mkdev.h" MAJOR_IN_MKDEV)
endif()

check_c_source_compiles("
  #include <sched.h>
  int main(void) { cpu_set_t s; CPU_ZERO (&s); return CPU_COUNT (&s); }"
  GLIBC_HAS_CPU_COUNT)

check_c_source_compiles("
  #include <netinet/tcp.h>
  int main(void) { int i = (int) TCP_ESTABLISHED; return i; }"
  HAVE_TCP_H_TCPSTATE_ENUM)

check_c_source_compiles("
  #include <sys/types.h>
  #include <sys/stat.h>
  #include <fcntl.h>
  int main(void) { return open (\"/dev/null\", O_RDONLY | O_LARGEFILE); }"
  HAVE_LARGE_FILE_SUPPORT)

# Types
if(MONO_HOST_WINDOWS)
  set(CMAKE_EXTRA_INCLUDE_FILES sys/types.h winsock2.h ws2tcpip.h afunix.h
                                sys/stat.h time.h fcntl.h sys/utime.h signal.h)
else()
  set(CMAKE_EXTRA_INCLUDE_FILES sys/types.h sys/socket.h netinet/in.h sys/un.h
                                sys/stat.h sys/time.h time.h poll.h fcntl.h
                                utime.h dirent.h signal.h)
endif()
# Answers HAVE_<var> for a type, the way autoconf's AC_CHECK_TYPE did: the
# size is a means, and only whether the type exists at all is kept.
function(_mono_probe_type type var)
  check_type_size("${type}" ${var}_SIZE LANGUAGE C)
  if(HAVE_${var}_SIZE)
    set(${var} 1 PARENT_SCOPE)
  else()
    set(${var} "" PARENT_SCOPE)
  endif()
endfunction()

_mono_probe_type("clockid_t"                HAVE_CLOCKID_T)
_mono_probe_type("blksize_t"                HAVE_BLKSIZE_T)
_mono_probe_type("blkcnt_t"                 HAVE_BLKCNT_T)
_mono_probe_type("suseconds_t"              HAVE_SUSECONDS_T)
_mono_probe_type("socklen_t"                HAVE_SOCKLEN_T)
_mono_probe_type("struct flock"             HAVE_STRUCT_FLOCK)
_mono_probe_type("struct flock64"           HAVE_STRUCT_FLOCK64)
_mono_probe_type("struct iovec"             HAVE_STRUCT_IOVEC)
_mono_probe_type("struct linger"            HAVE_STRUCT_LINGER)
_mono_probe_type("struct pollfd"            HAVE_STRUCT_POLLFD)
_mono_probe_type("struct sockaddr"          HAVE_STRUCT_SOCKADDR)
_mono_probe_type("struct sockaddr_storage"  HAVE_STRUCT_SOCKADDR_STORAGE)
_mono_probe_type("struct sockaddr_in"       HAVE_STRUCT_SOCKADDR_IN)
_mono_probe_type("struct sockaddr_in6"      HAVE_STRUCT_SOCKADDR_IN6)
_mono_probe_type("struct sockaddr_un"       HAVE_STRUCT_SOCKADDR_UN)
_mono_probe_type("struct stat"              HAVE_STRUCT_STAT)
_mono_probe_type("struct timespec"          HAVE_STRUCT_TIMESPEC)
_mono_probe_type("struct timeval"           HAVE_STRUCT_TIMEVAL)
_mono_probe_type("struct timezone"          HAVE_STRUCT_TIMEZONE)
_mono_probe_type("struct utimbuf"           HAVE_STRUCT_UTIMBUF)
_mono_probe_type("struct ip_mreqn"          HAVE_STRUCT_IP_MREQN)
_mono_probe_type("struct kinfo_proc"        HAVE_STRUCT_KINFO_PROC)

# ws2tcpip.h names its ancillary-data header `struct cmsghdr` as well, and
# spells the members the same, so this probe answers yes on Windows for a type
# support/map.c is not written against -- it converts the POSIX one, whose other
# half is a set of SCM_* messages Winsock does not have.  Leaving the macro
# undefined is what winconfig.h, the msvc build's config.h, does too.
if(NOT MONO_HOST_WINDOWS)
  _mono_probe_type("struct cmsghdr"         HAVE_STRUCT_CMSGHDR)
endif()
unset(CMAKE_EXTRA_INCLUDE_FILES)

if(HAVE_STRUCT_FLOCK64)
  set(HAVE_FLOCK64 1)
endif()
if(HAVE_STRUCT_IP_MREQN)
  set(HAVE_IP_MREQN 1)
endif()

check_c_source_compiles("
  #include <netinet/in.h>
  int main(void) { struct in_pktinfo p; (void) p.ipi_addr; return 0; }"
  HAVE_IN_PKTINFO)
check_c_source_compiles("
  #include <netinet/in.h>
  int main(void) { struct in6_pktinfo p; (void) p.ipi6_addr; return 0; }"
  HAVE_IPV6_PKTINFO)
# The original probe only forward-declared the tag, so this is really asking
# whether <net/route.h> exists at all.  Kept as-is: the runtime's users of
# HAVE_RT_MSGHDR guard on BSD-only code that Linux never reaches anyway.
check_c_source_compiles("
  #include <sys/types.h>
  #include <net/route.h>
  int main(void) { struct rt_msghdr; return 0; }"
  HAVE_RT_MSGHDR)

# Struct members
check_struct_has_member("struct dirent" d_off    dirent.h  HAVE_STRUCT_DIRENT_D_OFF)
check_struct_has_member("struct dirent" d_reclen dirent.h  HAVE_STRUCT_DIRENT_D_RECLEN)
check_struct_has_member("struct dirent" d_type   dirent.h  HAVE_STRUCT_DIRENT_D_TYPE)
check_struct_has_member("struct dirent" d_namlen dirent.h  HAVE_DIRENT_NAME_LEN)
check_struct_has_member("struct passwd" pw_gecos pwd.h     HAVE_STRUCT_PASSWD_PW_GECOS)
check_struct_has_member("struct statfs" f_flags  "sys/vfs.h;sys/statfs.h" HAVE_STRUCT_STATFS_F_FLAGS)
check_struct_has_member("struct statfs" f_fstypename "sys/vfs.h;sys/statfs.h" HAVE_STATFS_FSTYPENAME)
check_struct_has_member("struct statvfs" f_fstypename "sys/statvfs.h" HAVE_STATVFS_FSTYPENAME)
check_struct_has_member("struct stat" st_atim      sys/stat.h HAVE_STRUCT_STAT_ST_ATIM)
check_struct_has_member("struct stat" st_mtim      sys/stat.h HAVE_STRUCT_STAT_ST_MTIM)
check_struct_has_member("struct stat" st_ctim      sys/stat.h HAVE_STRUCT_STAT_ST_CTIM)
check_struct_has_member("struct stat" st_atimespec sys/stat.h HAVE_STRUCT_STAT_ST_ATIMESPEC)
check_struct_has_member("struct stat" st_birthtime sys/stat.h HAVE_STAT_BIRTHTIME)
check_struct_has_member("struct stat" st_flags     sys/stat.h HAVE_STAT_FLAGS)
check_struct_has_member("struct stat" st_atimensec sys/stat.h HAVE_STAT_NSEC)
check_struct_has_member("struct kinfo_proc" kp_proc sys/sysctl.h HAVE_STRUCT_KINFO_PROC_KP_PROC)
check_struct_has_member("struct tm" tm_gmtoff time.h HAVE_TM_GMTOFF)
check_struct_has_member("struct fd_set" fds_bits sys/select.h HAVE_FDS_BITS)
check_struct_has_member("struct fd_set" __fds_bits sys/select.h HAVE_PRIVATE_FDS_BITS)

if(HAVE_STRUCT_STAT_ST_ATIM)
  set(HAVE_STAT_TIM 1)
endif()
if(HAVE_STRUCT_STAT_ST_ATIMESPEC)
  set(HAVE_STAT_TIMESPEC 1)
endif()

check_symbol_exists(sys_siglist "signal.h" HAVE_SYSSIGNAME)

# Signedness / arity differences between libcs
#
# autoconf compiled a call with the candidate signature and looked for a
# warning. -Werror on the probe gives the same answer without parsing output.
set(_saved_flags "${CMAKE_REQUIRED_FLAGS}")
if(MSVC)
  set(CMAKE_REQUIRED_FLAGS "${CMAKE_REQUIRED_FLAGS} /WX")
else()
  set(CMAKE_REQUIRED_FLAGS "${CMAKE_REQUIRED_FLAGS} -Werror")
endif()

check_c_source_compiles("
  #include <sys/socket.h>
  int main(void) { socklen_t len = 0; bind (0, (struct sockaddr *) 0, len); return 0; }"
  _mono_bind_socklen)
if(NOT _mono_bind_socklen)
  check_c_source_compiles("
    #include <sys/socket.h>
    int main(void) { unsigned int len = 0; bind (0, (struct sockaddr *) 0, len); return 0; }"
    BIND_ADDRLEN_UNSIGNED)
else()
  # socklen_t is itself unsigned on glibc, which is what the original probe
  # was really detecting.
  set(BIND_ADDRLEN_UNSIGNED 1)
endif()

check_c_source_compiles("
  #include <netinet/in.h>
  int main(void) { struct ipv6_mreq m; m.ipv6mr_interface = (unsigned int) 0; return 0; }"
  IPV6MR_INTERFACE_UNSIGNED)

check_c_source_compiles("
  #include <sys/resource.h>
  int main(void) { int who = 0; return getpriority (PRIO_PROCESS, who); }"
  PRIORITY_REQUIRES_INT_WHO)

check_c_source_compiles("
  #include <sys/socket.h>
  #include <netdb.h>
  int main(void) { int flags = 0; return getnameinfo (0, 0, 0, 0, 0, 0, flags); }"
  HAVE_GETNAMEINFO_SIGNED_FLAGS)

check_c_source_compiles("
  #include <unistd.h>
  int main(void) { size_t n = 0; return getdomainname ((char *) 0, n); }"
  HAVE_GETDOMAINNAME_SIZET)

# A conflicting redeclaration is an error, so this asks about the declared
# parameter type instead of a warning the compiler does not always emit.
check_c_source_compiles("
  #include <sys/inotify.h>
  extern int inotify_rm_watch (int, unsigned int);
  int main(void) { return 0; }"
  INOTIFY_RM_WATCH_WD_UNSIGNED)

check_c_source_compiles("
  #include <sys/sendfile.h>
  int main(void) { return (int) sendfile (0, 0, (off_t *) 0, (size_t) 0); }"
  HAVE_SENDFILE_4)
if(NOT HAVE_SENDFILE_4)
  check_c_source_compiles("
    #include <sys/types.h>
    #include <sys/socket.h>
    #include <sys/uio.h>
    int main(void) { return (int) sendfile (0, 0, (off_t) 0, (size_t) 0, (void *) 0, (off_t *) 0); }"
    HAVE_SENDFILE_6)
endif()

set(CMAKE_REQUIRED_FLAGS "${_saved_flags}")

check_c_source_compiles("
  #include <sys/socket.h>
  #include <netinet/in.h>
  int main(void) { struct in6_pktinfo i; struct ip_mreqn m; (void) i; (void) m;
                   return IPV6_RECVPKTINFO + IP_PKTINFO; }"
  HAVE_SUPPORT_FOR_DUAL_MODE_IPV4_PACKET_INFO)
check_symbol_exists(IP_PKTINFO "netinet/in.h" HAVE_IP_PKTINFO)

# Compiler and runtime behaviour
check_c_source_compiles("
  void __attribute__ ((visibility (\"hidden\"))) f (void) {}
  int main(void) { f (); return 0; }"
  HAVE_VISIBILITY_HIDDEN)

check_c_source_compiles("
  __thread int x __attribute__ ((tls_model (\"local-exec\")));
  int main(void) { return x; }"
  HAVE_TLS_MODEL_ATTR)

check_c_source_compiles("
  int main(void) { int x = 0; __sync_bool_compare_and_swap (&x, 0, 1);
                   __sync_fetch_and_add (&x, 1); __sync_synchronize (); return x; }"
  USE_GCC_ATOMIC_OPS)

# sigaltstack that actually delivers on an alternate stack.  autoconf ran this;
# so do we, and we assume "works" when cross-compiling.
check_c_source_runs("
  #include <signal.h>
  #include <stdlib.h>
  #include <string.h>
  #include <sys/mman.h>
  static char *stack;
  static volatile int handled;
  static void handler (int sig) { handled = 1; }
  int main (void) {
    stack_t ss; struct sigaction sa;
    stack = malloc (SIGSTKSZ);
    ss.ss_sp = stack; ss.ss_size = SIGSTKSZ; ss.ss_flags = 0;
    if (sigaltstack (&ss, NULL) != 0) return 1;
    memset (&sa, 0, sizeof sa);
    sa.sa_handler = handler; sa.sa_flags = SA_ONSTACK;
    if (sigaction (SIGUSR1, &sa, NULL) != 0) return 1;
    raise (SIGUSR1);
    return handled ? 0 : 1;
  }"
  HAVE_WORKING_SIGALTSTACK)

check_c_source_runs("
  #include <fcntl.h>
  #include <sys/mman.h>
  #include <sys/stat.h>
  #include <unistd.h>
  int main (void) {
    int fd = shm_open (\"/mono-conf-test\", O_CREAT | O_RDWR, 0600);
    void *p;
    if (fd < 0) return 1;
    if (ftruncate (fd, 4096) != 0) { shm_unlink (\"/mono-conf-test\"); return 1; }
    p = mmap (0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    shm_unlink (\"/mono-conf-test\");
    return p == MAP_FAILED;
  }"
  HAVE_SHM_OPEN_THAT_WORKS_WELL_ENOUGH_WITH_MMAP)

# Sizes
check_type_size("void *"    SIZEOF_VOID_P      BUILTIN_TYPES_ONLY LANGUAGE C)
check_type_size("int"       SIZEOF_INT         BUILTIN_TYPES_ONLY LANGUAGE C)
check_type_size("long"      SIZEOF_LONG        BUILTIN_TYPES_ONLY LANGUAGE C)
check_type_size("long long" SIZEOF_LONG_LONG   BUILTIN_TYPES_ONLY LANGUAGE C)
check_type_size("size_t"    SIZEOF_SIZE_T      LANGUAGE C)
set(TARGET_SIZEOF_VOID_P ${SIZEOF_VOID_P})
set(SIZEOF_REGISTER ${SIZEOF_VOID_P})

include(TestBigEndian)
test_big_endian(_mono_big_endian)
if(_mono_big_endian)
  set(WORDS_BIGENDIAN 1)
endif()
set(TARGET_BYTE_ORDER "G_BYTE_ORDER")

unset(CMAKE_REQUIRED_DEFINITIONS)
unset(CMAKE_REQUIRED_LIBRARIES)
unset(CMAKE_REQUIRED_QUIET)
