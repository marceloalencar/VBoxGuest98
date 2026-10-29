# VBoxGuest98
VirtualBox Guest Additions backport for Microsoft Windows 98/ME, using Windows Driver Model 1.0. Tracks VirtualBox 7.2.22 (r175245) source code.

## Components
**VBoxGst**: PCI driver for VirtualBox Guest Device (`PCI\VEN_80EE&DEV_CAFE`)

**VBoxMou**: Dynamic VxD loaded by VBoxTray for DirectInput applications and full screen MS-DOS programs

**VBoxSF**: Static VxD, a network file system driver for the shared folders

**VBoxNP**: Network provider DLL for the shared folders

**VBoxTray**: Tray application started at logon:
- Mouse pointer integration
- Time synchronization
- Shared clipboard (text, images, HTML, files and folders
- Guest statistics
- Display resizing ("Auto-resize Guest Display")
- Seamless mode
- Drag and drop between the host and the guest
- Guest properties: OS version, logged in user and network adapters (IP, MAC),
  shown in the VM's session information and `VBoxManage guestproperty enumerate`
- Guest control: `VBoxManage guestcontrol`
- Shared folders auto-mount
- Reports the VBoxTrayClient facility status to the host
- Logs start/stop, setup failures, clock adjustments, display mode changes
  and the statistics interval to the VM's `VBox.log`

## Installing
Run `VBoxGuest98-x.y.z.exe` in the guest and restart Windows when it asks.
Command line options (upper or lower case):
- `/S`: silent installation
- `/R`: restart Windows without asking at the end

For example, from the host with guest control (`START.EXE` keeps the installer
running when it closes VBoxTray):
```
VBoxManage guestcontrol <vm> copyto --username x --password x --target-directory C:\WINDOWS\TEMP\ VBoxGuest98-x.y.z.exe
VBoxManage guestcontrol <vm> start --username x --password x --exe C:\WINDOWS\COMMAND\START.EXE -- C:\WINDOWS\TEMP\VBoxGuest98-x.y.z.exe /S /R
```

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
| `VBoxMou/VBoxMou.asm` | VxD posting mouse moves to VMOUSE (DirectInput, MS-DOS) |
| `VBoxSF/VBoxSFA.asm` | Shared folders VxD: device declaration, control procedure, service thunks |
| `VBoxSF/VBoxSF.c` | Shared folders: initialization, Win32 interface, logging, helpers |
| `VBoxSF/Host.c` | Shared folders: VBoxSharedFolders HGCM client (through VBoxGst) |
| `VBoxSF/Net.c` | Shared folders: IFSMgr connections and volume functions |
| `VBoxSF/File.c` | Shared folders: open files (read, write, locks, times) |
| `VBoxSF/Find.c` | Shared folders: Win32 and MS-DOS directory searches |
| `VBoxSF/Names.c` | Shared folders: 8.3 names and path translation |
| `VBoxNP/VBoxNP.c` | Shared folders network provider: browsing and drive letters through MPR |
| `VBoxTray/Mouse.c` | Mouse pointer integration |
| `VBoxTray/TimeSync.c` | Guest clock synchronization |
| `VBoxTray/Clipboard.c` | Shared clipboard (text, images, HTML) |
| `VBoxTray/ClipFile.c` | Clipboard file transfers (virtual files from the host, guest files to the host) |
| `VBoxTray/Stats.c` | Guest statistics |
| `VBoxTray/Display.c` | Display resizing |
| `VBoxTray/Seamless.c` | Seamless mode (visible region of the guest windows) |
| `VBoxTray/DnD.c` | Drag and drop: proxy window and worker thread |
| `VBoxTray/DnDHgcm.c` | Drag and drop HGCM protocol calls |
| `VBoxTray/DnDTransfer.c` | Drag and drop transfers: receiving and sending files and text |
| `VBoxTray/DnDUtil.c` | Drag and drop text conversions, paths, CF_HDROP and CF_TEXT data |
| `VBoxTray/DnDObj.c` | Drag and drop OLE objects (data object, drop source, drop target) in C |
| `VBoxTray/VMInfo.c` | Guest properties: OS, user and network information |
| `VBoxTray/GuestProp.c` | Guest property HGCM calls |
| `VBoxTray/GstCtrl.c` | Guest control: sessions and programs started from the host |
| `VBoxTray/GstCtrlFile.c` | Guest control: files, directories and file systems |
| `VBoxTray/AutoMount.c` | Automatic mounting of shared folders on drive letters |
| `VBoxTray/VbglR3.c` | Driver access helpers for user mode, HGCM call builder |
| `VBoxTray/VBoxTray.rc` | Icon (VirtualBox OSE icon, GPLv3) and version information |
| `installer/VBoxGuest98.nsi` | Installer and uninstaller (NSIS 2.46) |

# Compiling:
Microsoft Visual C++ 6.0

Microsoft Windows 98 DDK

[NSIS 2.46](https://sourceforge.net/projects/nsis/files/NSIS%202/2.46/) (optional)

Run `build.cmd` (free build) or `build.cmd checked` in this folder. It expects
Visual C++ in `C:\MSVC600`, the Windows 98 DDK in `C:\98DDK` and NSIS in
`C:\NSIS246`; set `MSVC6DIR`, `DDK98DIR` and `NSIS2DIR` to use other
locations.
