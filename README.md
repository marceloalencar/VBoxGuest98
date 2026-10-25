# VBoxGuest98
VirtualBox Guest Additions backport for Microsoft Windows 98/ME, using Windows Driver Model 1.0. Tracks VirtualBox 7.2.22 (r175245) source code.

## Components
**VBoxGst**: PCI driver for VirtualBox Guest Device (`PCI\VEN_80EE&DEV_CAFE`)

**VBGTest**: Checks if driver is installed correctly and exercises its interface
(versions, raw VMMDev request, event wait, guest property read over HGCM).

**VBoxTray**: Tray application started at logon (installed by `VBoxGst.inf`):
- Mouse pointer integration: reports absolute pointing support to the host and
  positions the pointer with `mouse_event` when the host mouse moves. Windows 98
  drives the PS/2 mouse with VxDs, so this is done in user mode rather than with a
  WDM filter driver. Buttons still come through the PS/2 mouse.
- Time synchronization: sets the guest clock when it is off by 1 second or more,
  every 10 seconds and after restoring a saved state (Windows 98 can't slew the clock).
- Shared clipboard, text only, using the legacy `VBoxSharedClipboard` HGCM protocol.
  Guest text is ANSI (`CF_TEXT`, converted with the active code page); the
  host side is UTF-16. Host text is copied to the guest clipboard as soon as
  the host reports it.
- Guest statistics, at the interval the host asks for (off unless guest metrics
  are requested, e.g. `VBoxManage metrics setup`): physical memory, memory load,
  disk cache, swap file, processes, threads and CPU load (from the Windows 9x
  `KERNEL\CPUUsage` counter, which has no kernel/user split: all busy time is
  reported as user time).
- Reports the VBoxTrayClient facility status to the host.

## VBoxGst features
- Reports guest info (OS type, additions version) and driver facility status to the host.
- Interrupt handling: events are acknowledged in the ISR and dispatched in a DPC.
- Per-handle sessions: event filter, guest capabilities and mouse features of all
  sessions are combined and sent to the host; everything is undone when a handle closes.
- Waiting for VMMDev events, with timeout and interruption.
- Raw VMMDev requests from user mode (requests the driver manages are refused).
- HGCM connect/disconnect/call (needed by shared clipboard and guest properties).
- Mouse position notification callback for a kernel mode mouse filter (VBoxMous).
- Guest heartbeat.
- Optional VM power off when Windows shuts down (`PowerOffOnShutdown` in the INF).

The client interface (I/O control codes and structures) is in
[inc/VBoxGuest98.h](inc/VBoxGuest98.h). It is **not** binary compatible with the
upstream `VBGL_IOCTL_*` interface. User mode opens the device through the device
interface `GUID_DEVINTERFACE_VBOXGUEST98` (see VBGTest), since `\\.\` names refer
to VxDs on Windows 98.

## Source layout
| File | Contents |
| --- | --- |
| `inc/VMMDev.h` | Guest/host protocol (VMMDev requests, events, HGCM, status codes) |
| `inc/VBoxGuest98.h` | Driver interface shared with clients |
| `VBoxGst/Driver.c` | DriverEntry, AddDevice, create/cleanup/close |
| `VBoxGst/PnP.c` | PnP and power IRPs, hardware resources, configuration |
| `VBoxGst/Interrupt.c` | ISR and DPC |
| `VBoxGst/Common.c` | Host initialization, sessions, events, heartbeat |
| `VBoxGst/Hgcm.c` | HGCM requests |
| `VBoxGst/Ioctl.c` | Device control dispatch |
| `VBoxGst/VbglR0.c` | VMMDev request allocation (DMA common buffers) and submission |
| `VBoxTray/VBoxTray.c` | WinMain, tray icon, service threads |
| `VBoxTray/Mouse.c` | Mouse pointer integration |
| `VBoxTray/TimeSync.c` | Guest clock synchronization |
| `VBoxTray/Clipboard.c` | Shared clipboard (text) |
| `VBoxTray/Stats.c` | Guest statistics |
| `VBoxTray/VbglR3.c` | Driver access helpers for user mode |
| `VBoxTray/VBoxTray.rc` | Icon (VirtualBox OSE icon, GPLv3) and version information |

The driver must only import functions NTKERN.VXD provides: check with
`dumpbin /imports VBoxGst.sys` after adding new kernel calls.

# Todo:

**VBoxTray**: Clipboard bitmaps and HTML, drag and drop.

**Mouse**: If the user mode integration proves too slow, a VMOUSE VxD or a HID
minidriver could use `IOCTL_VBOXGUEST98_INTERNAL_SET_MOUSE_NOTIFY` and
`..._SET_MOUSE_STATUS` instead.

# Compiling:
Microsoft Visual C++ 6.0

Microsoft Windows 2000 DDK

Run `build.cmd` (free build) or `build.cmd checked` in this folder. It expects
Visual C++ in `C:\MSVC600` and the DDK in `C:\NTDDK`; set `MSVC6DIR` and
`DDKDIR` to use other locations. Alternatively run `build -cZ` in the
Free Build Environment.

Make it compile with something newer please
