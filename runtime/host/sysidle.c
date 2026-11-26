/* How long the system has seen no key and no mouse, and what it does with a PC left alone:
 * sysidle.h. Windows' headers stay in this file. */
#include "sysidle.h"

#ifdef _WIN32
#include <windows.h>
#include <stdio.h>

int sys_idle_seconds(void)
{
    LASTINPUTINFO li;
    li.cbSize = sizeof li;
    if (!GetLastInputInfo(&li))
        return -1;
    return (int)((GetTickCount() - li.dwTime) / 1000);     /* both wrap at 32 bits: their difference does not */
}

/* the power plan's time to switch the display off, on mains (ac) and on battery (dc), in seconds
 * (0: never); 0 when the plan cannot be read. powrprof.dll is loaded here for it: the game does
 * not link it */
static int display_off_times(DWORD *ac, DWORD *dc)
{
    typedef DWORD (WINAPI *scheme_fn)(HKEY, GUID **);
    typedef DWORD (WINAPI *value_fn)(HKEY, const GUID *, const GUID *, const GUID *, LPDWORD);
    /* the power plan's "Display" group and its "Turn off display after" value */
    static const GUID video = { 0x7516b95f, 0xf776, 0x4464, { 0x8c, 0x53, 0x06, 0x16, 0x7f, 0x40, 0xcc, 0x99 } };
    static const GUID video_off = { 0x3c0bc021, 0xc8a8, 0x4e07, { 0xa9, 0x73, 0x6b, 0x14, 0xcb, 0xcb, 0x2b, 0x7e } };
    HMODULE pp = LoadLibraryA("powrprof.dll");
    int ok = 0;
    if (!pp)
        return 0;
    scheme_fn scheme_of = (scheme_fn)(void *)GetProcAddress(pp, "PowerGetActiveScheme");
    value_fn ac_of = (value_fn)(void *)GetProcAddress(pp, "PowerReadACValueIndex");
    value_fn dc_of = (value_fn)(void *)GetProcAddress(pp, "PowerReadDCValueIndex");
    GUID *scheme = NULL;
    if (scheme_of && ac_of && dc_of && scheme_of(NULL, &scheme) == ERROR_SUCCESS && scheme) {
        ok = ac_of(NULL, scheme, &video, &video_off, ac) == ERROR_SUCCESS &&
             dc_of(NULL, scheme, &video, &video_off, dc) == ERROR_SUCCESS;
        LocalFree(scheme);
    }
    FreeLibrary(pp);
    return ok;
}

static void seconds_or_never(char *buf, int n, DWORD s)
{
    if (s)
        snprintf(buf, (size_t)n, "after %lu s", (unsigned long)s);
    else
        snprintf(buf, (size_t)n, "never");
}

void sys_idle_describe(char *buf, int n)
{
    SYSTEM_POWER_STATUS ps;
    BOOL saver = FALSE;
    UINT wait = 0;
    DWORD ac = 0, dc = 0;
    const char *power = "power not known";
    char sv[64] = "screen saver not known", off[112] = "the display's switching off not known", a[32], d[32];
    if (GetSystemPowerStatus(&ps))
        power = ps.ACLineStatus == 1 ? "on mains" : ps.ACLineStatus == 0 ? "on battery" : "power not known";
    if (SystemParametersInfoA(SPI_GETSCREENSAVEACTIVE, 0, &saver, 0)) {
        if (!saver)
            snprintf(sv, sizeof sv, "no screen saver set");
        else if (SystemParametersInfoA(SPI_GETSCREENSAVETIMEOUT, 0, &wait, 0))
            snprintf(sv, sizeof sv, "a screen saver set, after %u s", (unsigned)wait);
        else
            snprintf(sv, sizeof sv, "a screen saver set");
    }
    if (display_off_times(&ac, &dc)) {
        seconds_or_never(a, sizeof a, ac);
        seconds_or_never(d, sizeof d, dc);
        snprintf(off, sizeof off, "the display switched off %s on mains, %s on battery", a, d);
    }
    snprintf(buf, (size_t)n, "%s; %s; %s", power, sv, off);
}
#else
int sys_idle_seconds(void)
{
    return -1;
}

void sys_idle_describe(char *buf, int n)
{
    if (n > 0)
        buf[0] = 0;
}
#endif
