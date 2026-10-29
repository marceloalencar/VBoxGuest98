;******************************************************************************
;
;   VBoxSF - VirtualBox shared folders for Windows 98 and ME.
;
;   The device declaration, the control procedure and the thunks to the VMM,
;   IFSMgr and NTKERN services the C code calls.  The rest is in C.
;
;   VBoxSF is a static VxD (HKLM\System\CurrentControlSet\Services\VxD\VBoxSF,
;   StaticVxD): a network file system driver registers with IFSMgr during
;   Device_Init.  Win32 programs reach it with CreateFile("\\.\VBOXSF") and
;   DeviceIoControl, see VBoxSF.h.
;
;******************************************************************************

        .386p

        .xlist
        include vmm.inc
        include vwin32.inc
        include ifsmgr.inc
        include ntkern.inc
        .list

Declare_Virtual_Device VBOXSF, 1, 0, VBoxSF_Control, Undefined_Device_ID, \
                       Undefined_Init_Order,,,

VxD_LOCKED_CODE_SEG

extrn _VBoxSFDeviceInit:near
extrn _VBoxSFInitComplete:near
extrn _VBoxSFW32IoCtl:near

;******************************************************************************
;
;   VBoxSF_Control - the control procedure.
;
;   Entry:  EAX = system control message
;   Exit:   Carry set on failure
;
;******************************************************************************
BeginProc VBoxSF_Control
        Control_Dispatch Device_Init, VBoxSF_Device_Init
        Control_Dispatch Init_Complete, VBoxSF_Init_Complete
        Control_Dispatch W32_DEVICEIOCONTROL, VBoxSF_W32_DeviceIoControl
        clc
        ret
EndProc VBoxSF_Control

BeginProc VBoxSF_Device_Init
        call    _VBoxSFDeviceInit
        neg     eax                     ; carry set if it returned nonzero
        ret
EndProc VBoxSF_Device_Init

BeginProc VBoxSF_Init_Complete
        call    _VBoxSFInitComplete
        clc
        ret
EndProc VBoxSF_Init_Complete

;
;   Entry:  ECX = control code, ESI -> DIOCParams
;   Exit:   EAX = 0 or a Win32 error code
;
BeginProc VBoxSF_W32_DeviceIoControl
        push    esi
        push    ecx
        call    _VBoxSFW32IoCtl
        add     esp, 8
        ret
EndProc VBoxSF_W32_DeviceIoControl

;
;   Thunks: the C prototypes are in VBoxSF.h.  The services take their
;   arguments on the stack (cdecl, or stdcall for the NTKERN ones), so a jump
;   with the caller's return address is all it takes.
;
SF_THUNK MACRO Name, Service
        PUBLIC  Name
Name    PROC    NEAR
        VxDJmp  Service
Name    ENDP
        ENDM

SF_VMM_THUNK MACRO Name, Service
        PUBLIC  Name
Name    PROC    NEAR
        VMMJmp  Service
Name    ENDP
        ENDM

;
;   PVOID SfCreateSemaphore(LONG cTokens) - NULL on failure.
;
        PUBLIC  _SfCreateSemaphore
_SfCreateSemaphore PROC NEAR
        mov     ecx, [esp + 4]
        VMMCall Create_Semaphore
        jnc     short @F
        xor     eax, eax
@@:
        ret
_SfCreateSemaphore ENDP

;
;   void SfDestroySemaphore(PVOID hSem)
;
        PUBLIC  _SfDestroySemaphore
_SfDestroySemaphore PROC NEAR
        mov     eax, [esp + 4]
        VMMCall Destroy_Semaphore
        ret
_SfDestroySemaphore ENDP

;
;   void SfWaitSemaphore(PVOID hSem, ULONG fFlags) - fFlags = BLOCK_XXX.
;
        PUBLIC  _SfWaitSemaphore
_SfWaitSemaphore PROC NEAR
        mov     eax, [esp + 4]
        mov     ecx, [esp + 8]
        VMMCall Wait_Semaphore
        ret
_SfWaitSemaphore ENDP

;
;   void SfSignalSemaphore(PVOID hSem)
;
        PUBLIC  _SfSignalSemaphore
_SfSignalSemaphore PROC NEAR
        mov     eax, [esp + 4]
        VMMCall Signal_Semaphore_No_Switch
        ret
_SfSignalSemaphore ENDP

SF_VMM_THUNK _SfHeapAllocate,               _HeapAllocate
SF_VMM_THUNK _SfHeapFree,                   _HeapFree
SF_VMM_THUNK _SfCreateMutex,                _CreateMutex
SF_VMM_THUNK _SfEnterMutex,                 _EnterMutex
SF_VMM_THUNK _SfLeaveMutex,                 _LeaveMutex
SF_THUNK     _SfIfsGetVersion,              IFSMgr_Get_Version
SF_THUNK     _SfIfsRegisterNet,             IFSMgr_RegisterNet
SF_THUNK     _SfIfsWin32ToDosTime,          IFSMgr_Win32ToDosTime
SF_THUNK     _SfIfsDosToWin32Time,          IFSMgr_DosToWin32Time
SF_THUNK     _SfIfsMetaMatch,               IFSMgr_MetaMatch
SF_THUNK     _SfIfsUniToBCS,                UniToBCS
SF_THUNK     _SfIfsUniToUpper,              UniToUpper
SF_THUNK     <_SfNtCreateFile@44>,          _NtKernCreateFile
SF_THUNK     <_SfNtClose@4>,                _NtKernClose
SF_THUNK     <_SfNtDeviceIoControl@40>,     _NtKernDeviceIoControl
SF_THUNK     <_SfNtInternalDeviceIoControl@40>, _NtKernInternalDeviceIoControl
SF_THUNK     <_SfNtQueueWorkItem@8>,        _NtKernQueueWorkItem

VxD_LOCKED_CODE_ENDS

        END
