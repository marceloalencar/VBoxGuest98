# VBoxGuest98
VirtualBox Guest Additions backport for Microsoft Windows 98/ME, using Windows Driver Model 1.0. Tracks VirtualBox 7.2.22 (r175245) source code.

## Components
**VBoxGst**: PCI driver for VirtualBox Guest Device (`PCI\VEN_80EE&DEV_CAFE`)

**VBoxTray**: Tray application started at logon:
- Mouse pointer integration
- Time synchronization
- Shared clipboard
- Guest statistics
- Display resizing ("Auto-resize Guest Display")
- Drag and drop between the host and the guest
- Reports the VBoxTrayClient facility status to the host
- Logs start/stop, setup failures, clock adjustments, display mode changes
  and the statistics interval to the VM's `VBox.log`

## Recommended: SoftGPU
Windows 98 has no driver for the VirtualBox graphics adapters and falls back
to the standard VGA driver. Install [SoftGPU](https://github.com/JHRobotics/softgpu),
which includes the [vmdisp9x](https://github.com/JHRobotics/vmdisp9x) display
driver (VBoxVGA, VBoxSVGA and VMSVGA adapters). Display resizing needs it, or
another display driver with several modes. SoftGPU is not part of VBoxGuest98.

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
| `VBoxTray/DnD.c` | Drag and drop: proxy window and worker thread |
| `VBoxTray/DnDHgcm.c` | Drag and drop HGCM protocol calls |
| `VBoxTray/DnDTransfer.c` | Drag and drop transfers: receiving and sending files and text |
| `VBoxTray/DnDUtil.c` | Drag and drop text conversions, paths, CF_HDROP and CF_TEXT data |
| `VBoxTray/DnDObj.c` | Drag and drop OLE objects (data object, drop source, drop target) in C |
| `VBoxTray/VbglR3.c` | Driver access helpers for user mode, HGCM call builder |
| `VBoxTray/VBoxTray.rc` | Icon (VirtualBox OSE icon, GPLv3) and version information |
| `installer/VBoxGuest98.nsi` | Installer and uninstaller (NSIS 2.46) |

# Todo:

**VBoxTray**: Seamless mode, guest properties.

**Mouse**: Current implementation may fail on DirectInput applications.

# Compiling:
Microsoft Visual C++ 6.0

Microsoft Windows 2000 DDK

[NSIS 2.46](https://sourceforge.net/projects/nsis/files/NSIS%202/2.46/) (optional)

Run `build.cmd` (free build) or `build.cmd checked` in this folder. It expects
Visual C++ in `C:\MSVC600`, the DDK in `C:\NTDDK` and NSIS in `C:\NSIS246`;
set `MSVC6DIR`, `DDKDIR` and `NSIS2DIR` to use other locations. Alternatively run `build -cZ`
in the Free Build Environment and `makensis installer\VBoxGuest98.nsi`.
