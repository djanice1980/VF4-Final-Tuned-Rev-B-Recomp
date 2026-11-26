/* How long the system has seen no key and no mouse: sysidle.h. Windows' headers stay in this
 * file. */
#include "sysidle.h"

#ifdef _WIN32
#include <windows.h>

int sys_idle_seconds(void)
{
    LASTINPUTINFO li;
    li.cbSize = sizeof li;
    if (!GetLastInputInfo(&li))
        return -1;
    return (int)((GetTickCount() - li.dwTime) / 1000);     /* both wrap at 32 bits: their difference does not */
}
#else
int sys_idle_seconds(void)
{
    return -1;
}
#endif
