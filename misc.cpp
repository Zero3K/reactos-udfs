////////////////////////////////////////////////////////////////////
// Copyright (C) Alexander Telyatnikov, Ivan Keliukh, Yegor Anchishkin, SKIF Software, 1999-2013. Kiev, Ukraine
// All rights reserved
// This file was released under the GPLv2 on June 2015.
////////////////////////////////////////////////////////////////////
/*

 File: Misc.cpp

 Module: UDF File System Driver (Kernel mode execution only)

 Description:
   This file contains some miscellaneous support routines.

*/

#include            "udffs.h"
// define the file specific bug-check id
#define         UDF_BUG_CHECK_ID                UDF_FILE_MISC

/*

 Function: UDFInitializeZones()

 Description:
   Allocates some memory for global zones used to allocate FSD structures.
   Either all memory will be allocated or we will back out gracefully.

 Expected Interrupt Level (for execution) :

  IRQL_PASSIVE_LEVEL

 Return Value: STATUS_SUCCESS/Error

*/
NTSTATUS
UDFInitializeZones(VOID)
{
    NTSTATUS RC = STATUS_UNSUCCESSFUL;

    _SEH2_TRY {

        // determine memory requirements
        switch (MmQuerySystemSize()) {
        case MmMediumSystem:
            UDFGlobalData.MaxDelayedCloseCount = 24;
            UDFGlobalData.MinDelayedCloseCount = 6;
            UDFGlobalData.MaxDirDelayedCloseCount = 8;
            UDFGlobalData.MinDirDelayedCloseCount = 2;
            UDFGlobalData.WCacheMaxFrames = 8*4;
            UDFGlobalData.WCacheMaxBlocks = 16*64;
            UDFGlobalData.WCacheBlocksPerFrameSh = 8;
            UDFGlobalData.WCacheFramesToKeepFree = 4;
            break;
        case MmLargeSystem:
            UDFGlobalData.MaxDelayedCloseCount = 72;
            UDFGlobalData.MinDelayedCloseCount = 18;
            UDFGlobalData.MaxDirDelayedCloseCount = 24;
            UDFGlobalData.MinDirDelayedCloseCount = 6;
            UDFGlobalData.WCacheMaxFrames = 2*16*4;
            UDFGlobalData.WCacheMaxBlocks = 2*16*64;
            UDFGlobalData.WCacheBlocksPerFrameSh = 8;
            UDFGlobalData.WCacheFramesToKeepFree = 8;
            break;
        case MmSmallSystem:
        default:
            UDFGlobalData.MaxDelayedCloseCount = 8;
            UDFGlobalData.MinDelayedCloseCount = 2;
            UDFGlobalData.MaxDirDelayedCloseCount = 6;
            UDFGlobalData.MinDirDelayedCloseCount = 1;
            UDFGlobalData.WCacheMaxFrames = 8*4/2;
            UDFGlobalData.WCacheMaxBlocks = 16*64/2;
            UDFGlobalData.WCacheBlocksPerFrameSh = 8;
            UDFGlobalData.WCacheFramesToKeepFree = 2;
        }

        ExInitializeNPagedLookasideList(&UDFGlobalData.IrpContextLookasideList,
                                        NULL,
                                        NULL,
                                        POOL_NX_ALLOCATION | POOL_RAISE_IF_ALLOCATION_FAILURE,
                                        sizeof(IRP_CONTEXT),
                                        TAG_IRP_CONTEXT,
                                        0);

        // TODO: move to Paged?
        ExInitializeNPagedLookasideList(&UDFGlobalData.ObjectNameLookasideList,
                                        NULL,
                                        NULL,
                                        POOL_NX_ALLOCATION | POOL_RAISE_IF_ALLOCATION_FAILURE,
                                        sizeof(UDFObjectName),
                                        TAG_OBJECT_NAME,
                                        0);

        ExInitializeNPagedLookasideList(&UDFGlobalData.NonPagedFcbLookasideList,
                                        NULL,
                                        NULL,
                                        POOL_NX_ALLOCATION | POOL_RAISE_IF_ALLOCATION_FAILURE,
                                        sizeof(FCB),
                                        TAG_FCB_NONPAGED,
                                        0);

        ExInitializePagedLookasideList(&UDFGlobalData.CcbLookasideList,
                                        NULL,
                                        NULL,
                                        POOL_NX_ALLOCATION | POOL_RAISE_IF_ALLOCATION_FAILURE,
                                        sizeof(CCB),
                                        TAG_CCB,
                                        0);

        try_return(RC = STATUS_SUCCESS);

try_exit:   NOTHING;

    } _SEH2_FINALLY {
        if (!NT_SUCCESS(RC)) {
            // invoke the destroy routine now ...
            UDFDestroyZones();
        } else {
            // mark the fact that we have allocated zones ...
            SetFlag(UDFGlobalData.UDFFlags, UDF_DATA_FLAGS_ZONES_INITIALIZED);
        }
    } _SEH2_END;

    return(RC);
}


/*************************************************************************
*
* Function: UDFDestroyZones()
*
* Description:
*   Free up the previously allocated memory. NEVER do this once the
*   driver has been successfully loaded.
*
* Expected Interrupt Level (for execution) :
*
*  IRQL_PASSIVE_LEVEL
*
* Return Value: None
*
*************************************************************************/
VOID UDFDestroyZones(VOID)
{
    ExDeleteNPagedLookasideList(&UDFGlobalData.IrpContextLookasideList);
    ExDeleteNPagedLookasideList(&UDFGlobalData.ObjectNameLookasideList);
    ExDeleteNPagedLookasideList(&UDFGlobalData.NonPagedFcbLookasideList);

    ExDeletePagedLookasideList(&UDFGlobalData.CcbLookasideList);
}


/*************************************************************************
*
* Function: UDFIsIrpTopLevel()
*
* Description:
*   Helps the FSD determine who the "top level" caller is for this
*   request. A request can originate directly from a user process
*   (in which case, the "top level" will be NULL when this routine
*   is invoked), OR the user may have originated either from the NT
*   Cache Manager/VMM ("top level" may be set), or this could be a
*   recursion into our code in which we would have set the "top level"
*   field the last time around.
*
* Expected Interrupt Level (for execution) :
*
*  whatever level a particular dispatch routine is invoked at.
*
* Return Value: TRUE/FALSE (TRUE if top level was NULL when routine invoked)
*
*************************************************************************/
BOOLEAN
__fastcall
UDFIsIrpTopLevel(
    PIRP            Irp)            // the IRP sent to our dispatch routine
{
    if(!IoGetTopLevelIrp()) {
        // OK, so we can set ourselves to become the "top level" component
        IoSetTopLevelIrp(Irp);
        return TRUE;
    }
    return FALSE;
}


/*************************************************************************
*
* Function: UDFExceptionFilter()
*
* Description:
*   This routines allows the driver to determine whether the exception
*   is an "allowed" exception i.e. one we should not-so-quietly consume
*   ourselves, or one which should be propagated onwards in which case
*   we will most likely bring down the machine.
*
*   This routine employs the services of FsRtlIsNtstatusExpected(). This
*   routine returns a BOOLEAN result. A RC of FALSE will cause us to return
*   EXCEPTION_CONTINUE_SEARCH which will probably cause a panic.
*   The FsRtl.. routine returns FALSE iff exception values are (currently) :
*       STATUS_DATATYPE_MISALIGNMENT    ||  STATUS_ACCESS_VIOLATION ||
*       STATUS_ILLEGAL_INSTRUCTION  ||  STATUS_INSTRUCTION_MISALIGNMENT
*
* Expected Interrupt Level (for execution) :
*
*  ?
*
* Return Value: EXCEPTION_EXECUTE_HANDLER/EXECEPTION_CONTINUE_SEARCH
*
*************************************************************************/
long
UDFExceptionFilter(
    PIRP_CONTEXT IrpContext,
    PEXCEPTION_POINTERS PtrExceptionPointers
    )
{
    long                            ReturnCode = EXCEPTION_EXECUTE_HANDLER;
    NTSTATUS                        ExceptionCode = STATUS_SUCCESS;
#if defined UDF_DBG || defined PRINT_ALWAYS
    ULONG i;

    UDFPrint(("UDFExceptionFilter\n"));
    UDFPrint(("    Ex. Code: %x\n",PtrExceptionPointers->ExceptionRecord->ExceptionCode));
    UDFPrint(("    Ex. Addr: %x\n",PtrExceptionPointers->ExceptionRecord->ExceptionAddress));
    UDFPrint(("    Ex. Flag: %x\n",PtrExceptionPointers->ExceptionRecord->ExceptionFlags));
    UDFPrint(("    Ex. Pnum: %x\n",PtrExceptionPointers->ExceptionRecord->NumberParameters));
    for(i=0;i<PtrExceptionPointers->ExceptionRecord->NumberParameters;i++) {
        UDFPrint(("       %x\n",PtrExceptionPointers->ExceptionRecord->ExceptionInformation[i]));
    }
#ifdef _X86_
    UDFPrint(("Exception context:\n"));
    if(PtrExceptionPointers->ContextRecord->ContextFlags & CONTEXT_INTEGER) {
        UDFPrint(("EAX=%8.8x   ",PtrExceptionPointers->ContextRecord->Eax));
        UDFPrint(("EBX=%8.8x   ",PtrExceptionPointers->ContextRecord->Ebx));
        UDFPrint(("ECX=%8.8x   ",PtrExceptionPointers->ContextRecord->Ecx));
        UDFPrint(("EDX=%8.8x\n",PtrExceptionPointers->ContextRecord->Edx));

        UDFPrint(("ESI=%8.8x   ",PtrExceptionPointers->ContextRecord->Esi));
        UDFPrint(("EDI=%8.8x   ",PtrExceptionPointers->ContextRecord->Edi));
    }
    if(PtrExceptionPointers->ContextRecord->ContextFlags & CONTEXT_CONTROL) {
        UDFPrint(("EBP=%8.8x   ",PtrExceptionPointers->ContextRecord->Esp));
        UDFPrint(("ESP=%8.8x\n",PtrExceptionPointers->ContextRecord->Ebp));

        UDFPrint(("EIP=%8.8x\n",PtrExceptionPointers->ContextRecord->Eip));
    }
//    UDFPrint(("Flags: %s %s    ",PtrExceptionPointers->ContextRecord->Eip));
#endif //_X86_

#endif // UDF_DBG

    // figure out the exception code
    ExceptionCode = PtrExceptionPointers->ExceptionRecord->ExceptionCode;

    if ((ExceptionCode == STATUS_IN_PAGE_ERROR) && (PtrExceptionPointers->ExceptionRecord->NumberParameters >= 3)) {
        ExceptionCode = PtrExceptionPointers->ExceptionRecord->ExceptionInformation[2];
    }

    if (IrpContext) {
        IrpContext->ExceptionCode = ExceptionCode;
    }

    // check if we should propagate this exception or not
    if (!(FsRtlIsNtstatusExpected(ExceptionCode))) {

        // better free up the IrpContext now ...
        if (IrpContext) {
            UDFPrint(("    UDF Driver internal error\n"));
            BrutePoint();
        } else {
            // we are not ok, propagate this exception.
            //  NOTE: we will bring down the machine ...
            ReturnCode = EXCEPTION_CONTINUE_SEARCH;
        }
    }


    // return the appropriate code
    return(ReturnCode);
} // end UDFExceptionFilter()


/*************************************************************************
*
* Function: UDFExceptionHandler()
*
* Description:
*   One of the routines in the FSD or in the modules we invoked encountered
*   an exception. We have decided that we will "handle" the exception.
*   Therefore we will prevent the machine from a panic ...
*   You can do pretty much anything you choose to in your commercial
*   driver at this point to ensure a graceful exit. In the UDF
*   driver, We shall simply free up the IrpContext (if any), set the
*   error code in the IRP and complete the IRP at this time ...
*
* Expected Interrupt Level (for execution) :
*
*  ?
*
* Return Value: Error code
*
*************************************************************************/
NTSTATUS
UDFProcessException(
    PIRP_CONTEXT IrpContext,
    PIRP             Irp
    )
{
    NTSTATUS            ExceptionCode = STATUS_INSUFFICIENT_RESOURCES;
    PDEVICE_OBJECT      Device;
    PVPB Vpb;
    PETHREAD Thread;

    UDFPrint(("UDFExceptionHandler \n"));

    if (!Irp) {
        UDFPrint(("  !Irp, return\n"));
        ASSERT(!IrpContext);
        return ExceptionCode;
    }

    if (IrpContext) {
        ExceptionCode = IrpContext->ExceptionCode;
        // Free irp context here
        // UDFReleaseIrpContext(IrpContext);
    } else {
        UDFPrint(("  complete Irp and return\n"));
        ExceptionCode = STATUS_INSUFFICIENT_RESOURCES;
        Irp->IoStatus.Status = ExceptionCode;
        Irp->IoStatus.Information = 0;
        // PATCH: No IrpContext here, so just complete IRP
        IoCompleteRequest(Irp, IO_NO_INCREMENT);

        return ExceptionCode;
    }
	
    if (ExceptionCode == STATUS_VERIFY_REQUIRED) {
        if (KeGetCurrentIrql() >= APC_LEVEL) {
            UDFPrint(("  use UDFPostRequest()\n"));
            ExceptionCode = UDFPostRequest(IrpContext, Irp);
        }
    }

    if ((ExceptionCode == STATUS_PENDING) ||
        (ExceptionCode == STATUS_CANT_WAIT)) {

        UDFPrint(("  STATUS_PENDING/STATUS_CANT_WAIT, return\n"));
        return ExceptionCode;
    }

    Irp->IoStatus.Status = ExceptionCode;
    if (IoIsErrorUserInduced( ExceptionCode )) {

        if (ExceptionCode == STATUS_VERIFY_REQUIRED) {

            Device = IoGetDeviceToVerify( Irp->Tail.Overlay.Thread );
            IoSetDeviceToVerify( Irp->Tail.Overlay.Thread, NULL );

            if (Device == NULL) {
                Device = IoGetDeviceToVerify( PsGetCurrentThread() );
                IoSetDeviceToVerify( PsGetCurrentThread(), NULL );
                ASSERT( Device != NULL );
                if (Device == NULL) {
                    UDFPrint(("  Device == NULL, return\n"));
                    ExceptionCode = STATUS_DRIVER_INTERNAL_ERROR;
                    Irp->IoStatus.Status = ExceptionCode;
                    Irp->IoStatus.Information = 0;
                    // PATCH: Set IrpCompleted before completing
                    if (IrpContext) IrpContext->IrpCompleted = TRUE;
                    IoCompleteRequest(Irp, IO_NO_INCREMENT);

                    return ExceptionCode;
                }
            }

            UDFPrint(("  use UDFPerformVerify()\n"));
            //  UDFPerformVerify() will do the right thing with the Irp.
            return UDFPerformVerify( IrpContext, Irp, Device );
        }

        if (FlagOn(IrpContext->Flags, IRP_CONTEXT_FLAG_DISABLE_POPUPS)) {

            UDFPrint(("  DISABLE_POPUPS, complete Irp and return\n"));
            Irp->IoStatus.Status = ExceptionCode;
            Irp->IoStatus.Information = 0;
            // PATCH: Set IrpCompleted before completing
			if (!IrpContext->IrpCompleted) {
            IrpContext->IrpCompleted = TRUE;
            IoCompleteRequest(Irp, IO_NO_INCREMENT);}

            return ExceptionCode;
        } else {

            if (IoGetCurrentIrpStackLocation( Irp )->FileObject != NULL)
                Vpb = IoGetCurrentIrpStackLocation( Irp )->FileObject->Vpb;
            else
                Vpb = NULL;

            Thread = Irp->Tail.Overlay.Thread;
            Device = IoGetDeviceToVerify( Thread );
            if (Device == NULL) {
                Thread = PsGetCurrentThread();
                Device = IoGetDeviceToVerify( Thread );
                ASSERT( Device != NULL );
                if (Device == NULL) {
                    UDFPrint(("  Device == NULL, return(2)\n"));
                    Irp->IoStatus.Status = ExceptionCode;
                    Irp->IoStatus.Information = 0;
                    // PATCH: Set IrpCompleted before completing
					if (!IrpContext->IrpCompleted) {
                    IrpContext->IrpCompleted = TRUE;
                    IoCompleteRequest(Irp, IO_NO_INCREMENT);

                    return ExceptionCode;}
                }
            }

            IoMarkIrpPending( Irp );
            IoRaiseHardError( Irp, Vpb, Device );
            UDFPrint(("  use IoSetDeviceToVerify()\n"));
            IoSetDeviceToVerify( Thread, NULL );
            return STATUS_PENDING;
        }
    }

    // If it was a normal request from IOManager then complete it
    if (Irp) {
        UDFPrint(("  complete Irp\n"));
        Irp->IoStatus.Status = ExceptionCode;
        Irp->IoStatus.Information = 0;
        // PATCH: Set IrpCompleted before completing
		if (!IrpContext->IrpCompleted) {
        IrpContext->IrpCompleted = TRUE;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);}
    }

    UDFPrint(("  return from exception handler with code %x\n", ExceptionCode));
    return(ExceptionCode);
} // end UDFExceptionHandler()

/*************************************************************************
*
* Function: UDFLogEvent()
*
* Description:
*   Log a message in the NT Event Log. This is a rather simplistic log
*   methodology since we can potentially utilize the event log to
*   provide a lot of information to the user (and you should too!)
*
* Expected Interrupt Level (for execution) :
*
*  IRQL_PASSIVE_LEVEL
*
* Return Value: None
*
*************************************************************************/
VOID
UDFLogEvent(
    NTSTATUS UDFEventLogId,      // the UDF private message id
    NTSTATUS RC)                 // any NT error code we wish to log ...
{
    _SEH2_TRY {

        // Implement a call to IoAllocateErrorLogEntry() followed by a call
        // to IoWriteErrorLogEntry(). You should note that the call to IoWriteErrorLogEntry()
        // will free memory for the entry once the write completes (which in actuality
        // is an asynchronous operation).

    } _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        // nothing really we can do here, just do not wish to crash ...
        NOTHING;
    } _SEH2_END;

    return;
} // end UDFLogEvent()


/*************************************************************************
*
* Function: UDFAllocateObjectName()
*
* Description:
*   Allocate a new ObjectName structure to represent an open on-disk object.
*   Also initialize the ObjectName structure to NULL.
*
* Expected Interrupt Level (for execution) :
*
*  IRQL_PASSIVE_LEVEL
*
* Return Value: A pointer to the ObjectName structure OR NULL.
*
*************************************************************************/
PtrUDFObjectName
UDFAllocateObjectName(VOID)
{
    PtrUDFObjectName NewObjectName = NULL;

    NewObjectName = (PtrUDFObjectName)ExAllocateFromNPagedLookasideList(&UDFGlobalData.ObjectNameLookasideList);

    if (!NewObjectName) {
        return NULL;
    }

    // zero out the allocated memory block
    RtlZeroMemory(NewObjectName, sizeof(UDFObjectName));

    // set up some fields ...
    NewObjectName->NodeIdentifier.NodeTypeCode = UDF_NODE_TYPE_OBJECT_NAME;
    NewObjectName->NodeIdentifier.NodeByteSize = sizeof(UDFObjectName);

    return NewObjectName;
} // end UDFAllocateObjectName()


/*************************************************************************
*
* Function: UDFReleaseObjectName()
*
* Description:
*   Deallocate a previously allocated structure.
*
* Expected Interrupt Level (for execution) :
*
*  IRQL_PASSIVE_LEVEL
*
* Return Value: None
*
*************************************************************************/
VOID
UDFReleaseObjectName(
    PtrUDFObjectName ObjectName)
{
    ASSERT(ObjectName);

    ExFreeToNPagedLookasideList(&UDFGlobalData.ObjectNameLookasideList, ObjectName);

    return;
} // end UDFReleaseObjectName()


/*************************************************************************
*
* Function: UDFAllocateCCB()
*
* Description:
*   Allocate a new CCB structure to represent an open on-disk object.
*   Also initialize the CCB structure to NULL.
*
* Expected Interrupt Level (for execution) :
*
*  IRQL_PASSIVE_LEVEL
*
* Return Value: A pointer to the CCB structure OR NULL.
*
*************************************************************************/
PCCB
UDFAllocateCCB(VOID)
{
    PCCB NewCcb = NULL;

    NewCcb = (PCCB)ExAllocateFromPagedLookasideList(&UDFGlobalData.CcbLookasideList);

    if (!NewCcb) {
        return NULL;
    }

    // zero out the allocated memory block
    RtlZeroMemory(NewCcb, sizeof(CCB));

    // set up some fields ...
    NewCcb->NodeIdentifier.NodeTypeCode = UDF_NODE_TYPE_CCB;
    NewCcb->NodeIdentifier.NodeByteSize = sizeof(CCB);

    return NewCcb;
} // end UDFAllocateCCB()


/*************************************************************************
*
* Function: UDFReleaseCCB()
*
* Description:
*   Deallocate a previously allocated structure.
*
* Expected Interrupt Level (for execution) :
*
*  IRQL_PASSIVE_LEVEL
*
* Return Value: None
*
*************************************************************************/
VOID
UDFReleaseCCB(
    PCCB Ccb
    )
{
    ASSERT(Ccb);

    ExFreeToPagedLookasideList(&UDFGlobalData.CcbLookasideList, Ccb);

} // end UDFReleaseCCB()

/*
  Function: UDFCleanupCCB()

  Description:
    Cleanup and deallocate a previously allocated structure.

  Expected Interrupt Level (for execution) :

   IRQL_PASSIVE_LEVEL

  Return Value: None

*/
VOID
__fastcall
UDFCleanUpCCB(
    PCCB Ccb)
{
//    ASSERT(Ccb);
    if(!Ccb) return; // probably, we havn't allocated it...
    ASSERT(Ccb->NodeIdentifier.NodeTypeCode == UDF_NODE_TYPE_CCB);

    _SEH2_TRY {
        if(Ccb->Fcb) {
            UDFTouch(&(Ccb->Fcb->CcbListResource));
            UDFAcquireResourceExclusive(&(Ccb->Fcb->CcbListResource),TRUE);
            RemoveEntryList(&(Ccb->NextCCB));
            UDFReleaseResource(&(Ccb->Fcb->CcbListResource));
        } else {
            BrutePoint();
        }

        if (Ccb->DirectorySearchPattern) {
            if (Ccb->DirectorySearchPattern->Buffer) {
                MyFreePool__(Ccb->DirectorySearchPattern->Buffer);
                Ccb->DirectorySearchPattern->Buffer = NULL;
            }

            MyFreePool__(Ccb->DirectorySearchPattern);
            Ccb->DirectorySearchPattern = NULL;
        }

        UDFReleaseCCB(Ccb);
    } _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        BrutePoint();
    } _SEH2_END;
} // end UDFCleanUpCCB()

/*************************************************************************
*
* Function: UDFAllocateFCB()
*
* Description:
*   Allocate a new FCB structure to represent an open on-disk object.
*   Also initialize the FCB structure to NULL.
*
* Expected Interrupt Level (for execution) :
*
*  IRQL_PASSIVE_LEVEL
*
* Return Value: A pointer to the FCB structure OR NULL.
*
*************************************************************************/
PFCB
UDFAllocateFCB(VOID)
{
    PFCB Fcb = (PFCB)MyAllocatePool__(UDF_FCB_MT, sizeof(FCB));

    if (!Fcb) {
        return NULL;
    }

    // zero out the allocated memory block
    RtlZeroMemory(Fcb, sizeof(FCB));

    // set up some fields ...
    Fcb->NodeIdentifier.NodeTypeCode = UDF_NODE_TYPE_FCB;
    Fcb->NodeIdentifier.NodeByteSize = sizeof(FCB);

    UDFPrint(("UDFAllocateFCB: %x\n", Fcb));
    return(Fcb);
} // end UDFAllocateFCB()


/*************************************************************************
*
* Function: UDFReleaseFCB()
*
* Description:
*   Deallocate a previously allocated structure.
*
* Expected Interrupt Level (for execution) :
*
*  IRQL_PASSIVE_LEVEL
*
* Return Value: None
*
*************************************************************************/
/*VOID
UDFReleaseFCB(
    PtrUDFFCB Fcb
    )
{
    ASSERT(Fcb);

    MyFreePool__(Fcb);

    return;
}*/

/*************************************************************************
*
*
*************************************************************************/
VOID
__fastcall
UDFCleanUpFCB(
    PFCB Fcb
    )
{
    UDFPrint(("UDFCleanUpFCB: %x\n", Fcb));
    if(!Fcb) return;

    ASSERT(Fcb->NodeIdentifier.NodeTypeCode == UDF_NODE_TYPE_FCB);

    _SEH2_TRY {
        // Deinitialize FCBName field
        if (Fcb->FCBName) {
            if(Fcb->FCBName->ObjectName.Buffer) {
                MyFreePool__(Fcb->FCBName->ObjectName.Buffer);
                Fcb->FCBName->ObjectName.Buffer = NULL;
#ifdef UDF_DBG
                Fcb->FCBName->ObjectName.Length =
                Fcb->FCBName->ObjectName.MaximumLength = 0;
#endif
            }
#ifdef UDF_DBG
            else {
                UDFPrint(("UDF: Fcb has invalid FCBName Buffer\n"));
                BrutePoint();
            }
#endif
            UDFReleaseObjectName(Fcb->FCBName);
            Fcb->FCBName = NULL;
        }
#ifdef UDF_DBG
        else {
            UDFPrint(("UDF: Fcb has invalid FCBName field\n"));
            BrutePoint();
        }
#endif


        // begin transaction {
        UDFTouch(&(Fcb->Vcb->FcbListResource));
        UDFAcquireResourceExclusive(&(Fcb->Vcb->FcbListResource), TRUE);
        // Remove this FCB from list of all FCB in VCB
        RemoveEntryList(&(Fcb->NextFCB));
        UDFReleaseResource(&(Fcb->Vcb->FcbListResource));
        // } end transaction

        if(Fcb->FCBFlags & UDF_FCB_INITIALIZED_CCB_LIST_RESOURCE)
            UDFDeleteResource(&(Fcb->CcbListResource));

        // Free memory
        UDFReleaseFCB(Fcb);
    } _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        BrutePoint();
    } _SEH2_END;
} // end UDFCleanUpFCB()

/*************************************************************************
*
* Function: UDFCreateIrpContext()
*
* Description:
*   The UDF FSD creates an IRP context for each request received. This
*   routine simply allocates (and initializes to NULL) a UDFIrpContext
*   structure.
*   Most of the fields in the context structure are then initialized here.
*
* Expected Interrupt Level (for execution) :
*
*  IRQL_PASSIVE_LEVEL
*
* Return Value: A pointer to the IrpContext structure OR NULL.
*
*************************************************************************/
PIRP_CONTEXT
UDFCreateIrpContext(
    PIRP           Irp,
    PDEVICE_OBJECT PtrTargetDeviceObject
    )
{
    ASSERT(Irp);

    PIRP_CONTEXT NewIrpContext = NULL;
    PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);

    //  The only operations a filesystem device object should ever receive
    //  are create/teardown of fsdo handles and operations which do not
    //  occur in the context of fileobjects (i.e., mount).

    if (UdfDeviceIsFsdo(IrpSp->DeviceObject)) {

        if (IrpSp->FileObject != NULL &&
            IrpSp->MajorFunction != IRP_MJ_CREATE &&
            IrpSp->MajorFunction != IRP_MJ_CLEANUP &&
            IrpSp->MajorFunction != IRP_MJ_CLOSE) {

            ExRaiseStatus(STATUS_INVALID_DEVICE_REQUEST);
        }

        NT_ASSERT( IrpSp->FileObject != NULL ||

                (IrpSp->MajorFunction == IRP_MJ_FILE_SYSTEM_CONTROL &&
                 IrpSp->MinorFunction == IRP_MN_USER_FS_REQUEST &&
                 IrpSp->Parameters.FileSystemControl.FsControlCode == FSCTL_INVALIDATE_VOLUMES) ||

                (IrpSp->MajorFunction == IRP_MJ_FILE_SYSTEM_CONTROL &&
                 IrpSp->MinorFunction == IRP_MN_MOUNT_VOLUME ) ||

                IrpSp->MajorFunction == IRP_MJ_SHUTDOWN );
    }

    NewIrpContext = (PIRP_CONTEXT)ExAllocateFromNPagedLookasideList(&UDFGlobalData.IrpContextLookasideList);

    if (NewIrpContext == NULL) {
        return NULL;
    }

    // zero out the allocated memory block
    RtlZeroMemory(NewIrpContext, sizeof(IRP_CONTEXT));

    // Set the proper node type code and node byte size
    NewIrpContext->NodeIdentifier.NodeTypeCode = UDF_NODE_TYPE_IRP_CONTEXT;
    NewIrpContext->NodeIdentifier.NodeByteSize = sizeof(IRP_CONTEXT);

    // Set the originating Irp field
    NewIrpContext->Irp = Irp;
#ifdef UDF_DBG
    NewIrpContext->OverflowQueueMagic = 0;
#endif

    NewIrpContext->TargetDeviceObject = PtrTargetDeviceObject;

    // --- PATCH: Ensure IRP_COMPLETED is initialized to FALSE ---
    NewIrpContext->IrpCompleted = FALSE;

    // TODO: fix
    if (false && IrpSp->FileObject != NULL) {

        PFILE_OBJECT FileObject = IrpSp->FileObject;

        ASSERT(FileObject->DeviceObject == PtrTargetDeviceObject);
        NewIrpContext->TargetDeviceObject = FileObject->DeviceObject;

        //
        //  See if the request is Write Through. Look for both FileObjects opened
        //  as write through, and non-cached requests with the SL_WRITE_THROUGH flag set.
        //
        //  The latter can only originate from kernel components. (Note - NtWriteFile()
        //  does redundantly set the SL_W_T flag for all requests it issues on write
        //  through file objects)
        //

        if (IsFileWriteThrough( FileObject, NewIrpContext->Vcb ) ||
            ( (IrpSp->MajorFunction == IRP_MJ_WRITE) &&
              BooleanFlagOn( Irp->Flags, IRP_NOCACHE) &&
              BooleanFlagOn( IrpSp->Flags, SL_WRITE_THROUGH))) {

            SetFlag(NewIrpContext->Flags, IRP_CONTEXT_FLAG_WRITE_THROUGH);
        }
    }

    if (!UdfDeviceIsFsdo(IrpSp->DeviceObject)) {

        // TODO: use IrpContext->Vcb
        //NewIrpContext->Vcb = &((PVOLUME_DEVICE_OBJECT)IrpSp->DeviceObject)->Vcb;
    }

    //  Major/Minor Function codes
    NewIrpContext->MajorFunction = IrpSp->MajorFunction;
    NewIrpContext->MinorFunction = IrpSp->MinorFunction;

    // Often, a FSD cannot honor a request for asynchronous processing
    // of certain critical requests. For example, a "close" request on
    // a file object can typically never be deferred. Therefore, do not
    // be surprised if sometimes our FSD (just like all other FSD
    // implementations on the Windows NT system) has to override the flag
    // below.
    if (IrpSp->FileObject == NULL) {
        NewIrpContext->Flags |= IRP_CONTEXT_FLAG_WAIT;
    } else {
        if (IoIsOperationSynchronous(Irp)) {
            NewIrpContext->Flags |= IRP_CONTEXT_FLAG_WAIT;
        }
    }

    // Are we top-level ? This information is used by the dispatching code
    // later (and also by the FSD dispatch routine)
    if (IoGetTopLevelIrp() != Irp) {
        // We are not top-level. Note this fact in the context structure
        SetFlag(NewIrpContext->Flags, UDF_IRP_CONTEXT_NOT_TOP_LEVEL);
    }

    return NewIrpContext;
}
 // end UDFCreateIrpContext()


/*************************************************************************
*
* Function: UDFCleanupIrpContext()
*
* Description:
*   Deallocate a previously allocated structure.
*
* Expected Interrupt Level (for execution) :
*
*  IRQL_PASSIVE_LEVEL
*
* Return Value: None
*
*************************************************************************/
VOID
UDFCleanupIrpContext(
    PIRP_CONTEXT IrpContext)
{
    ASSERT(IrpContext);

    if (!FlagOn(IrpContext->Flags, IRP_CONTEXT_FLAG_ON_STACK)) {
#ifdef UDF_DBG
    IrpContext->OverflowQueueMagic = 0;
#endif
        ExFreeToNPagedLookasideList(&UDFGlobalData.IrpContextLookasideList, IrpContext);
    }
} // end UDFCleanupIrpContext()

_When_(RaiseOnError || return, _At_(Fcb->FileLock, _Post_notnull_))
_When_(RaiseOnError, _At_(IrpContext, _Pre_notnull_))
BOOLEAN
UDFCreateFileLock (
    _In_opt_ PIRP_CONTEXT IrpContext,
    _Inout_ PFCB Fcb,
    _In_ BOOLEAN RaiseOnError
    )

/*++

Routine Description:

    This routine is called when we want to attach a file lock structure to the
    given Fcb.  It is possible the file lock is already attached.

    This routine is sometimes called from the fast path and sometimes in the
    Irp-based path.  We don't want to raise in the fast path, just return FALSE.

Arguments:

    Fcb - This is the Fcb to create the file lock for.

    RaiseOnError - If TRUE, we will raise on an allocation failure.  Otherwise we
        return FALSE on an allocation failure.

Return Value:

    BOOLEAN - TRUE if the Fcb has a filelock, FALSE otherwise.

--*/

{
    BOOLEAN Result = TRUE;
    PFILE_LOCK FileLock;

    PAGED_CODE();

    ASSERT(RaiseOnError == FALSE);

    //  Lock the Fcb and check if there is really any work to do.

    //TODO: impl
    //UDFLockFcb( IrpContext, Fcb );

    if (Fcb->FileLock != NULL) {

        //TODO: impl
        //UDFUnlockFcb( IrpContext, Fcb );
        return TRUE;
    }

    Fcb->FileLock = FileLock = FsRtlAllocateFileLock(NULL, NULL);

    //TODO: impl
    //UDFUnlockFcb( IrpContext, Fcb );

    //  Return or raise as appropriate.
    if (FileLock == NULL) {
         
        if (RaiseOnError) {

            NT_ASSERT(ARGUMENT_PRESENT(IrpContext));

            UDFRaiseStatus(IrpContext, STATUS_INSUFFICIENT_RESOURCES);
        }

        Result = FALSE;
    }

    return Result;
}

/*************************************************************************
*
* Function: UDFPostRequest()
*
* Description:
*   Queue up a request for deferred processing (in the context of a system
*   worker thread). The caller must have locked the user buffer (if required)
*
* Expected Interrupt Level (for execution) :
*
*  IRQL_PASSIVE_LEVEL
*
* Return Value: STATUS_PENDING
*
*************************************************************************/
NTSTATUS
UDFPostRequest(
    IN PIRP_CONTEXT IrpContext,
    IN PIRP             Irp
    )
{
    KIRQL SavedIrql;
//    PIO_STACK_LOCATION IrpSp;
    PVCB Vcb;

//    IrpSp = IoGetCurrentIrpStackLocation(Irp);

/*
    if(Vcb->StopOverflowQueue) {
        if(Irp) {
            Irp->IoStatus.Status = STATUS_WRONG_VOLUME;
            Irp->IoStatus.Information = 0;
            IoCompleteRequest(Irp, IO_DISK_INCREMENT);
        }
        UDFReleaseIrpContext(IrpContext);
        return STATUS_WRONG_VOLUME;
    }
*/
    // mark the IRP pending if this is not double post
    if(Irp)
        IoMarkIrpPending(Irp);

    Vcb = (PVCB)(IrpContext->TargetDeviceObject->DeviceExtension);
    KeAcquireSpinLock(&(Vcb->OverflowQueueSpinLock), &SavedIrql);

    if ( Vcb->PostedRequestCount > FSP_PER_DEVICE_THRESHOLD) {

        //  We cannot currently respond to this IRP so we'll just enqueue it
        //  to the overflow queue on the volume.
        //  Note: we just reuse LIST_ITEM field inside WorkQueueItem, this
        //  doesn't matter to regular processing of WorkItems.
        #ifdef UDF_DBG
        // Check that this IRP_CONTEXT isn't already queued
        ASSERT(IrpContext->OverflowQueueMagic != UDF_OVERFLOWQ_MAGIC);
        #endif

        InsertTailList( &(Vcb->OverflowQueue),
                        &(IrpContext->WorkQueueItem.List) );
         Vcb->OverflowQueueCount++;
         #ifdef UDF_DBG
         IrpContext->OverflowQueueMagic = UDF_OVERFLOWQ_MAGIC;
         #endif
         KeReleaseSpinLock( &(Vcb->OverflowQueueSpinLock), SavedIrql );

    } else {

        //  We are going to send this Irp to an ex worker thread so up
        //  the count.
        Vcb->PostedRequestCount++;

        KeReleaseSpinLock( &(Vcb->OverflowQueueSpinLock), SavedIrql );

        // queue up the request
        ExInitializeWorkItem(&(IrpContext->WorkQueueItem), UDFFspDispatch, IrpContext);

        ExQueueWorkItem(&(IrpContext->WorkQueueItem), CriticalWorkQueue);
    //    ExQueueWorkItem(&(IrpContext->WorkQueueItem), DelayedWorkQueue);
        #ifdef UDF_DBG
        IrpContext->OverflowQueueMagic = 0;
        #endif

    }

    // return status pending
    return STATUS_PENDING;
} // end UDFPostRequest()


/*************************************************************************
*
* Function: UDFFspDispatch()
*
* Description:
*   The common dispatch routine invoked in the context of a system worker
*   thread. All we do here is pretty much case off the major function
*   code and invoke the appropriate FSD dispatch routine for further
*   processing.
*
* Expected Interrupt Level (for execution) :
*
*   IRQL PASSIVE_LEVEL
*
* Return Value: None
*
*************************************************************************/
VOID
NTAPI
UDFFspDispatch(
    IN PVOID Context   // actually is a pointer to IRPContext structure
    )
{
    NTSTATUS         RC = STATUS_SUCCESS;
    PIRP_CONTEXT     IrpContext = (PIRP_CONTEXT)Context;
    PIRP_CONTEXT     PrevIrpContext = NULL;
    PIRP             Irp = NULL;
    PVCB             Vcb;
    KIRQL            SavedIrql;
    PLIST_ENTRY      Entry;
    BOOLEAN          SpinLock = FALSE;

    // ... (assertions etc)

    Vcb = (PVCB)(IrpContext->TargetDeviceObject->DeviceExtension);
    ASSERT(Vcb);

    UDFPrint(("  *** Thr: %x  ThCnt: %x  QCnt: %x  Started!\n", PsGetCurrentThread(), Vcb->PostedRequestCount, Vcb->OverflowQueueCount));

    // ** Set to FALSE before the loop (for the first IRP_CONTEXT) **
    IrpContext->IrpCompleted = FALSE;

    while (TRUE) {
        UDFPrint(("    Next IRP\n"));
        FsRtlEnterFileSystem();

        //  Get a pointer to the IRP structure
        Irp = IrpContext->Irp;

        // ... (top-level IRP logic)

        IrpContext->Flags |= IRP_CONTEXT_FLAG_WAIT;

        _SEH2_TRY {
            UDFPrint(("  *** MJ: %x, Thr: %x\n", IrpContext->MajorFunction, PsGetCurrentThread()));
            switch (IrpContext->MajorFunction) {
            case IRP_MJ_CREATE:
                RC = UDFCommonCreate(IrpContext, Irp);
                break;
            case IRP_MJ_READ:
                RC = UDFCommonRead(IrpContext, Irp);
                break;
            case IRP_MJ_WRITE:
                RC = UDFCommonWrite(IrpContext, Irp);
                break;
            case IRP_MJ_CLEANUP:
                RC = UDFCommonCleanup(IrpContext, Irp);
                break;
            case IRP_MJ_CLOSE:
                RC = UDFCommonClose(IrpContext, Irp, TRUE);
                break;
            case IRP_MJ_DIRECTORY_CONTROL:
                RC = UDFCommonDirControl(IrpContext, Irp);
                break;
            case IRP_MJ_QUERY_INFORMATION:
                RC = UDFCommonQueryInfo(IrpContext, Irp);
                break;
            case IRP_MJ_SET_INFORMATION:
                RC = UDFCommonSetInfo(IrpContext, Irp);
                break;
            case IRP_MJ_QUERY_VOLUME_INFORMATION:
                RC = UDFCommonQueryVolInfo(IrpContext, Irp);
                break;
            case IRP_MJ_SET_VOLUME_INFORMATION:
                RC = UDFCommonSetVolInfo(IrpContext, Irp);
                break;
            default:
                UDFPrint(("  unhandled *** MJ: %x, Thr: %x\n", IrpContext->MajorFunction, PsGetCurrentThread()));
				if (!IrpContext->IrpCompleted) {
                Irp->IoStatus.Status = STATUS_INVALID_DEVICE_REQUEST;
                Irp->IoStatus.Information = 0;
			    IrpContext->IrpCompleted = TRUE;
                IoCompleteRequest(Irp, IO_NO_INCREMENT);
                UDFCleanupIrpContext(IrpContext);
                break;}
            }

            UDFPrint(("  *** Thr: %x  Done!\n", PsGetCurrentThread()));

        } _SEH2_EXCEPT(UDFExceptionFilter(IrpContext, _SEH2_GetExceptionInformation())) {
            RC = UDFProcessException(IrpContext, Irp);
            UDFLogEvent(UDF_ERROR_INTERNAL_ERROR, RC);
            // Only complete if not already completed
            if (!IrpContext->IrpCompleted && Irp) {
				IrpContext->IrpCompleted = TRUE;
                IoCompleteRequest(Irp, IO_NO_INCREMENT);
            }
        }  _SEH2_END;

        FsRtlExitFileSystem();
        IoSetTopLevelIrp(NULL);

        // Overflow queue handling
        if (!Vcb) {
            BrutePoint();
            break;
        }

        KeAcquireSpinLock(&(Vcb->OverflowQueueSpinLock), &SavedIrql);
        SpinLock = TRUE;
        if (!Vcb->OverflowQueueCount) {
            KeReleaseSpinLock(&(Vcb->OverflowQueueSpinLock), SavedIrql);
            SpinLock = FALSE;
            break;
        }

        Vcb->OverflowQueueCount--;
        Entry = RemoveHeadList(&Vcb->OverflowQueue);

#ifdef UDF_DBG
        PIRP_CONTEXT qctx = CONTAINING_RECORD(Entry, IRP_CONTEXT, WorkQueueItem.List);
        UDFPrint(("UDFFspDispatch: Dequeued Entry=%p NodeTypeCode=0x%x NodeByteSize=0x%x OverflowQueueMagic=0x%x\n",
            qctx,
            qctx->NodeIdentifier.NodeTypeCode,
            qctx->NodeIdentifier.NodeByteSize,
            qctx->OverflowQueueMagic));
        ASSERT(qctx->NodeIdentifier.NodeTypeCode == UDF_NODE_TYPE_IRP_CONTEXT);
        ASSERT(qctx->NodeIdentifier.NodeByteSize == sizeof(IRP_CONTEXT));
        ASSERT(qctx->OverflowQueueMagic == UDF_OVERFLOWQ_MAGIC);
        qctx->OverflowQueueMagic = 0;
        // Optionally walk the queue...
#endif

        KeReleaseSpinLock(&(Vcb->OverflowQueueSpinLock), SavedIrql);
        SpinLock = FALSE;

        // Only now is it safe to clean up the previous context
        if (PrevIrpContext) {
            UDFCleanupIrpContext(PrevIrpContext);
            PrevIrpContext = NULL;
        }
        PrevIrpContext = IrpContext;
        IrpContext = CONTAINING_RECORD(Entry, IRP_CONTEXT, WorkQueueItem.List);
        // **Set to FALSE for the new IRP_CONTEXT before next loop**
        IrpContext->IrpCompleted = FALSE;
    }

    // Clean up the final context after loop exit
    if (PrevIrpContext) {
        UDFCleanupIrpContext(PrevIrpContext);
    } else if (IrpContext) {
        UDFCleanupIrpContext(IrpContext);
    }

    if (!SpinLock)
        KeAcquireSpinLock(&(Vcb->OverflowQueueSpinLock), &SavedIrql);
    Vcb->PostedRequestCount--;
    KeReleaseSpinLock(&(Vcb->OverflowQueueSpinLock), SavedIrql);

    UDFPrint(("  *** Thr: %x  ThCnt: %x  QCnt: %x  Terminated!\n", PsGetCurrentThread(), Vcb->PostedRequestCount, Vcb->OverflowQueueCount));
    return;
} // end UDFFspDispatch()


/*************************************************************************
*
* Function: UDFInitializeVCB()
*
* Description:
*   Perform the initialization for a VCB structure.
*
* Expected Interrupt Level (for execution) :
*
*   IRQL PASSIVE_LEVEL
*
* Return Value: status
*
*************************************************************************/
NTSTATUS
UDFInitializeVCB(
    IN PDEVICE_OBJECT PtrVolumeDeviceObject,
    IN PDEVICE_OBJECT PtrTargetDeviceObject,
    IN PVPB           PtrVPB
    )
{
    NTSTATUS RC = STATUS_SUCCESS;
    PVCB     Vcb = NULL;
    SHORT    i;

    BOOLEAN VCBResourceInit     = FALSE;
    BOOLEAN BitMapResource1Init = FALSE;
    BOOLEAN FcbListResourceInit = FALSE;
    BOOLEAN FileIdResourceInit  = FALSE;
    BOOLEAN DlocResourceInit    = FALSE;
    BOOLEAN DlocResource2Init   = FALSE;
    BOOLEAN FlushResourceInit   = FALSE;
    BOOLEAN PreallocResourceInit= FALSE;
    BOOLEAN IoResourceInit      = FALSE;

    Vcb = (PVCB)(PtrVolumeDeviceObject->DeviceExtension);

    _SEH2_TRY {
    // Zero it out (typically this has already been done by the I/O
    // Manager but it does not hurt to do it again)!
    RtlZeroMemory(Vcb, sizeof(VCB));

    // Initialize the signature fields
    Vcb->NodeIdentifier.NodeTypeCode = UDF_NODE_TYPE_VCB;
    Vcb->NodeIdentifier.NodeByteSize = sizeof(VCB);

    // Initialize the ERESOURCE object.
    RC = UDFInitializeResourceLite(&(Vcb->VCBResource));
    if(!NT_SUCCESS(RC))
        try_return(RC);
    VCBResourceInit = TRUE;

    RC = UDFInitializeResourceLite(&(Vcb->BitMapResource1));
    if(!NT_SUCCESS(RC))
        try_return(RC);
    BitMapResource1Init = TRUE;

    RC = UDFInitializeResourceLite(&(Vcb->FcbListResource));
    if(!NT_SUCCESS(RC))
        try_return(RC);
    FcbListResourceInit = TRUE;

    RC = UDFInitializeResourceLite(&(Vcb->FileIdResource));
    if(!NT_SUCCESS(RC))
        try_return(RC);
    FileIdResourceInit = TRUE;

    RC = UDFInitializeResourceLite(&(Vcb->DlocResource));
    if(!NT_SUCCESS(RC))
        try_return(RC);
    DlocResourceInit = TRUE;

    RC = UDFInitializeResourceLite(&(Vcb->DlocResource2));
    if(!NT_SUCCESS(RC))
        try_return(RC);
    DlocResource2Init = TRUE;

    RC = UDFInitializeResourceLite(&(Vcb->FlushResource));
    if(!NT_SUCCESS(RC))
        try_return(RC);
    FlushResourceInit = TRUE;

    RC = UDFInitializeResourceLite(&(Vcb->PreallocResource));
    if(!NT_SUCCESS(RC))
        try_return(RC);
    PreallocResourceInit = TRUE;

    RC = UDFInitializeResourceLite(&(Vcb->IoResource));
    if(!NT_SUCCESS(RC))
        try_return(RC);
    IoResourceInit = TRUE;

//    RC = UDFInitializeResourceLite(&(Vcb->DelayedCloseResource));
//    ASSERT(NT_SUCCESS(RC));

    // Allocate buffer for statistics
    Vcb->Statistics = (PFILE_SYSTEM_STATISTICS)MyAllocatePool__(NonPagedPool, sizeof(FILE_SYSTEM_STATISTICS) * KeNumberProcessors );
    if(!Vcb->Statistics)
        try_return(RC = STATUS_INSUFFICIENT_RESOURCES);
    RtlZeroMemory( Vcb->Statistics, sizeof(FILE_SYSTEM_STATISTICS) * KeNumberProcessors );
    for (i=0; i < (KeNumberProcessors); i++) {
        Vcb->Statistics[i].Common.FileSystemType = FILESYSTEM_STATISTICS_TYPE_NTFS;
        Vcb->Statistics[i].Common.Version = 1;
        Vcb->Statistics[i].Common.SizeOfCompleteStructure =
            sizeof(FILE_SYSTEM_STATISTICS);
    }

    // Pick up a VPB right now so we know we can pull this filesystem stack off
    // of the storage stack on demand.
    Vcb->SwapVpb = (PVPB)FsRtlAllocatePoolWithTag(NonPagedPoolNx, sizeof(VPB), TAG_VPB);

    if(!Vcb->SwapVpb) {
        try_return(RC = STATUS_INSUFFICIENT_RESOURCES);
    }

    RtlZeroMemory(Vcb->SwapVpb, sizeof(VPB));

    // We know the target device object.
    // Note that this is not neccessarily a pointer to the actual
    // physical/virtual device on which the logical volume should
    // be mounted. This is actually a pointer to either the actual
    // (real) device or to any device object that may have been
    // attached to it. Any IRPs that we send down should be sent to this
    // device object. However, the "real" physical/virtual device object
    // on which we perform our mount operation can be determined from the
    // RealDevice field in the VPB sent to us.
    Vcb->TargetDeviceObject = PtrTargetDeviceObject;

    // We also have a pointer to the newly created device object representing
    // this logical volume (remember that this VCB structure is simply an
    // extension of the created device object).
    Vcb->VCBDeviceObject = PtrVolumeDeviceObject;

    // We also have the VPB pointer. This was obtained from the
    // Parameters.MountVolume.Vpb field in the current I/O stack location
    // for the mount IRP.
    Vcb->Vpb = PtrVPB;
    // Target Vcb field in Vcb onto itself. This required for check in
    // open/lock/unlock volume dispatch poits
    Vcb->Vcb=Vcb;

    //  Set the removable media flag based on the real device's
    //  characteristics
    if (PtrVPB->RealDevice->Characteristics & FILE_REMOVABLE_MEDIA) {
        Vcb->VCBFlags |= VCB_STATE_REMOVABLE_MEDIA;
    }

    // Initialize the list anchor (head) for some lists in this VCB.
    InitializeListHead(&(Vcb->NextFCB));
    InitializeListHead(&(Vcb->NextNotifyIRP));
    InitializeListHead(&(Vcb->NextCCB));

    //  Initialize the overflow queue for the volume
    Vcb->OverflowQueueCount = 0;
    InitializeListHead(&(Vcb->OverflowQueue));

    Vcb->PostedRequestCount = 0;
    KeInitializeSpinLock(&(Vcb->OverflowQueueSpinLock));

    // Initialize the notify IRP list mutex
    FsRtlNotifyInitializeSync(&(Vcb->NotifyIRPMutex));

    // Intilize FCB for this VCB

    // Set the initial file size values appropriately. Note that our FSD may
    // wish to guess at the initial amount of information we would like to
    // read from the disk until we have really determined that this a valid
    // logical volume (on disk) that we wish to mount.
    // Vcb->FileSize = Vcb->AllocationSize = ??

    // We do not want to bother with valid data length callbacks
    // from the Cache Manager for the file stream opened for volume metadata
    // information
    Vcb->Header.ValidDataLength.QuadPart = 0x7FFFFFFFFFFFFFFFULL;

    Vcb->VolumeLockPID = -1;

    Vcb->VCBOpenCount = 1;

    Vcb->WCacheMaxBlocks        = UDFGlobalData.WCacheMaxBlocks;
    Vcb->WCacheMaxFrames        = UDFGlobalData.WCacheMaxFrames;
    Vcb->WCacheBlocksPerFrameSh = UDFGlobalData.WCacheBlocksPerFrameSh;
    Vcb->WCacheFramesToKeepFree = UDFGlobalData.WCacheFramesToKeepFree;

    // Create a stream file object for this volume.
    //Vcb->PtrStreamFileObject = IoCreateStreamFileObject(NULL,
    //                                            Vcb->Vpb->RealDevice);
    //ASSERT(Vcb->PtrStreamFileObject);

    // Initialize some important fields in the newly created file object.
    //Vcb->PtrStreamFileObject->FsContext = (PVOID)Vcb;
    //Vcb->PtrStreamFileObject->FsContext2 = NULL;
    //Vcb->PtrStreamFileObject->SectionObjectPointer = &(Vcb->SectionObject);

    //Vcb->PtrStreamFileObject->Vpb = PtrVPB;

    // Link this chap onto the global linked list of all VCB structures.
    // We consider that GlobalDataResource was acquired in past
    UDFAcquireResourceExclusive(&(UDFGlobalData.GlobalDataResource), TRUE);
    InsertTailList(&(UDFGlobalData.VCBQueue), &(Vcb->NextVCB));

    // Initialize caching for the stream file object.
    //CcInitializeCacheMap(Vcb->PtrStreamFileObject, (PCC_FILE_SIZES)(&(Vcb->AllocationSize)),
    //                            TRUE,       // We will use pinned access.
    //                            &(UDFGlobalData.CacheMgrCallBacks), Vcb);

    UDFReleaseResource(&(UDFGlobalData.GlobalDataResource));

    // Mark the fact that this VCB structure is initialized.
    Vcb->VCBFlags |= VCB_STATE_VCB_INITIALIZED;

    RC = STATUS_SUCCESS;

try_exit:   NOTHING;

    } _SEH2_FINALLY {

        if(!NT_SUCCESS(RC)) {

            if(Vcb->Statistics)
                MyFreePool__(Vcb->Statistics);

            if(VCBResourceInit)
                UDFDeleteResource(&(Vcb->VCBResource));
            if(BitMapResource1Init)
                UDFDeleteResource(&(Vcb->BitMapResource1));
            if(FcbListResourceInit)
                UDFDeleteResource(&(Vcb->FcbListResource));
            if(FileIdResourceInit)
                UDFDeleteResource(&(Vcb->FileIdResource));
            if(DlocResourceInit)
                UDFDeleteResource(&(Vcb->DlocResource));
            if(DlocResource2Init)
                UDFDeleteResource(&(Vcb->DlocResource2));
            if(FlushResourceInit)
                UDFDeleteResource(&(Vcb->FlushResource));
            if(PreallocResourceInit)
                UDFDeleteResource(&(Vcb->PreallocResource));
            if(IoResourceInit)
                UDFDeleteResource(&(Vcb->IoResource));
        }
    } _SEH2_END;

    return RC;
} // end UDFInitializeVCB()

typedef ULONG
(*ptrUDFGetParameter)(
    IN PVCB Vcb,
    IN PCWSTR Name,
    IN ULONG DefValue
    );

VOID
UDFUpdateCompatOption(
    PVCB Vcb,
    BOOLEAN Update,
    BOOLEAN UseCfg,
    PCWSTR Name,
    ULONG Flag,
    BOOLEAN Default
    )
{
    ptrUDFGetParameter UDFGetParameter = UseCfg ? UDFGetCfgParameter : UDFGetRegParameter;

    if(UDFGetParameter(Vcb, Name, Update ? ((Vcb->CompatFlags & Flag) ? TRUE : FALSE) : Default)) {
        Vcb->CompatFlags |= Flag;
    } else {
        Vcb->CompatFlags &= ~Flag;
    }
} // end UDFUpdateCompatOption()

VOID
UDFReadRegKeys(
    PVCB Vcb,
    BOOLEAN Update,
    BOOLEAN UseCfg
    )
{
    ULONG mult = 1;
    ptrUDFGetParameter UDFGetParameter = UseCfg ? UDFGetCfgParameter : UDFGetRegParameter;

    Vcb->DefaultRegName = REG_DEFAULT_UNKNOWN;

    // Should we use Extended FE by default ?
    Vcb->UseExtendedFE = (UCHAR)UDFGetParameter(Vcb, REG_USEEXTENDEDFE_NAME,
        Update ? Vcb->UseExtendedFE : FALSE);
    // What type of AllocDescs should we use
    Vcb->DefaultAllocMode = (USHORT)UDFGetParameter(Vcb, REG_DEFALLOCMODE_NAME,
        Update ? Vcb->DefaultAllocMode : ICB_FLAG_AD_SHORT);
    if(Vcb->DefaultAllocMode > ICB_FLAG_AD_LONG) Vcb->DefaultAllocMode = ICB_FLAG_AD_SHORT;
    // Default UID & GID to be set on newly created files
    Vcb->DefaultUID = UDFGetParameter(Vcb, UDF_DEFAULT_UID_NAME, Update ? Vcb->DefaultUID : -1);
    Vcb->DefaultGID = UDFGetParameter(Vcb, UDF_DEFAULT_GID_NAME, Update ? Vcb->DefaultGID : -1);
    // FE allocation charge for plain Dirs
    Vcb->FECharge = UDFGetParameter(Vcb, UDF_FE_CHARGE_NAME, Update ? Vcb->FECharge : 0);
    if(!Vcb->FECharge)
        Vcb->FECharge = UDF_DEFAULT_FE_CHARGE;
    // FE allocation charge for Stream Dirs (SDir)
    Vcb->FEChargeSDir = UDFGetParameter(Vcb, UDF_FE_CHARGE_SDIR_NAME,
        Update ? Vcb->FEChargeSDir : 0);
    if(!Vcb->FEChargeSDir)
        Vcb->FEChargeSDir = UDF_DEFAULT_FE_CHARGE_SDIR;
    // How many Deleted entries should contain Directory to make us
    // start packing it.
    Vcb->PackDirThreshold = UDFGetParameter(Vcb, UDF_DIR_PACK_THRESHOLD_NAME,
        Update ? Vcb->PackDirThreshold : 0);
    if(Vcb->PackDirThreshold == 0xffffffff)
        Vcb->PackDirThreshold = UDF_DEFAULT_DIR_PACK_THRESHOLD;
    // The binary exponent for the number of Pages to be read-ahead'ed
    // This information would be sent to System Cache Manager
    if(!Update) {
        Vcb->SystemCacheGran = (1 << UDFGetParameter(Vcb, UDF_READAHEAD_GRAN_NAME, 0)) * PAGE_SIZE;
        if(!Vcb->SystemCacheGran)
            Vcb->SystemCacheGran = UDF_DEFAULT_READAHEAD_GRAN;
    }
    // Timeouts for FreeSpaceBitMap & TheWholeDirTree flushes
    Vcb->BM_FlushPriod = UDFGetParameter(Vcb, UDF_BM_FLUSH_PERIOD_NAME,
        Update ? Vcb->BM_FlushPriod : 0);
    if(!Vcb->BM_FlushPriod) {
        Vcb->BM_FlushPriod = UDF_DEFAULT_BM_FLUSH_TIMEOUT;
    } else
    if(Vcb->BM_FlushPriod == (ULONG)-1) {
        Vcb->BM_FlushPriod = 0;
    }
    Vcb->Tree_FlushPriod = UDFGetParameter(Vcb, UDF_TREE_FLUSH_PERIOD_NAME,
        Update ? Vcb->Tree_FlushPriod : 0);
    if(!Vcb->Tree_FlushPriod) {
        Vcb->Tree_FlushPriod = UDF_DEFAULT_TREE_FLUSH_TIMEOUT;
    } else
    if(Vcb->Tree_FlushPriod == (ULONG)-1) {
        Vcb->Tree_FlushPriod = 0;
    }
    Vcb->SkipCountLimit = UDFGetParameter(Vcb, UDF_NO_UPDATE_PERIOD_NAME,
        Update ? Vcb->SkipCountLimit : 0);
    if(!Vcb->SkipCountLimit)
        Vcb->SkipCountLimit = -1;

    Vcb->SkipEjectCountLimit = UDFGetParameter(Vcb, UDF_NO_EJECT_PERIOD_NAME,
        Update ? Vcb->SkipEjectCountLimit : 3);

    if(!Update) {
        // How many threads are allowed to sodomize Disc simultaneously on each CPU
        Vcb->ThreadsPerCpu = UDFGetParameter(Vcb, UDF_FSP_THREAD_PER_CPU_NAME,
            Update ? Vcb->ThreadsPerCpu : 2);
        if(Vcb->ThreadsPerCpu < 2)
            Vcb->ThreadsPerCpu = UDF_DEFAULT_FSP_THREAD_PER_CPU;
    }
    // The mimimum FileSize increment when we'll decide not to allocate
    // on-disk space.
    Vcb->SparseThreshold = UDFGetParameter(Vcb, UDF_SPARSE_THRESHOLD_NAME,
        Update ? Vcb->SparseThreshold : 0);
    if(!Vcb->SparseThreshold)
        Vcb->SparseThreshold = UDF_DEFAULT_SPARSE_THRESHOLD;
    // This option is used to VERIFY all the data written. It decreases performance
    Vcb->VerifyOnWrite = UDFGetParameter(Vcb, UDF_VERIFY_ON_WRITE_NAME,
        Update ? Vcb->VerifyOnWrite : FALSE) ? TRUE : FALSE;

    // Should we update AttrFileTime on Attr changes
    UDFUpdateCompatOption(Vcb, Update, UseCfg, UDF_UPDATE_TIMES_ATTR, UDF_VCB_IC_UPDATE_ATTR_TIME, FALSE);
    // Should we update ModifyFileTime on Writes changes
    // It also affects ARCHIVE bit setting on write operations
    UDFUpdateCompatOption(Vcb, Update, UseCfg, UDF_UPDATE_TIMES_MOD, UDF_VCB_IC_UPDATE_MODIFY_TIME, FALSE);
    // Should we update AccessFileTime on Exec & so on.
    UDFUpdateCompatOption(Vcb, Update, UseCfg, UDF_UPDATE_TIMES_ACCS, UDF_VCB_IC_UPDATE_ACCESS_TIME, FALSE);
    // Should we update Archive bit
    UDFUpdateCompatOption(Vcb, Update, UseCfg, UDF_UPDATE_ATTR_ARCH, UDF_VCB_IC_UPDATE_ARCH_BIT, FALSE);
    // Should we update Dir's Times & Attrs on Modify
    UDFUpdateCompatOption(Vcb, Update, UseCfg, UDF_UPDATE_DIR_TIMES_ATTR_W, UDF_VCB_IC_UPDATE_DIR_WRITE, FALSE);
    // Should we update Dir's Times & Attrs on Access
    UDFUpdateCompatOption(Vcb, Update, UseCfg, UDF_UPDATE_DIR_TIMES_ATTR_R, UDF_VCB_IC_UPDATE_DIR_READ, FALSE);
    // Should we allow user to write into Read-Only Directory
    UDFUpdateCompatOption(Vcb, Update, UseCfg, UDF_ALLOW_WRITE_IN_RO_DIR, UDF_VCB_IC_WRITE_IN_RO_DIR, TRUE);
    // Should we allow user to change Access Time for unchanged Directory
    UDFUpdateCompatOption(Vcb, Update, UseCfg, UDF_ALLOW_UPDATE_TIMES_ACCS_UCHG_DIR, UDF_VCB_IC_UPDATE_UCHG_DIR_ACCESS_TIME, FALSE);
    // Should we record Allocation Descriptors in W2k-compatible form
    UDFUpdateCompatOption(Vcb, Update, UseCfg, UDF_W2K_COMPAT_ALLOC_DESCS, UDF_VCB_IC_W2K_COMPAT_ALLOC_DESCS, TRUE);
    // Should we read LONG_ADs with invalid PartitionReferenceNumber (generated by Nero Instant Burner)
    UDFUpdateCompatOption(Vcb, Update, UseCfg, UDF_INSTANT_COMPAT_ALLOC_DESCS, UDF_VCB_IC_INSTANT_COMPAT_ALLOC_DESCS, TRUE);
    // Should we make a copy of VolumeLabel in LVD
    // usually only PVD is updated
    UDFUpdateCompatOption(Vcb, Update, UseCfg, UDF_W2K_COMPAT_VLABEL, UDF_VCB_IC_W2K_COMPAT_VLABEL, TRUE);
    // Should we handle or ignore HW_RO flag
    UDFUpdateCompatOption(Vcb, Update, UseCfg, UDF_HANDLE_HW_RO, UDF_VCB_IC_HW_RO, FALSE);
    // Should we handle or ignore SOFT_RO flag
    UDFUpdateCompatOption(Vcb, Update, UseCfg, UDF_HANDLE_SOFT_RO, UDF_VCB_IC_SOFT_RO, TRUE);

    // Check if we should generate UDF-style or OS-style DOS-names
    UDFUpdateCompatOption(Vcb, Update, UseCfg, UDF_OS_NATIVE_DOS_NAME, UDF_VCB_IC_OS_NATIVE_DOS_NAME, FALSE);
    // should we force FO_WRITE_THROUGH on removable media
    UDFUpdateCompatOption(Vcb, Update, UseCfg, UDF_FORCE_WRITE_THROUGH_NAME, UDF_VCB_IC_FORCE_WRITE_THROUGH,
                          (Vcb->TargetDeviceObject->Characteristics & FILE_REMOVABLE_MEDIA) ? TRUE : FALSE
                         );
    // Should we ignore FO_SEQUENTIAL_ONLY
    UDFUpdateCompatOption(Vcb, Update, UseCfg, UDF_IGNORE_SEQUENTIAL_IO, UDF_VCB_IC_IGNORE_SEQUENTIAL_IO, FALSE);
// Force Read-only mounts
    UDFUpdateCompatOption(Vcb, Update, UseCfg, UDF_FORCE_HW_RO, UDF_VCB_IC_FORCE_HW_RO, FALSE);
    // Check if we should send FLUSH request for File/Dir down to
    // underlaying driver
    if(UDFGetParameter(Vcb, UDF_FLUSH_MEDIA,Update ? Vcb->FlushMedia : FALSE)) {
        Vcb->FlushMedia = TRUE;
    } else {
        Vcb->FlushMedia = FALSE;
    }
    // compare data from packet with data to be writen there
    // before physical writing
    if(!UDFGetParameter(Vcb, UDF_COMPARE_BEFORE_WRITE, Update ? Vcb->DoNotCompareBeforeWrite : FALSE)) {
        Vcb->DoNotCompareBeforeWrite = TRUE;
    } else {
        Vcb->DoNotCompareBeforeWrite = FALSE;
    }
    if(!Update)  {
        if(UDFGetParameter(Vcb, UDF_CHAINED_IO, TRUE)) {
            Vcb->CacheChainedIo = TRUE;
        }

        // Should we show Blank.Cd file on damaged/unformatted,
        // but UDF-compatible disks
        Vcb->ShowBlankCd = (UCHAR)UDFGetParameter(Vcb, UDF_SHOW_BLANK_CD, FALSE);
        if(Vcb->ShowBlankCd) {
            Vcb->CompatFlags |= UDF_VCB_IC_SHOW_BLANK_CD;
            if(Vcb->ShowBlankCd > 2) {
                Vcb->ShowBlankCd = 2;
            }
        }
        // Should we wait util CD device return from
        // Becoming Ready state
        if(UDFGetParameter(Vcb, UDF_WAIT_CD_SPINUP, TRUE)) {
            Vcb->CompatFlags |= UDF_VCB_IC_WAIT_CD_SPINUP;
        }
        // Should we remenber bad VDS locations during mount
        // Caching will improve mount performance on bad disks, but
        // will degrade mauntability of unreliable discs
        if(UDFGetParameter(Vcb, UDF_CACHE_BAD_VDS, TRUE)) {
            Vcb->CompatFlags |= UDF_VCB_IC_CACHE_BAD_VDS;
        }

        // Set partitially damaged volume mount mode
        Vcb->PartitialDamagedVolumeAction = (UCHAR)UDFGetParameter(Vcb, UDF_PART_DAMAGED_BEHAVIOR, UDF_PART_DAMAGED_RW);
        if(Vcb->PartitialDamagedVolumeAction > 2) {
            Vcb->PartitialDamagedVolumeAction = UDF_PART_DAMAGED_RW;
        }

        // Set partitially damaged volume mount mode
        Vcb->NoFreeRelocationSpaceVolumeAction = (UCHAR)UDFGetParameter(Vcb, UDF_NO_SPARE_BEHAVIOR, UDF_PART_DAMAGED_RW);
        if(Vcb->NoFreeRelocationSpaceVolumeAction > 1) {
            Vcb->NoFreeRelocationSpaceVolumeAction = UDF_PART_DAMAGED_RW;
        }

        // Set dirty volume mount mode
        if(UDFGetParameter(Vcb, UDF_DIRTY_VOLUME_BEHAVIOR, UDF_PART_DAMAGED_RO)) {
            Vcb->CompatFlags |= UDF_VCB_IC_DIRTY_RO;
        }

        mult = UDFGetParameter(Vcb, UDF_CACHE_SIZE_MULTIPLIER, 1);
        if(!mult) mult = 1;
        Vcb->WCacheMaxBlocks *= mult;
        Vcb->WCacheMaxFrames *= mult;
    }
    return;
} // end UDFReadRegKeys()

ULONG
UDFGetRegParameter(
    IN PVCB Vcb,
    IN PCWSTR Name,
    IN ULONG DefValue
    )
{
    return UDFRegCheckParameterValue(&(UDFGlobalData.SavedRegPath),
                                     Name,
                                     NULL,
                                     Vcb ? Vcb->DefaultRegName : NULL,
                                     DefValue);
} // end UDFGetRegParameter()

ULONG
UDFGetCfgParameter(
    IN PVCB Vcb,
    IN PCWSTR Name,
    IN ULONG DefValue
    )
{
    ULONG len;
    CHAR NameA[128];
    ULONG ret_val=0;
    CHAR a;
    BOOLEAN wait_name=TRUE;
    BOOLEAN wait_val=FALSE;
    BOOLEAN wait_nl=FALSE;
    ULONG radix=10;
    ULONG i;

    PUCHAR Cfg    = Vcb->Cfg;
    ULONG  Length = Vcb->CfgLength;

    if(!Cfg || !Length)
        return DefValue;

    len = wcslen(Name);
    if(len >= sizeof(NameA))
        return DefValue;
    sprintf(NameA, "%S", Name);

    for(i=0; i<Length; i++) {
        a=Cfg[i];
        switch(a) {
        case '\n':
        case '\r':
        case ',':
            if(wait_val)
                return DefValue;
            continue;
        case ';':
        case '#':
        case '[': // ignore sections for now, treat as comment
            if(!wait_name)
                return DefValue;
            wait_nl = TRUE;
            continue;
        case '=':
            if(!wait_val)
                return DefValue;
            continue;
        case ' ':
        case '\t':
            continue;
        default:
            if(wait_nl)
                continue;
        }
        if(wait_name) {
            if(i+len+2 > Length)
                return DefValue;
            if(RtlCompareMemory(Cfg+i, NameA, len) == len) {
                a=Cfg[i+len];
                switch(a) {
                case '\n':
                case '\r':
                case ',':
                case ';':
                case '#':
                    return DefValue;
                case '=':
                case ' ':
                case '\t':
                    break;
                default:
                    wait_nl = TRUE;
                    wait_val = FALSE;
                    i+=len;
                    continue;
                }
                wait_name = FALSE;
                wait_nl = FALSE;
                wait_val = TRUE;
                i+=len;

            } else {
                wait_nl = TRUE;
            }
            continue;
        }
        if(wait_val) {
            if(i+3 > Length) {
                if(a=='0' && Cfg[i+1]=='x') {
                    i+=2;
                    radix=16;
                }
            }
            if(i >= Length) {
                return DefValue;
            }
            while(i<Length) {
                a=Cfg[i];
                switch(a) {
                case '\n':
                case '\r':
                case ' ':
                case '\t':
                case ',':
                case ';':
                case '#':
                    if(wait_val)
                        return DefValue;
                    return ret_val;
                }
                if(a >= '0' && a <= '9') {
                    a -= '0';
                } else {
                    if(radix != 16)
                        return DefValue;
                    if(a >= 'a' && a <= 'f') {
                        a -= 'a';
                    } else
                    if(a >= 'A' && a <= 'F') {
                        a -= 'A';
                    } else {
                        return DefValue;
                    }
                    a += 0x0a;
                }
                ret_val = ret_val*radix + a;
                wait_val = FALSE;
                i++;
            }
            return ret_val;
        }
    }
    return DefValue;

} // end UDFGetCfgParameter()

VOID
UDFDeleteVCB(
    PVCB  Vcb
    )
{
    LARGE_INTEGER delay;
    UDFPrint(("UDFDeleteVCB\n"));

    delay.QuadPart = -500000; // 0.05 sec
    while(Vcb->PostedRequestCount) {
        UDFPrint(("UDFDeleteVCB: PostedRequestCount = %d\n", Vcb->PostedRequestCount));
        // spin until all queues IRPs are processed
        KeDelayExecutionThread(KernelMode, FALSE, &delay);
        delay.QuadPart -= 500000; // grow delay 0.05 sec
    }

    _SEH2_TRY {
        UDFPrint(("UDF: Flushing buffers\n"));
        UDFVRelease(Vcb);
        WCacheFlushAll__(&(Vcb->FastCache),Vcb);
        WCacheRelease__(&(Vcb->FastCache));

    } _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        BrutePoint();
    } _SEH2_END;

#ifdef UDF_DBG
    _SEH2_TRY {
        if (!ExIsResourceAcquiredShared(&UDFGlobalData.GlobalDataResource)) {
            UDFPrint(("UDF: attempt to access to not protected data\n"));
            UDFPrint(("UDF: UDFGlobalData\n"));
            BrutePoint();
        }
    } _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        BrutePoint();
    } _SEH2_END;
#endif

    _SEH2_TRY {
        RemoveEntryList(&(Vcb->NextVCB));
    } _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        BrutePoint();
    } _SEH2_END;

    _SEH2_TRY {
        UDFPrint(("UDF: Delete resources\n"));
        UDFDeleteResource(&(Vcb->VCBResource));
        UDFDeleteResource(&(Vcb->BitMapResource1));
        UDFDeleteResource(&(Vcb->FcbListResource));
        UDFDeleteResource(&(Vcb->FileIdResource));
        UDFDeleteResource(&(Vcb->DlocResource));
        UDFDeleteResource(&(Vcb->DlocResource2));
        UDFDeleteResource(&(Vcb->FlushResource));
        UDFDeleteResource(&(Vcb->PreallocResource));
        UDFDeleteResource(&(Vcb->IoResource));
    } _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        BrutePoint();
    } _SEH2_END;

    _SEH2_TRY {
        UDFPrint(("UDF: Cleanup VCB\n"));
        ASSERT(IsListEmpty(&(Vcb->NextNotifyIRP)));
        FsRtlNotifyUninitializeSync(&(Vcb->NotifyIRPMutex));
        UDFCleanupVCB(Vcb);
    } _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        BrutePoint();
    } _SEH2_END;

    // Chuck the backpocket Vpb we kept just in case.
    UDFFreePool((PVOID*)&Vcb->SwapVpb);

    // If there is a Vpb then we must delete it ourselves.
    UDFFreePool((PVOID*)&Vcb->Vpb);

    _SEH2_TRY {
        UDFPrint(("UDF: Delete DO\n"));
        IoDeleteDevice(Vcb->VCBDeviceObject);
    } _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        BrutePoint();
    } _SEH2_END;

} // end UDFDeleteVCB()

/*
    Read DWORD from Registry
*/
ULONG
UDFRegCheckParameterValue(
    IN PUNICODE_STRING RegistryPath,
    IN PCWSTR Name,
    IN PUNICODE_STRING PtrVolumePath,
    IN PCWSTR DefaultPath,
    IN ULONG DefValue
    )
{
    NTSTATUS          status;

    ULONG             val = DefValue;

    UNICODE_STRING    paramStr;
    UNICODE_STRING    defaultParamStr;
    UNICODE_STRING    paramPathUnknownStr;

    UNICODE_STRING    paramSuffix;
    UNICODE_STRING    paramPath;
    UNICODE_STRING    paramPathUnknown;
    UNICODE_STRING    paramDevPath;
    UNICODE_STRING    defaultParamPath;

    _SEH2_TRY {

        paramPath.Buffer = NULL;
        paramDevPath.Buffer = NULL;
        paramPathUnknown.Buffer = NULL;
        defaultParamPath.Buffer = NULL;

        // First append \Parameters to the passed in registry path
        // Note, RtlInitUnicodeString doesn't allocate memory
        RtlInitUnicodeString(&paramStr, L"\\Parameters");
        RtlInitUnicodeString(&paramPath, NULL);

        RtlInitUnicodeString(&paramPathUnknownStr, REG_DEFAULT_UNKNOWN);
        RtlInitUnicodeString(&paramPathUnknown, NULL);

        paramPathUnknown.MaximumLength = RegistryPath->Length + paramPathUnknownStr.Length + paramStr.Length + sizeof(WCHAR);
        paramPath.MaximumLength = RegistryPath->Length + paramStr.Length + sizeof(WCHAR);

        paramPath.Buffer = (PWCH)MyAllocatePool__(PagedPool, paramPath.MaximumLength);
        if(!paramPath.Buffer) {
            UDFPrint(("UDFCheckRegValue: couldn't allocate paramPath\n"));
            try_return(val = DefValue);
        }
        paramPathUnknown.Buffer = (PWCH)MyAllocatePool__(PagedPool, paramPathUnknown.MaximumLength);
        if(!paramPathUnknown.Buffer) {
            UDFPrint(("UDFCheckRegValue: couldn't allocate paramPathUnknown\n"));
            try_return(val = DefValue);
        }

        RtlZeroMemory(paramPath.Buffer, paramPath.MaximumLength);
        status = RtlAppendUnicodeToString(&paramPath, RegistryPath->Buffer);
        if(!NT_SUCCESS(status)) {
            try_return(val = DefValue);
        }
        status = RtlAppendUnicodeToString(&paramPath, paramStr.Buffer);
        if(!NT_SUCCESS(status)) {
            try_return(val = DefValue);
        }
        UDFPrint(("UDFCheckRegValue: (1) |%S|\n", paramPath.Buffer));

        RtlZeroMemory(paramPathUnknown.Buffer, paramPathUnknown.MaximumLength);
        status = RtlAppendUnicodeToString(&paramPathUnknown, RegistryPath->Buffer);
        if(!NT_SUCCESS(status)) {
            try_return(val = DefValue);
        }
        status = RtlAppendUnicodeToString(&paramPathUnknown, paramStr.Buffer);
        if(!NT_SUCCESS(status)) {
            try_return(val = DefValue);
        }
        status = RtlAppendUnicodeToString(&paramPathUnknown, paramPathUnknownStr.Buffer);
        if(!NT_SUCCESS(status)) {
            try_return(val = DefValue);
        }
        UDFPrint(("UDFCheckRegValue: (2) |%S|\n", paramPathUnknown.Buffer));

        // First append \Parameters\Default_XXX to the passed in registry path
        if(DefaultPath) {
            RtlInitUnicodeString(&defaultParamStr, DefaultPath);
            RtlInitUnicodeString(&defaultParamPath, NULL);
            defaultParamPath.MaximumLength = paramPath.Length + defaultParamStr.Length + sizeof(WCHAR);
            defaultParamPath.Buffer = (PWCH)MyAllocatePool__(PagedPool, defaultParamPath.MaximumLength);
            if(!defaultParamPath.Buffer) {
                UDFPrint(("UDFCheckRegValue: couldn't allocate defaultParamPath\n"));
                try_return(val = DefValue);
            }

            RtlZeroMemory(defaultParamPath.Buffer, defaultParamPath.MaximumLength);
            status = RtlAppendUnicodeToString(&defaultParamPath, paramPath.Buffer);
            if(!NT_SUCCESS(status)) {
                try_return(val = DefValue);
            }
            status = RtlAppendUnicodeToString(&defaultParamPath, defaultParamStr.Buffer);
            if(!NT_SUCCESS(status)) {
                try_return(val = DefValue);
            }
            UDFPrint(("UDFCheckRegValue: (3) |%S|\n", defaultParamPath.Buffer));
        }

        if(PtrVolumePath) {
            paramSuffix = *PtrVolumePath;
        } else {
            RtlInitUnicodeString(&paramSuffix, NULL);
        }

        RtlInitUnicodeString(&paramDevPath, NULL);
        // now build the device specific path
        paramDevPath.MaximumLength = paramPath.Length + paramSuffix.Length + sizeof(WCHAR);
        paramDevPath.Buffer = (PWCH)MyAllocatePool__(PagedPool, paramDevPath.MaximumLength);
        if(!paramDevPath.Buffer) {
            try_return(val = DefValue);
        }

        RtlZeroMemory(paramDevPath.Buffer, paramDevPath.MaximumLength);
        status = RtlAppendUnicodeToString(&paramDevPath, paramPath.Buffer);
        if(!NT_SUCCESS(status)) {
            try_return(val = DefValue);
        }
        if(paramSuffix.Buffer) {
            status = RtlAppendUnicodeToString(&paramDevPath, paramSuffix.Buffer);
            if(!NT_SUCCESS(status)) {
                try_return(val = DefValue);
            }
        }

        UDFPrint(( " Parameter = %ws\n", Name));

        {
            HKEY hk = NULL;
            status = RegTGetKeyHandle(NULL, RegistryPath->Buffer, &hk);
            if(NT_SUCCESS(status)) {
                RegTCloseKeyHandle(hk);
            }
        }


        // *** Read GLOBAL_DEFAULTS from
        // "\DwUdf\Parameters_Unknown\"

        status = RegTGetDwordValue(NULL, paramPath.Buffer, Name, &val);

        // *** Read DEV_CLASS_SPEC_DEFAULTS (if any) from
        // "\DwUdf\Parameters_%DevClass%\"

        if(DefaultPath) {
            status = RegTGetDwordValue(NULL, defaultParamPath.Buffer, Name, &val);
        }

        // *** Read DEV_SPEC_PARAMS from (if device supports GetDevName)
        // "\DwUdf\Parameters\%DevName%\"

        status = RegTGetDwordValue(NULL, paramDevPath.Buffer, Name, &val);

try_exit:   NOTHING;

    } _SEH2_FINALLY {

        if(DefaultPath && defaultParamPath.Buffer) {
            MyFreePool__(defaultParamPath.Buffer);
        }
        if(paramPath.Buffer) {
            MyFreePool__(paramPath.Buffer);
        }
        if(paramDevPath.Buffer) {
            MyFreePool__(paramDevPath.Buffer);
        }
        if(paramPathUnknown.Buffer) {
            MyFreePool__(paramPathUnknown.Buffer);
        }
    } _SEH2_END;

    UDFPrint(( "UDFCheckRegValue: %ws for drive %s is %x\n\n", Name, PtrVolumePath, val));
    return val;
} // end UDFRegCheckParameterValue()

/*
Routine Description:
    This routine is called to initialize an IrpContext for the current
    UDFFS request.  The IrpContext is on the stack and we need to initialize
    it for the current request.  The request is a close operation.

Arguments:

    IrpContext - IrpContext to initialize.

    IrpContextLite - source for initialization

Return Value:

    None

*/
VOID
UDFInitializeStackIrpContextFromLite(
    OUT PIRP_CONTEXT IrpContext,
    IN PIRP_CONTEXT_LITE IrpContextLite
    )
{
    ASSERT(IrpContextLite->NodeIdentifier.NodeTypeCode == UDF_NODE_TYPE_IRP_CONTEXT_LITE);
    ASSERT(IrpContextLite->NodeIdentifier.NodeByteSize == sizeof(IRP_CONTEXT_LITE));

    // Zero and then initialize the structure.
    RtlZeroMemory(IrpContext, sizeof(IRP_CONTEXT));
#ifdef UDF_DBG
    IrpContext->OverflowQueueMagic = 0;
#endif

    // Set the proper node type code and node byte size
    IrpContext->NodeIdentifier.NodeTypeCode = UDF_NODE_TYPE_IRP_CONTEXT;
    IrpContext->NodeIdentifier.NodeByteSize = sizeof(IRP_CONTEXT);

    //  Major/Minor Function codes
    IrpContext->MajorFunction = IRP_MJ_CLOSE;
    IrpContext->Vcb = IrpContextLite->Fcb->Vcb;
    IrpContext->Fcb = IrpContextLite->Fcb;
    IrpContext->TreeLength = IrpContextLite->TreeLength;
    IrpContext->TargetDeviceObject = IrpContextLite->RealDevice;

    // Note that this is from the stack.
    SetFlag(IrpContext->Flags, IRP_CONTEXT_FLAG_ON_STACK);

    // Set the wait parameter
    SetFlag(IrpContext->Flags, IRP_CONTEXT_FLAG_WAIT);

} // end UDFInitializeStackIrpContextFromLite()

/*
Routine Description:
    This routine is called to initialize an IrpContext for the current
    UDFFS request.  The IrpContext is on the stack and we need to initialize
    it for the current request.  The request is a close operation.

Arguments:

    IrpContext - IrpContext to initialize.

    IrpContextLite - source for initialization

Return Value:

    None

*/
NTSTATUS
UDFInitializeIrpContextLite(
    OUT PIRP_CONTEXT_LITE *IrpContextLite,
    IN PIRP_CONTEXT IrpContext,
    IN PFCB                Fcb
    )
{
    PIRP_CONTEXT_LITE LocalIrpContextLite = (PIRP_CONTEXT_LITE)MyAllocatePool__(NonPagedPool, sizeof(IRP_CONTEXT_LITE));
    if (!LocalIrpContextLite)
        return STATUS_INSUFFICIENT_RESOURCES;
    //  Zero and then initialize the structure.
    RtlZeroMemory(LocalIrpContextLite, sizeof(IRP_CONTEXT_LITE));

    LocalIrpContextLite->NodeIdentifier.NodeTypeCode = UDF_NODE_TYPE_IRP_CONTEXT_LITE;
    LocalIrpContextLite->NodeIdentifier.NodeByteSize = sizeof(IRP_CONTEXT_LITE);

    LocalIrpContextLite->Fcb = Fcb;
    LocalIrpContextLite->TreeLength = IrpContext->TreeLength;
    //  Copy RealDevice for workque algorithms.
    LocalIrpContextLite->RealDevice = IrpContext->TargetDeviceObject;
    *IrpContextLite = LocalIrpContextLite;

    return STATUS_SUCCESS;
} // end UDFInitializeIrpContextLite()

ULONG
UDFIsResourceAcquired(
    IN PERESOURCE Resource
    )
{
    ULONG ReAcqRes =
        ExIsResourceAcquiredExclusiveLite(Resource) ? 1 :
        (ExIsResourceAcquiredSharedLite(Resource) ? 2 : 0);
    return ReAcqRes;
} // end UDFIsResourceAcquired()

BOOLEAN
UDFAcquireResourceExclusiveWithCheck(
    IN PERESOURCE Resource
    )
{
    ULONG ReAcqRes =
        ExIsResourceAcquiredExclusiveLite(Resource) ? 1 :
        (ExIsResourceAcquiredSharedLite(Resource) ? 2 : 0);
    if(ReAcqRes) {
        UDFPrint(("UDFAcquireResourceExclusiveWithCheck: ReAcqRes, %x\n", ReAcqRes));
    } else {
//        BrutePoint();
    }

    if(ReAcqRes == 1) {
        // OK
    } else
    if(ReAcqRes == 2) {
        UDFPrint(("UDFAcquireResourceExclusiveWithCheck: !!! Shared !!!\n"));
        //BrutePoint();
    } else {
        UDFAcquireResourceExclusive(Resource, TRUE);
        return TRUE;
    }
    return FALSE;
} // end UDFAcquireResourceExclusiveWithCheck()

BOOLEAN
UDFAcquireResourceSharedWithCheck(
    IN PERESOURCE Resource
    )
{
    ULONG ReAcqRes =
        ExIsResourceAcquiredExclusiveLite(Resource) ? 1 :
        (ExIsResourceAcquiredSharedLite(Resource) ? 2 : 0);
    if(ReAcqRes) {
        UDFPrint(("UDFAcquireResourceSharedWithCheck: ReAcqRes, %x\n", ReAcqRes));
/*    } else {
        BrutePoint();*/
    }

    if(ReAcqRes == 2) {
        // OK
    } else
    if(ReAcqRes == 1) {
        UDFPrint(("UDFAcquireResourceSharedWithCheck: Exclusive\n"));
        //BrutePoint();
    } else {
        UDFAcquireResourceShared(Resource, TRUE);
        return TRUE;
    }
    return FALSE;
} // end UDFAcquireResourceSharedWithCheck()

NTSTATUS
UDFWCacheErrorHandler(
    IN PVOID Context,
    IN PWCACHE_ERROR_CONTEXT ErrorInfo
    )
{
    InterlockedIncrement((PLONG)&(((PVCB)Context)->IoErrorCounter));
    return ErrorInfo->Status;
}

VOID
UDFSetModified(
    IN PVCB        Vcb
    )
{
    if (UDFInterlockedIncrement((PLONG) & (Vcb->Modified)) & 0x80000000)
        Vcb->Modified = 2;
} // end UDFSetModified()

VOID
UDFPreClrModified(
    IN PVCB Vcb
    )
{
    Vcb->Modified = 1;
} // end UDFPreClrModified()

VOID
UDFClrModified(
    IN PVCB        Vcb
    )
{
    UDFPrint(("ClrModified\n"));
    UDFInterlockedDecrement((PLONG) & (Vcb->Modified));
} // end UDFClrModified()

NTSTATUS
UDFToggleMediaEjectDisable (
    IN PVCB Vcb,
    IN BOOLEAN PreventRemoval
    )
{
    PREVENT_MEDIA_REMOVAL Prevent;

    //  If PreventRemoval is the same as VCB_STATE_MEDIA_LOCKED,
    //  no-op this call, otherwise toggle the state of the flag.

    if ((PreventRemoval ^ BooleanFlagOn(Vcb->VCBFlags, VCB_STATE_MEDIA_LOCKED)) == 0) {

        return STATUS_SUCCESS;

    } else {

        Vcb->VCBFlags ^= VCB_STATE_MEDIA_LOCKED;
    }

    Prevent.PreventMediaRemoval = PreventRemoval;

    return UDFPhSendIOCTL(IOCTL_DISK_MEDIA_REMOVAL,
                          Vcb->TargetDeviceObject,
                          &Prevent,
                          sizeof(Prevent),
                          NULL,
                          0,
                          FALSE,
                          NULL);
}

#include "Include/regtools.cpp"

