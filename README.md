# VBoxGuest98
VirtualBox Guest Additions backport for Microsoft Windows 98, using Windows Driver Model 1.0. Tracks VirtualBox 6.1.6 source code.

## Components
**VBoxGst**: PCI driver for VirtualBox Guest Device (`PCI\VEN&80EE&DEV_CAFE`)

**VBGTest**: Checks if driver is installed correctly.

# Todo:

**VBoxMous**: Filter driver for mouse pointer integration.

**VBoxTray**: Shared clipboard/drag and drop helper.

# Compiling:
Microsoft Visual C++ 6.0

Microsoft Windows 2000 DDK

Run `build -cZ` in this folder using Free Build Environment.

Make it compile with something newer please
