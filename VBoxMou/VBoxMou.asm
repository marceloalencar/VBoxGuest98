;******************************************************************************
;
;   VBoxMou - Mouse movement for DirectInput and MS-DOS programs.
;
;   With mouse integration the host sends absolute positions, which VBoxTray
;   gives to USER with mouse_event.  DirectInput and MS-DOS programs don't
;   see that: on Windows 98 and ME they get the mouse from VMOUSE, which only
;   hears from the mouse drivers through VMD_Post_Pointer_Message.  This
;   dynamic VxD lets VBoxTray post relative moves there too, the same way the
;   PS/2 driver does.  (VMD_Post_Absolute_Pointer_Message doesn't move the
;   Windows pointer here, so VBoxTray puts it back with mouse_event.)
;
;   An MS-DOS program counts the relative moves with its own mickey ratio and
;   screen size, and loses them at the screen edges, so when a full screen
;   MS-DOS VM has the mouse its INT 33h pointer is also put at the host
;   position afterwards (function 04h, run in that VM).
;
;   VBoxTray loads it with CreateFile("\\.\VBOXMOU.VXD") and the Win32
;   DeviceIoControl codes below; it is unloaded when VBoxTray closes the
;   handle.
;
;******************************************************************************

        .386p

        .xlist
        include vmm.inc
        include vwin32.inc
        include vmd.inc
        .list

VBOXMOU_DYNAMIC         EQU 1

Declare_Virtual_Device VBOXMOU, 1, 0, VBoxMou_Control, Undefined_Device_ID, \
                       Undefined_Init_Order,,,

;
; DeviceIoControl codes, see VBoxTray\Mouse.c.
;
IOCTL_POST_MOVE         EQU 1           ; in: four dwords, dx and dy (pixels), then the
                                        ; host x and y (0..0FFFFh);
                                        ; out (optional): as IOCTL_OWNER_IS_SYS_VM
IOCTL_OWNER_IS_SYS_VM   EQU 2           ; out: dword, 1 if the system VM has the mouse
IOCTL_SET_DOS_POS       EQU 3           ; in: two dwords, the host x and y (0..0FFFFh);
                                        ; puts the MS-DOS pointer there, if MS-DOS has the mouse

ERROR_NOT_SUPPORTED     EQU 50
ERROR_INVALID_PARAMETER EQU 87

RTLOG_DEBUG_PORT        EQU 504h        ; VMMDev backdoor log port, lands in VBox.log


VxD_LOCKED_DATA_SEG

PrevPostPointer         dd      0       ; Hook_Proc chain
SysVM                   dd      0
fHooked                 dd      0
fOwnPost                dd      0       ; set while we post ourselves
LastButtons             dd      0       ; raw button byte of the last PS/2 packet
LastInstance            dd      0       ; Mouse_Instance of the last PS/2 packet (0 for VMOUSE's own PS/2 code)
DosX                    dd      0       ; host position for the MS-DOS pointer (0..0FFFFh)
DosY                    dd      0
fDosPending             dd      0       ; VBoxMou_Dos_Event is scheduled
szLoaded                db      'VBoxMou: loaded', 10, 0
szUnloaded              db      'VBoxMou: unloaded', 10, 0

VxD_LOCKED_DATA_ENDS


VxD_LOCKED_CODE_SEG

;******************************************************************************
;
;   LogStr - writes the zero terminated string at ESI to VBox.log.
;
;   Uses:   AL, DX, ESI
;
;******************************************************************************
BeginProc LogStr
        mov     dx, RTLOG_DEBUG_PORT
@@:
        lodsb
        or      al, al
        jz      short @F
        out     dx, al
        jmp     short @B
@@:
        ret
EndProc LogStr

;******************************************************************************
;
;   Hook of VMD_Post_Pointer_Message: remembers the button state of the mouse
;   driver's packets, so our own moves don't press or release buttons.
;
;   Entry:  ESI = dx, EDI = dy, AL = raw buttons, EDX -> Mouse_Instance
;
;******************************************************************************
BeginProc VBoxMou_Post_Hook, High_Freq, Hook_Proc, PrevPostPointer
        cmp     [fOwnPost], 0
        jne     short @F
        mov     BYTE PTR [LastButtons], al
        mov     [LastInstance], edx
@@:
        jmp     [PrevPostPointer]
EndProc VBoxMou_Post_Hook

;******************************************************************************
;
;   VBoxMou_Dos_Event - puts the INT 33h pointer of the MS-DOS VM that has
;   the mouse at the latest host position, scaled to the virtual screen of
;   the current video mode (function 26h, 640x200 if it isn't supported).
;   The mouse driver keeps it inside the program's own limits.
;
;   Entry:  EBX = VM handle, EBP -> client registers
;
;******************************************************************************
BeginProc VBoxMou_Dos_Event
        mov     [fDosPending], 0
        Push_Client_State USES_EDI
        VMMCall Begin_Nest_V86_Exec

        mov     [ebp].Client_AX, 26h    ; get maximum virtual coordinates
        mov     [ebp].Client_CX, 0
        mov     [ebp].Client_DX, 0
        mov     eax, 33h
        VMMCall Exec_Int
        movzx   ecx, [ebp].Client_CX
        movzx   esi, [ebp].Client_DX
        or      ecx, ecx
        jz      short DE_Default
        or      esi, esi
        jnz     short DE_Scale
DE_Default:
        mov     ecx, 639
        mov     esi, 199
DE_Scale:
        inc     ecx                     ; x = DosX * (max x + 1) / 10000h
        mov     eax, [DosX]
        mul     ecx
        shr     eax, 16
        mov     [ebp].Client_CX, ax
        inc     esi                     ; y = DosY * (max y + 1) / 10000h
        mov     eax, [DosY]
        mul     esi
        shr     eax, 16
        mov     [ebp].Client_DX, ax
        mov     [ebp].Client_AX, 4      ; set cursor position
        mov     eax, 33h
        VMMCall Exec_Int

        VMMCall End_Nest_Exec
        Pop_Client_State USES_ESI
        ret
EndProc VBoxMou_Dos_Event

;******************************************************************************
;
;   VBoxMou_Control - system control dispatcher.
;
;******************************************************************************
BeginProc VBoxMou_Control
        Control_Dispatch SYS_DYNAMIC_DEVICE_INIT, VBoxMou_Dynamic_Init
        Control_Dispatch SYS_DYNAMIC_DEVICE_EXIT, VBoxMou_Dynamic_Exit
        Control_Dispatch W32_DEVICEIOCONTROL,     VBoxMou_DeviceIoControl
        clc
        ret
EndProc VBoxMou_Control

BeginProc VBoxMou_Dynamic_Init
        VMMCall Get_Sys_VM_Handle
        mov     [SysVM], ebx

        mov     eax, @@VMD_Post_Pointer_Message
        mov     esi, OFFSET32 VBoxMou_Post_Hook
        VMMCall Hook_Device_Service
        jc      short DI_Fail
        mov     [fHooked], 1

        mov     esi, OFFSET32 szLoaded
        call    LogStr
        clc
        ret

DI_Fail:
        stc
        ret
EndProc VBoxMou_Dynamic_Init

BeginProc VBoxMou_Dynamic_Exit
        cmp     [fHooked], 0
        je      short DE_Done
        mov     eax, @@VMD_Post_Pointer_Message
        mov     esi, OFFSET32 VBoxMou_Post_Hook
        VMMCall Unhook_Device_Service
        mov     [fHooked], 0
        mov     esi, OFFSET32 szUnloaded
        call    LogStr
DE_Done:
        clc
        ret
EndProc VBoxMou_Dynamic_Exit

;******************************************************************************
;
;   SetDosPos - when an MS-DOS VM has the mouse, puts its pointer at the host
;   position at EBX (two dwords).  An event pending from an earlier call
;   takes the new position.
;
;   Exit:   EBX = the mouse owner
;   Uses:   EAX, EBX, ECX, EDX, ESI, EDI
;
;******************************************************************************
BeginProc SetDosPos
        mov     eax, [ebx]
        mov     [DosX], eax
        mov     eax, [ebx + 4]
        mov     [DosY], eax
        VxDCall VMD_Get_Mouse_Owner
        cmp     ebx, [SysVM]
        je      short SD_Done
        cmp     [fDosPending], 0
        jne     short SD_Done
        mov     [fDosPending], 1
        push    ebx
        mov     eax, LOW_PRI_DEVICE_BOOST
        mov     ecx, PEF_WAIT_FOR_STI OR PEF_WAIT_NOT_CRIT OR PEF_ALWAYS_SCHED
        xor     edx, edx
        mov     esi, OFFSET32 VBoxMou_Dos_Event
        xor     edi, edi
        VMMCall Call_Priority_VM_Event
        pop     ebx
SD_Done:
        ret
EndProc SetDosPos

;******************************************************************************
;
;   VBoxMou_DeviceIoControl - Win32 DeviceIoControl.
;
;   Entry:  ECX = control code, ESI -> DIOCParams
;   Exit:   EAX = 0 on success, a Win32 error code otherwise
;
;******************************************************************************
BeginProc VBoxMou_DeviceIoControl
        cmp     ecx, DIOC_OPEN
        je      DC_Ok
        cmp     ecx, DIOC_CLOSEHANDLE
        je      DC_Ok
        cmp     ecx, IOCTL_POST_MOVE
        je      short DC_Post
        cmp     ecx, IOCTL_OWNER_IS_SYS_VM
        je      short DC_Owner
        cmp     ecx, IOCTL_SET_DOS_POS
        je      DC_DosPos
        mov     eax, ERROR_NOT_SUPPORTED
        ret

        ;
        ; Posts the relative move with the mouse driver's button state and
        ; instance, with interrupts disabled like the driver's interrupt
        ; handler, puts the MS-DOS pointer at the host position if MS-DOS
        ; has the mouse, then returns the mouse owner when there is room
        ; for it.
        ;
DC_Post:
        cmp     [esi].cbInBuffer, 16
        jb      DC_Invalid
        mov     ebx, [esi].lpvInBuffer
        push    esi
        push    ebx
        pushfd
        cli
        mov     [fOwnPost], 1
        mov     esi, [ebx]
        mov     edi, [ebx + 4]
        mov     al, BYTE PTR [LastButtons]
        mov     edx, [LastInstance]
        VxDCall VMD_Post_Pointer_Message
        mov     [fOwnPost], 0
        popfd
        pop     ebx
        add     ebx, 8
        call    SetDosPos
        pop     esi
        cmp     [esi].cbOutBuffer, 4    ; the owner too?
        jb      DC_Ok
        jmp     short DC_Store_Owner

DC_Owner:
        cmp     [esi].cbOutBuffer, 4
        jb      short DC_Invalid
        push    esi
        VxDCall VMD_Get_Mouse_Owner
        pop     esi
DC_Store_Owner:
        xor     eax, eax
        cmp     ebx, [SysVM]
        jne     short @F
        inc     eax
@@:
        mov     edi, [esi].lpvOutBuffer
        mov     [edi], eax
        mov     edi, [esi].lpcbBytesReturned
        or      edi, edi
        jz      short DC_Ok
        mov     DWORD PTR [edi], 4
DC_Ok:
        xor     eax, eax
        ret

DC_Invalid:
        mov     eax, ERROR_INVALID_PARAMETER
        ret

DC_DosPos:
        cmp     [esi].cbInBuffer, 8
        jb      short DC_Invalid
        mov     ebx, [esi].lpvInBuffer
        call    SetDosPos
        jmp     short DC_Ok
EndProc VBoxMou_DeviceIoControl

VxD_LOCKED_CODE_ENDS

        END
