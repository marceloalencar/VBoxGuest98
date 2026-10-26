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
- Shared clipboard, text, images and HTML, using the legacy `VBoxSharedClipboard` HGCM protocol.
  Guest text is ANSI (`CF_TEXT`, converted with the active code page); the
  host side is UTF-16. Images travel as DIBs (`CF_DIB`; Windows converts to
  and from `CF_BITMAP`), up to 16 MB (e.g. 2560x1440 at 32 bpp). HTML travels
  as a UTF-8 fragment, wrapped in (and taken out of) the Windows `HTML Format`
  (`CF_HTML`) header like upstream does. As upstream, host content uses
  delayed rendering: the host's formats are announced on the guest clipboard
  (`CF_TEXT` and `CF_UNICODETEXT` for text, since Windows 98 doesn't convert
  between them) and the data is fetched from the host only when an application
  pastes. When VBoxTray exits, host content it announced leaves the clipboard.
- Guest statistics, at the interval the host asks for (off unless guest metrics
  are requested, e.g. `VBoxManage metrics setup`): physical memory, memory load,
  disk cache, swap file, processes, threads and CPU load (from the Windows 9x
  `KERNEL\CPUUsage` counter, which has no kernel/user split: all busy time is
  reported as user time).
- Display resizing ("Auto-resize Guest Display"): reports graphics support and
  switches to the resolution the host asks for. Needs a display driver with
  several modes, such as [vmdisp9x](https://github.com/JHRobotics/vmdisp9x) (part
  of SoftGPU). When the exact resolution isn't in the driver's mode list,
  VBoxTray adds it (`MODES` key under the display driver key; only the last
  mode it added is kept); if that fails, the largest listed mode that fits is
  used. The color depth is kept.
- Drag and drop between the host and the guest (mode set in the VM's Devices
  menu), files and text, with the `VBoxDragAndDropSvc` HGCM protocol. Only
  copying, both ways.

  Host to guest, as upstream VBoxDnD: when a host drag enters the guest, an invisible
  window covers the screen and the left button is pressed on it, which starts an
  OLE drag (`DoDragDrop`) that follows the host pointer. On the drop the data is
  requested from the host; files and directories are written to
  `%TEMP%\VirtualBox Dropped Files\<time stamp>` and handed to the drop target as
  `CF_HDROP` (copies), text as `CF_TEXT` (converted from UTF-8 to the active code
  page). Names the code page can't show get `_`. The dropped files stay in the
  temporary directory, like upstream.

  Guest to host, also as upstream: when the host pointer leaves the VM window
  the host asks whether a guest drag is going on. The invisible window, an OLE
  drop target, then covers the screen and the left button is released on it, so
  a guest drag in progress drops there (the guest source sees a copy). Its data
  (`CF_HDROP` files and directories, or `CF_TEXT` converted to UTF-8) is kept
  for a minute and sent when the user drops on the host.
- Reports the VBoxTrayClient facility status to the host.
- Logs start/stop, setup failures, clock adjustments, display mode changes
  and the statistics interval to the release log (see below).

## VBoxGst features
- Reports guest info (OS type, additions version) and driver facility status to the host.
- Interrupt handling: events are acknowledged in the ISR and dispatched in a DPC.
- Per-handle sessions: event filter, guest capabilities and mouse features of all
  sessions are combined and sent to the host; everything is undone when a handle closes.
- Waiting for VMMDev events, with timeout and interruption.
- Raw VMMDev requests from user mode (requests the driver manages are refused).
- HGCM connect/disconnect/call (needed by shared clipboard and guest properties),
  with up to 16 MB of buffers per call, bounced through nonpaged memory.
- Mouse position notification callback for a kernel mode mouse filter (VBoxMous).
- Guest heartbeat.
- Optional VM power off when Windows shuts down (`PowerOffOnShutdown` in the INF).
- Release log through the VMMDev backdoor port (0x504): versions, host features,
  resources, heartbeat and failures go to the VM's `VBox.log` as
  `VMMDev: Guest Log: VBoxGst: ...` lines. User mode writes there with
  `IOCTL_VBOXGUEST98_WRITE_LOG` (VBoxTray lines start with `VBoxTray:`).
  Checked builds also log HGCM connects and disconnects. The host ignores the
  port when the VM has `VBoxInternal/Devices/VMMDev/0/Config/BackdoorLogDisabled` set.

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
| `VBoxGst/Log.c` | Release logging through the backdoor port |
| `VBoxTray/VBoxTray.c` | WinMain, tray icon, service threads |
| `VBoxTray/Mouse.c` | Mouse pointer integration |
| `VBoxTray/TimeSync.c` | Guest clock synchronization |
| `VBoxTray/Clipboard.c` | Shared clipboard (text, images, HTML) |
| `VBoxTray/Stats.c` | Guest statistics |
| `VBoxTray/Display.c` | Display resizing |
| `VBoxTray/DnD.c` | Drag and drop: HGCM protocol, receiving and sending files, proxy window |
| `VBoxTray/DnDObj.c` | Drag and drop OLE objects (data object, drop source, drop target) in C |
| `VBoxTray/VbglR3.c` | Driver access helpers for user mode |
| `VBoxTray/VBoxTray.rc` | Icon (VirtualBox OSE icon, GPLv3) and version information |

The driver must only import functions NTKERN.VXD provides: check with
`dumpbin /imports VBoxGst.sys` after adding new kernel calls.

# Todo:

**VBoxTray**: Seamless mode, guest properties.

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
