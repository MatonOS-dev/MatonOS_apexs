#ifndef MATON_SESSION_CONTROL_H
#define MATON_SESSION_CONTROL_H
#include <sys/types.h>
#define MATON_SESSION_FLATPAK_UNSUPPORTED 4
#define MATON_SESSION_FLATPAK_ERROR "org.matonos.DBus.Error.UnsupportedFlatpakVersion"
/* Private native registration, not exposed inside Flatpak sandboxes.
 * SO_PEERCRED authenticates the supervisor; its child remains gated until
 * accepted and pidfd_open pins the child before its D-Bus RequestName.
 * Compatibility is the system Flatpak version only, with no protocol version.
 * Status: 0 = start child, 2 = reuse supervisor, 3 = unavailable,
 * 4 = unsupported/unknown system Flatpak (error/message carry a D-Bus error).
 * After status 0, byte 1 acknowledges the portal's RequestName.
 * Keep control connected until portal exit or session/broker shutdown.
 * The registration carries exactly one SCM_RIGHTS directory FD: system UID
 * owns the mode-0777 child of its private mode-0700 runtime directory. The
 * compositor broker binds a second listener there, preserving credentials
 * while giving Flatpak's pivoted proxy helper a native filesystem bus path.
 * The supervisor removes that directory when registration ends.
 * Exact packet lengths are required; missing version metadata fails closed. */
struct MatonSessionRegistration { pid_t pid; char monitor[192]; char flatpak_version[64]; };
struct MatonSessionReply { int status; pid_t supervisor; char error[96]; char message[256]; };
#endif
