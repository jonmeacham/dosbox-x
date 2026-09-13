#ifndef DOSBOX_GUEST_CONTROL_H
#define DOSBOX_GUEST_CONTROL_H

/* Opt-in, guest-only control socket.  The normal loop calls this without
 * changing behavior unless DOSBOX_X_GUEST_CONTROL_SOCKET is set. */
void GuestControl_Poll();
void GuestControl_Shutdown();

#endif
