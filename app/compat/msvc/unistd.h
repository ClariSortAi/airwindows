/* Minimal unistd.h for building UxPlay's lib/ with MSVC: only usleep is needed. */
#ifndef AIRPLAY_COMPAT_UNISTD_H
#define AIRPLAY_COMPAT_UNISTD_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>

static __inline int usleep(unsigned int microseconds)
{
    DWORD ms = (DWORD) (microseconds / 1000);
    if (ms == 0 && microseconds > 0) {
        ms = 1;
    }
    Sleep(ms);
    return 0;
}

#endif /* AIRPLAY_COMPAT_UNISTD_H */
