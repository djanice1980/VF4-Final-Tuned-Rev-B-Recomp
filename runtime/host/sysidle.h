/* How long the system has seen no key and no mouse (docs/design.md section 85).
 *
 * Windows keeps that time for its own idle timers (the screen's switching off, the screen saver),
 * and a gamepad's buttons do not reset it there, so a player with a pad alone is away to Windows
 * however hard he plays. The window's watch (main.c) writes it into its line when the picture
 * stops, to tell whether the stop comes at one such time. Any thread may ask. */
#ifndef VF4FT_SYSIDLE_H
#define VF4FT_SYSIDLE_H

/* whole seconds since the system's last key or mouse input; -1 where it is not known (anything
 * but Windows) */
int sys_idle_seconds(void);

#endif
