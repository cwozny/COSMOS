#ifndef RUBY_COMPAT_H
#define RUBY_COMPAT_H

// ruby.h for code that also uses Qt. Include it after the Qt headers.
//
// On Windows, Ruby's win32.h #defines POSIX names as macros -- connect(),
// close(), read(), write(), send(), select(), sleep(), open, truncate and
// more -- and includes windows.h, which #defines min() and max() unless
// NOMINMAX is set. Every Qt call or Qt header after it that uses one of those
// names then stops compiling (QObject::connect(4 args), QWidget::close(),
// QDialog::open(), QString::truncate()). The binding never calls the POSIX
// functions, so the macros are dropped. strcasecmp, getenv and strerror keep
// Ruby's definitions: the binding uses them with their usual meaning.
#if defined(_WIN32) && !defined(NOMINMAX)
#  define NOMINMAX
#endif
#include <ruby.h>

#ifdef _WIN32
#  undef accept
#  undef access
#  undef bind
#  undef close
#  undef connect
#  undef dup2
#  undef execv
#  undef fclose
#  undef fstat
#  undef getcwd
#  undef gethostbyaddr
#  undef gethostbyname
#  undef gethostname
#  undef getpeername
#  undef getpid
#  undef getppid
#  undef getsockname
#  undef getsockopt
#  undef ioctlsocket
#  undef isatty
#  undef isnan
#  undef listen
#  undef lseek
#  undef lstat
#  undef mkdir
#  undef open
#  undef pipe
#  undef pow
#  undef read
#  undef recv
#  undef recvfrom
#  undef rename
#  undef rmdir
#  undef select
#  undef send
#  undef sendto
#  undef setsockopt
#  undef shutdown
#  undef sleep
#  undef socket
#  undef stat
#  undef times
#  undef truncate
#  undef unlink
#  undef utime
#  undef write
#endif

#endif
