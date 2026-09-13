# Opt-in guest control channel

Set both `DOSBOX_X_GUEST_CONTROL_SOCKET` and `DOSBOX_X_GUEST_CONTROL_DIR` to
absolute paths before launch. The endpoint must not already exist. The artifact
directory must exist and must not contain `guest-control.log`; each run owns a
fresh log. With both variables absent, the channel creates nothing. Invalid
configuration disables the channel. Startup succeeds only if logging and the
private mode-0600 socket are available.

The normal CPU loop polls a nonblocking Unix stream socket. This channel never
enters the debugger, changes registers, writes guest memory, or invokes a game
handler. A single local client sends newline-delimited commands:

```
KEY <delay_ms> <duration_ms> <name>
MOUSE MOVE <delay_ms> <signed_dx> <signed_dy>
MOUSE BUTTON <delay_ms> <left|right|middle> <press|release>
MEMDUMP <delay_ms> <address_hex> <length_hex> <basename>
```

Delays are 0–600000 ms. Key duration is 1–600000 ms and delay plus duration must
fit that same limit. Keys are lowercase ASCII letters, digits, or named controls:
`escape`/`esc`, `enter`, `space`, `tab`, `backspace`, arrows, `insert`, `delete`,
`home`, `end`, `pageup`, `pagedown`. A key already scheduled or held by this
channel is rejected until its release; this prevents overlapping pulses from
releasing each other. Guest keyboard repeat remains the emulator's behavior.

Mouse movement uses the existing relative `Mouse_CursorInjected` primitive,
with signed 16-bit deltas. Absolute positioning is not exposed. The primitive
restores the previous host lock state after injection; the DOS mouse driver's
motion-counter API can therefore return zero when the headless pointer is
unlocked even though the guest cursor moved. Do not equate those counters with
delivery failure without checking the driver's lock semantics.

Memory reads use `mem_readb_checked`, bounded to the first 16 MiB, with a positive
length. Dumps are created exclusively under the already-open artifact directory.
Basenames have at most 128 ASCII letters/digits/dots/underscores/hyphens and may
not begin with a dot. Existing files and symlinks are never overwritten. A read,
write, or close error removes the partial file. `done` follows successful close.
Dumps are synchronous host work: they can perturb wall-clock timing and do not
establish uninstrumented temporal fidelity.

Replies and the append-only JSONL log carry a monotonic receipt ID, client
session, PID, guest PIC tick, escaped original command, status, and reason.
Accepted input reports `enqueued`, then `done` or `error`; key pulses also report
`pressed`. Completion confirms delivery to the emulator primitive, not that the
guest application consumed the event. Check guest state separately. The initial
log record identifies protocol `guest-control-v1`.

Each poll reads at most 256 bytes and sends at most 4096 bytes. Lines are limited
to 512 bytes; an oversized line is rejected once and discarded through its next
newline. Its logged command is the bounded prefix. At most 128 logical commands
are pending. Output is capped at 64 KiB; a slow reader is disconnected instead
of silently dropping receipts. Scheduled events continue across disconnects,
but their completions only reach the originating session and the persistent
log. A logging failure stops further channel input. Normal shutdown cancels
pending callbacks, releases channel-held keys/buttons, closes descriptors and
removes the owned endpoint. Abrupt termination can leave the endpoint behind;
use a fresh owned path rather than deleting an unknown existing socket.

## Validation

With `TMPDIR` set to a volume-local directory, run:

```
python3 scripts/test_guest_control_protocol.py
```

This compiles the actual module with deterministic PIC/device stubs and the real
keyboard enum. It exercises parsing, alphabetical mapping, delayed callbacks,
socket framing, dump integrity, reconnect isolation, queue/output caps,
disabled mode and shutdown. It is not a second implementation of the parser.
Build DOSBox-X and verify actual guest BIOS input separately before using the
channel for game evidence. Preserve source/executable hashes, commands, receipt
logs, guest RAM, and owned-process termination records for each capture.

Receipt IDs are bounded by the PIC callback transport type. Exhausting that
range fails closed before an ID can truncate or wrap. Sessions isolate reply
streams, not guest-device ownership: an accepted delayed command remains
accepted across reconnects. Start a fresh owned emulator when replacing an
abandoned controller and a clean input queue is required.
