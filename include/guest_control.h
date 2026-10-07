#ifndef DOSBOX_GUEST_CONTROL_H
#define DOSBOX_GUEST_CONTROL_H

/* Opt-in control socket.  True makes Normal_Loop yield before another CPU
 * chunk, for a debugger transition or while its acknowledgment drains. */
bool GuestControl_Poll();
void GuestControl_Shutdown();

#endif
