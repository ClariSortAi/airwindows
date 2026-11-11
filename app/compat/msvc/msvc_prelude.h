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

/*
 * UxPlay's kernel_timestamp_session_recv (raop_ntp.c) reads every UDP packet of a
 * session: timing replies, audio control and audio data. Built without
 * SIO_TIMESTAMPING, as here, it calls
 *     recvfrom(sock, buf, len, 0, src_addr, (socklen_t *) &addrlen)
 * where addrlen is its own "int *" parameter and every caller passes NULL for both.
 * &addrlen then points at a null pointer, Winsock reads an address length of 0 and
 * fails with WSAEFAULT (10014). No packet was ever read, so the clock never
 * synchronized and a mirroring iPhone had no sound. When no address is wanted,
 * pass no length either.
 */
static __inline int airwindows_recvfrom(SOCKET s, char* buf, int len, int flags, struct sockaddr* from, int* fromlen) {
    return recvfrom(s, buf, len, flags, from, from ? fromlen : NULL);
}
#define recvfrom airwindows_recvfrom

#endif /* AIRPLAY_COMPAT_MSVC_PRELUDE_H */
