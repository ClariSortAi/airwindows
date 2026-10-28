/*
 * Force-included (/FI) ahead of every UxPlay lib/ source when building with
 * MSVC, so the upstream files compile unmodified.
 */
#ifndef AIRPLAY_COMPAT_MSVC_PRELUDE_H
#define AIRPLAY_COMPAT_MSVC_PRELUDE_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <BaseTsd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* MSVC's <sys/types.h> has no ssize_t. */
typedef SSIZE_T ssize_t;

/*
 * UxPlay's compat.h does "#define snprintf _snprintf" when snprintf is not a
 * macro. _snprintf does not terminate on truncation, and the UCRT rejects a
 * snprintf macro seen before <stdio.h>. <stdio.h> is already included above, so
 * a self-referential macro is harmless and keeps the standard snprintf.
 */
#define snprintf snprintf

#define strcasecmp _stricmp
#define strncasecmp _strnicmp

/* MinGW's headers define SOL_TCP; the Windows SDK only has IPPROTO_TCP. */
#ifndef SOL_TCP
#define SOL_TCP IPPROTO_TCP
#endif

#endif /* AIRPLAY_COMPAT_MSVC_PRELUDE_H */
