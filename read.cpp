////////////////////////////////////////////////////////////////////
// Copyright (C) Alexander Telyatnikov, Ivan Keliukh, Yegor Anchishkin, SKIF Software, 1999-2013. Kiev, Ukraine
// All rights reserved
// This file was released under the GPLv2 on June 2015.
////////////////////////////////////////////////////////////////////
/*************************************************************************
*
* File: Read.cpp
*
* Module: UDF File System Driver (Kernel mode execution only)
*
* Description:
*   Contains code to handle the "Read" dispatch entry point.
*
*************************************************************************/

#include            "udffs.h"

// define the file specific bug-check id
#define         UDF_BUG_CHECK_ID                UDF_FILE_READ

#ifdef _M_IX86
#if DBG
#define OVERFLOW_READ_THRESHHOLD         (0xE00)
#else
#define OVERFLOW_READ_THRESHHOLD         (0xA00)
#endif // UDF_DBG
#else  // defined(_M_IX86)
#define OVERFLOW_READ_THRESHHOLD         (0x1000)
#endif // defined(_M_IX86)

//  This macro just puts a nice little try-except around RtlZeroMemory

#define SafeZeroMemory(AT,BYTE_COUNT) {                            \
    _SEH2_TRY {                                                    \
        RtlZeroMemory((AT), (BYTE_COUNT));                         \
    } _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {                    \
         UDFRaiseStatus(IrpContext, STATUS_INVALID_USER_BUFFER);\
    } _SEH2_END;                                                   \
}

/*************************************************************************
*
* Function: UDFRead()
*
* Description:
*   The I/O Manager will invoke this routine to handle a read
*   request
*
* Expected Interrupt Level (for execution) :
*
*  IRQL_PASSIVE_LEVEL (invocation at higher IRQL will cause execution
*   to be deferred to a worker thread context)
*
* Return Value: STATUS_SUCCESS/Error
*
*************************************************************************/
NTSTATUS
NTAPI
UDFRead(
    PDEVICE_OBJECT DeviceObject,       // the logical volume device object
    PIRP           Irp)                // I/O Request Packet
{
    NTSTATUS            RC = STATUS_SUCCESS;
    PIRP_CONTEXT IrpContext = NULL;
    BOOLEAN             AreWeTopLevel = FALSE;

    TmPrint(("UDFRead: \n"));

    FsRtlEnterFileSystem();
    ASSERT(DeviceObject);
    ASSERT(Irp);

    // set the top level context
    AreWeTopLevel = UDFIsIrpTopLevel(Irp);
    ASSERT(!UDFIsFSDevObj(DeviceObject));

    _SEH2_TRY {

        // get an IRP context structure and issue the request
        IrpContext = UDFCreateIrpContext(Irp, DeviceObject);
        if(IrpContext) {
            RC = UDFCommonRead(IrpContext, Irp);
        } else {
            RC = STATUS_INSUFFICIENT_RESOURCES;
            Irp->IoStatus.Status = RC;
            Irp->IoStatus.Information = 0;
            // complete the IRP (no IrpContext to reference!)
            IoCompleteRequest(Irp, IO_DISK_INCREMENT);
        }

    } _SEH2_EXCEPT(UDFExceptionFilter(IrpContext, _SEH2_GetExceptionInformation())) {

        RC = UDFProcessException(IrpContext, Irp);

        UDFLogEvent(UDF_ERROR_INTERNAL_ERROR, RC);
    } _SEH2_END;

    if (AreWeTopLevel) {
        IoSetTopLevelIrp(NULL);
    }

    FsRtlExitFileSystem();

    return(RC);
} // end UDFRead()


/*************************************************************************
*
* Function: UDFPostStackOverflowRead()
*
* Description:
*    Post a read request that could not be processed by
*    the fsp thread because of stack overflow potential.
*
* Arguments:
*    Irp - Supplies the request to process.
*    Fcb - Supplies the file.
*
* Return Value: STATUS_PENDING.
*
*************************************************************************/
NTSTATUS
UDFPostStackOverflowRead(
    IN PIRP_CONTEXT IrpContext,
    IN PIRP             Irp,
    IN PFCB             Fcb
    )
{
    PKEVENT Event;
    PERESOURCE Resource;

    UDFPrint(("Getting too close to stack limit pass request to Fsp\n"));

    //  Allocate an event and get shared on the resource we will
    //  be later using the common read.
    Event = (PKEVENT)MyAllocatePool__(NonPagedPool, sizeof(KEVENT));
    if(!Event)
        return STATUS_INSUFFICIENT_RESOURCES;
    KeInitializeEvent( Event, NotificationEvent, FALSE );

    if (Irp->Flags & IRP_PAGING_IO && Fcb->Header.PagingIoResource) {
        Resource = Fcb->Header.PagingIoResource;
    } else {
        Resource = Fcb->Header.Resource;
    }

    UDFAcquireResourceShared(Resource, TRUE);

    _SEH2_TRY {
        //  If this read is the result of a verify, we have to
        //  tell the overflow read routne to temporarily
        //  hijack the Vcb->VerifyThread field so that reads
        //  can go through.
        FsRtlPostStackOverflow(IrpContext, Event, UDFStackOverflowRead);
        //  And wait for the worker thread to complete the item
        DbgWaitForSingleObject(Event, NULL);

    } _SEH2_FINALLY {

        UDFReleaseResource( Resource );
        MyFreePool__( Event );
    } _SEH2_END;

    return STATUS_PENDING;

} // end UDFPostStackOverflowRead()

/*************************************************************************
*
* Function: UDFStackOverflowRead()
*
* Description:
*    Process a read request that could not be processed by
*    the fsp thread because of stack overflow potential.
*
* Arguments:
*    Context - Supplies the IrpContext being processed
*    Event - Supplies the event to be signaled when we are done processing this
*        request.
*
* Expected Interrupt Level (for execution) :
*
*  IRQL_PASSIVE_LEVEL
*
* Return Value: None.
*
*************************************************************************/
VOID
NTAPI
UDFStackOverflowRead(
    IN PVOID Context,
    IN PKEVENT Event
    )
{
    PIRP_CONTEXT IrpContext = (PIRP_CONTEXT)Context;
    NTSTATUS RC;

    UDFPrint(("UDFStackOverflowRead: \n"));
    //  Make it now look like we can wait for I/O to complete
    IrpContext->Flags |= IRP_CONTEXT_FLAG_WAIT;

    //  Do the read operation protected by a try-except clause
    _SEH2_TRY {
        UDFCommonRead(IrpContext, IrpContext->Irp);
    } _SEH2_EXCEPT(UDFExceptionFilter(IrpContext, _SEH2_GetExceptionInformation())) {
        RC = UDFProcessException(IrpContext, IrpContext->Irp);
        UDFLogEvent(UDF_ERROR_INTERNAL_ERROR, RC);
    } _SEH2_END;

    //  Set the stack overflow item's event to tell the original
    //  thread that we're done.
    KeSetEvent( Event, 0, FALSE );
} // end UDFStackOverflowRead()


/*************************************************************************
*
* Function: UDFCommonRead()
*
* Description:
*   The actual work is performed here. This routine may be invoked in one
*   of the two possible contexts:
*   (a) in the context of a system worker thread
*   (b) in the context of the original caller
*
* Expected Interrupt Level (for execution) :
*
*  IRQL_PASSIVE_LEVEL
*
* Return Value: STATUS_SUCCESS/Error
*
*************************************************************************/
NTSTATUS
UDFCommonRead(
    PIRP_CONTEXT IrpContext,
    PIRP             Irp
    )
{
    NTSTATUS                RC = STATUS_SUCCESS;
    PIO_STACK_LOCATION      IrpSp = NULL;
    LARGE_INTEGER           ByteOffset;
    ULONG                   ReadLength = 0, TruncatedLength = 0;
    SIZE_T                  NumberBytesRead = 0;
    PFILE_OBJECT            FileObject = NULL;
    PFCB                    Fcb = NULL;
    PCCB                    Ccb = NULL;
    PVCB                    Vcb = NULL;
    BOOLEAN                 VcbAcquired = FALSE;
    BOOLEAN                 MainResourceAcquired = FALSE;
    BOOLEAN                 PagingIoResourceAcquired = FALSE;
    PVOID                   SystemBuffer = NULL;
    PIRP                    TopIrp;

    BOOLEAN                 CacheLocked = FALSE;

    BOOLEAN                 CanWait = FALSE;
    BOOLEAN                 PagingIo = FALSE;
    BOOLEAN                 NonCachedIo = FALSE;
    BOOLEAN                 SynchronousIo = FALSE;

    TmPrint(("UDFCommonRead: irp %x\n", Irp));

    _SEH2_TRY {

        TopIrp = IoGetTopLevelIrp();
        switch((ULONG_PTR)TopIrp) {
        case FSRTL_FSP_TOP_LEVEL_IRP:
            UDFPrint(("  FSRTL_FSP_TOP_LEVEL_IRP\n"));
            break;
        case FSRTL_CACHE_TOP_LEVEL_IRP:
            UDFPrint(("  FSRTL_CACHE_TOP_LEVEL_IRP\n"));
            break;
        case FSRTL_MOD_WRITE_TOP_LEVEL_IRP:
            UDFPrint(("  FSRTL_MOD_WRITE_TOP_LEVEL_IRP\n"));
//            BrutePoint()
            break;
        case FSRTL_FAST_IO_TOP_LEVEL_IRP:
            UDFPrint(("  FSRTL_FAST_IO_TOP_LEVEL_IRP\n"));
//            BrutePoint()
            break;
        case NULL:
            UDFPrint(("  NULL TOP_LEVEL_IRP\n"));
            break;
        default:
            if(TopIrp == Irp) {
                UDFPrint(("  TOP_LEVEL_IRP\n"));
            } else {
                UDFPrint(("  RECURSIVE_IRP, TOP = %x\n", TopIrp));
            }
            break;
        }
        // First, get a pointer to the current I/O stack location
        IrpSp = IoGetCurrentIrpStackLocation(Irp);
        ASSERT(IrpSp);
        MmPrint(("    Enter Irp, MDL=%x\n", Irp->MdlAddress));
        if(Irp->MdlAddress) {
            UDFTouch(Irp->MdlAddress);
        }

        // If this happens to be a MDL read complete request, then
        // there is not much processing that the FSD has to do.
        if (IrpSp->MinorFunction & IRP_MN_COMPLETE) {
            // Caller wants to tell the Cache Manager that a previously
            // allocated MDL can be freed.
            UDFMdlComplete(IrpContext, Irp, IrpSp, TRUE);
            // The IRP has been completed.
            try_return(RC = STATUS_SUCCESS);
        }

        // If this is a request at IRQL DISPATCH_LEVEL, then post
        // the request (your FSD may choose to process it synchronously
        // if you implement the support correctly; obviously you will be
        // quite constrained in what you can do at such IRQL).
        if (IrpSp->MinorFunction & IRP_MN_DPC) {
            try_return(RC = STATUS_PENDING);
        }

        FileObject = IrpSp->FileObject;
        ASSERT(FileObject);

        // Get the FCB and CCB pointers
        Ccb = (PCCB)FileObject->FsContext2;
        ASSERT(Ccb);
        Fcb = Ccb->Fcb;
        ASSERT(Fcb);
        Vcb = Fcb->Vcb;

        if(Fcb->FCBFlags & UDF_FCB_DELETED) {
            ASSERT(FALSE);
            try_return(RC = STATUS_ACCESS_DENIED);
        }

        // check for stack overflow
        if (IoGetRemainingStackSize() < OVERFLOW_READ_THRESHHOLD) {
            RC = UDFPostStackOverflowRead( IrpContext, Irp, Fcb );
            try_return(RC);
        }

        // Disk based file systems might decide to verify the logical volume
        //  (if required and only if removable media are supported) at this time
        // As soon as Tray is locked, we needn't call UDFVerifyVcb()

        ByteOffset = IrpSp->Parameters.Read.ByteOffset;

        CanWait = (IrpContext->Flags & IRP_CONTEXT_FLAG_WAIT) ? TRUE : FALSE;
        PagingIo = (Irp->Flags & IRP_PAGING_IO) ? TRUE : FALSE;
        NonCachedIo = (Irp->Flags & IRP_NOCACHE) ? TRUE : FALSE;
        SynchronousIo = (FileObject->Flags & FO_SYNCHRONOUS_IO) ? TRUE : FALSE;
        UDFPrint(("    Flags: %s %s %s %s\n",
                      CanWait ? "W" : "w", PagingIo ? "Pg" : "pg",
                      NonCachedIo ? "NonCached" : "Cached", SynchronousIo ? "Snc" : "Asc"));

        if(!NonCachedIo &&
           (Fcb->NodeIdentifier.NodeTypeCode != UDF_NODE_TYPE_VCB)) {
            if(UDFIsAStream(Fcb->FileInfo)) {
                UDFNotifyFullReportChange( Vcb, Fcb->FileInfo,
                                           FILE_NOTIFY_CHANGE_LAST_ACCESS,
                                           FILE_ACTION_MODIFIED_STREAM);
            } else {
                UDFNotifyFullReportChange( Vcb, Fcb->FileInfo,
                                           FILE_NOTIFY_CHANGE_LAST_ACCESS,
                                           FILE_ACTION_MODIFIED);
            }
        }

        // Get some of the parameters supplied to us
        ReadLength = IrpSp->Parameters.Read.Length;
        if (ReadLength == 0) {
            // a 0 byte read can be immediately succeeded
            try_return(RC);
        }
        UDFPrint(("    ByteOffset = %I64x, ReadLength = %x\n", ByteOffset.QuadPart, ReadLength));

        // Is this a read of the volume itself ?
        if (Fcb->NodeIdentifier.NodeTypeCode == UDF_NODE_TYPE_VCB) {
            // Yup, we need to send this on to the disk driver after
            //  validation of the offset and length.
            Vcb = (PVCB)Fcb;
            Vcb->VCBFlags |= UDF_VCB_SKIP_EJECT_CHECK;
            if(!CanWait)
                try_return(RC = STATUS_PENDING);


            if(IrpContext->Flags & UDF_IRP_CONTEXT_FLUSH2_REQUIRED) {

                UDFPrint(("  UDF_IRP_CONTEXT_FLUSH2_REQUIRED\n"));
                IrpContext->Flags &= ~UDF_IRP_CONTEXT_FLUSH2_REQUIRED;

                if(!(Vcb->VCBFlags & VCB_STATE_RAW_DISK)) {
                    UDFCloseAllSystemDelayedInDir(Vcb, Vcb->RootDirFCB->FileInfo);
                }
#ifdef UDF_DELAYED_CLOSE
                UDFCloseAllDelayed(Vcb);
#endif //UDF_DELAYED_CLOSE

            }

            if(IrpContext->Flags & UDF_IRP_CONTEXT_FLUSH_REQUIRED) {

                UDFPrint(("  UDF_IRP_CONTEXT_FLUSH_REQUIRED\n"));
                IrpContext->Flags &= ~UDF_IRP_CONTEXT_FLUSH_REQUIRED;

                // Acquire the volume resource exclusive
                UDFAcquireResourceExclusive(&Vcb->VCBResource, TRUE);
                VcbAcquired = TRUE;

                UDFFlushLogicalVolume(NULL, NULL, Vcb, 0);

                UDFReleaseResource(&Vcb->VCBResource);
                VcbAcquired = FALSE;
            }

            // Acquire the volume resource shared ...
            UDFAcquireResourceShared(&Vcb->VCBResource, TRUE);
            VcbAcquired = TRUE;

#if 0
            if(PagingIo) {
                CollectStatistics(Vcb, MetaDataReads);
                CollectStatisticsEx(Vcb, MetaDataReadBytes, NumberBytesRead);
            }
#endif

            // Forward the request to the lower level driver
            // Lock the callers buffer
            if (!NT_SUCCESS(RC = UDFLockUserBuffer(IrpContext, ReadLength, IoWriteAccess))) {
                try_return(RC);
            }
            SystemBuffer = UDFMapUserBuffer(Irp);
            if(!SystemBuffer) {
                try_return(RC = STATUS_INVALID_USER_BUFFER);
            }
            if (Vcb->VcbCondition == VcbMounted) {
                 RC = UDFReadData(Vcb, TRUE, ByteOffset.QuadPart,
                                ReadLength, FALSE, (PCHAR)SystemBuffer,
                                &NumberBytesRead);
            } else {
                 RC = UDFTRead(Vcb, SystemBuffer, ReadLength,
                                (ULONG)(ByteOffset.QuadPart >> Vcb->BlockSizeBits),
                                &NumberBytesRead);
            }
            UDFUnlockCallersBuffer(IrpContext, Irp, SystemBuffer);
            try_return(RC);
        }
        Vcb->VCBFlags |= UDF_VCB_SKIP_EJECT_CHECK;

        // If the read request is directed to a page file (if your FSD
        // supports paging files), send the request directly to the disk
        // driver. For requests directed to a page file, you have to trust
        // that the offsets will be set correctly by the VMM. You should not
        // attempt to acquire any FSD resources either.
        if(Fcb->FCBFlags & UDF_FCB_PAGE_FILE) {
            NonCachedIo = TRUE;
        }

        if(ByteOffset.HighPart == -1) {
            if(ByteOffset.LowPart == FILE_USE_FILE_POINTER_POSITION) {
                ByteOffset = FileObject->CurrentByteOffset;
            }
        }

        // If this read is directed to a directory, it is not allowed
        //  by the UDF FSD.
        if(Fcb->FCBFlags & UDF_FCB_DIRECTORY) {
            RC = STATUS_INVALID_DEVICE_REQUEST;
            try_return(RC);
        }

#if 0
        if(PagingIo) {
            CollectStatistics(Vcb, UserFileReads);
            CollectStatisticsEx(Vcb, UserFileReadBytes, NumberBytesRead);
        }
#endif

        // There are certain complications that arise when the same file stream
        // has been opened for cached and non-cached access. The FSD is then
        // responsible for maintaining a consistent view of the data seen by
        // the caller.
        // Also, it is possible for file streams to be mapped in both as data files
        // and as an executable. This could also lead to consistency problems since
        // there now exist two separate sections (and pages) containing file
        // information.

        // The test below flushes the data cached in system memory if the current
        // request madates non-cached access (file stream must be cached) and
        // (a) the current request is not paging-io which indicates it is not
        //       a recursive I/O operation OR originating in the Cache Manager
        // (b) OR the current request is paging-io BUT it did not originate via
        //       the Cache Manager (or is a recursive I/O operation) and we do
        //       have an image section that has been initialized.

        // Acquire the appropriate FCB resource shared
        if (PagingIo) {

            // Don't offload jobs when doing paging IO - otherwise this can lead to
            // deadlocks in CcCopyRead.
            CanWait = true;
            // Try to acquire the FCB PagingIoResource shared
            if (!UDFAcquireSharedStarveExclusive(&Fcb->PagingIoResource, CanWait)) {
                try_return(RC = STATUS_PENDING);
            }
            PagingIoResourceAcquired = TRUE;

        } else {
            // Try to acquire the FCB MainResource shared
            if(NonCachedIo && Fcb->SectionObject.DataSectionObject) {

                // We hold the main resource exclusive here because the flush
                // may generate a recursive write in this thread.
                UDF_CHECK_PAGING_IO_RESOURCE(Fcb);
                if(!UDFAcquireResourceExclusive(&Fcb->MainResource, CanWait)) {
                    try_return(RC = STATUS_PENDING);
                }
                MainResourceAcquired = TRUE;

                // We hold PagingIo shared around the flush to fix a
                // cache coherency problem.
                UDFAcquireResourceShared(&Fcb->PagingIoResource, TRUE );

                MmPrint(("    CcFlushCache()\n"));
                CcFlushCache(&Fcb->SectionObject, &ByteOffset, ReadLength, &Irp->IoStatus);

                UDFReleaseResource(&Fcb->PagingIoResource);

                // If the flush failed, return error to the caller
                if(!NT_SUCCESS(RC = Irp->IoStatus.Status)) {
                    try_return(RC);
                }

                UDFConvertExclusiveToSharedLite(&Fcb->MainResource);

            } else {
                UDF_CHECK_PAGING_IO_RESOURCE(Fcb);
                if(!UDFAcquireResourceShared(&Fcb->MainResource, CanWait)) {
                    try_return(RC = STATUS_PENDING);
                }
                MainResourceAcquired = TRUE;
            }
        }

        // This is a good place for oplock related processing.

        // If this is the normal file we have to check for
        // write access according to the current state of the file locks.
        if (!PagingIo &&
            Fcb->FileLock != NULL &&
            !FsRtlCheckLockForReadAccess(Fcb->FileLock, Irp)) {

                try_return( RC = STATUS_FILE_LOCK_CONFLICT );
        }

        // Validate start offset and length supplied.
        // If start offset is > end-of-file, return an appropriate error. Note
        // that since a FCB resource has already been acquired, and since all
        // file size changes require acquisition of both FCB resources,
        // the contents of the FCB and associated data structures
        // can safely be examined.

        // Also note that we are using the file size in the "Common FCB Header"
        // to perform the check. However, your FSD might decide to keep a
        // separate copy in the FCB (or some other representation of the
        // file associated with the FCB).

        TruncatedLength = ReadLength;
        if (ByteOffset.QuadPart >= Fcb->Header.FileSize.QuadPart) {
            // Starting offset is >= file size
            try_return(RC = STATUS_END_OF_FILE);
        }
        // We can also go ahead and truncate the read length here
        //  such that it is contained within the file size
        if (Fcb->Header.FileSize.QuadPart < (ByteOffset.QuadPart + ReadLength)) {
            TruncatedLength = (ULONG)(Fcb->Header.FileSize.QuadPart - ByteOffset.QuadPart);
            // we can't get ZERO here
        }
        UDFPrint(("    TruncatedLength = %x\n", TruncatedLength));

        // This is also a good place to set whether fast-io can be performed
        // on this particular file or not. Your FSD must make it's own
        // determination on whether or not to allow fast-io operations.
        // Commonly, fast-io is not allowed if any byte range locks exist
        // on the file or if oplocks prevent fast-io. Practically any reason
        // choosen by your FSD could result in your setting FastIoIsNotPossible
        // OR FastIoIsQuestionable instead of FastIoIsPossible.

        Fcb->Header.IsFastIoPossible = UDFIsFastIoPossible(Fcb);
/*        if(NtReqFcb->CommonFCBHeader.IsFastIoPossible == FastIoIsPossible)
            NtReqFcb->CommonFCBHeader.IsFastIoPossible = FastIoIsQuestionable;*/

#ifdef UDF_DISABLE_SYSTEM_CACHE_MANAGER
        NonCachedIo = TRUE;
#endif

        if(Fcb && Fcb->FileInfo && Fcb->FileInfo->Dloc) {
            AdPrint(("UDFCommonRead: DataLoc %x, Mapping %x\n", &Fcb->FileInfo->Dloc->DataLoc, Fcb->FileInfo->Dloc->DataLoc.Mapping));
        }

        //  Branch here for cached vs non-cached I/O
        if (!NonCachedIo) {

            if(FileObject->Flags & FO_WRITE_THROUGH) {
                CanWait = TRUE;
            }
            // The caller wishes to perform cached I/O. Initiate caching if
            // this is the first cached I/O operation using this file object
            if (!(FileObject->PrivateCacheMap)) {
                // This is the first cached I/O operation. You must ensure
                // that the FCB Common FCB Header contains valid sizes at this time
                MmPrint(("    CcInitializeCacheMap()\n"));
                CcInitializeCacheMap(FileObject, (PCC_FILE_SIZES)&Fcb->Header.AllocationSize,
                    FALSE,      // We will not utilize pin access for this file
                    &(UDFGlobalData.CacheMgrCallBacks), // callbacks
                    Fcb);        // The context used in callbacks
                MmPrint(("    CcSetReadAheadGranularity()\n"));
                CcSetReadAheadGranularity(FileObject, Vcb->SystemCacheGran);
            }

            // Check and see if this request requires a MDL returned to the caller
            if (IrpSp->MinorFunction & IRP_MN_MDL) {
                // Caller does want a MDL returned. Note that this mode
                // implies that the caller is prepared to block
                MmPrint(("    CcMdlRead()\n"));
//                CcMdlRead(FileObject, &ByteOffset, TruncatedLength, &(Irp->MdlAddress), &(Irp->IoStatus));
//                NumberBytesRead = Irp->IoStatus.Information;
//                RC = Irp->IoStatus.Status;
                NumberBytesRead = 0;
                RC = STATUS_INVALID_PARAMETER;

                try_return(RC);
            }

            // This is a regular run-of-the-mill cached I/O request. Let the
            // Cache Manager worry about it!
            // First though, we need a buffer pointer (address) that is valid
            SystemBuffer = UDFMapUserBuffer(Irp);
            if(!SystemBuffer)
                try_return(RC = STATUS_INVALID_USER_BUFFER);
            ASSERT(SystemBuffer);
            MmPrint(("    CcCopyRead()\n"));
            if (!CcCopyRead(FileObject, &(ByteOffset), TruncatedLength, CanWait, SystemBuffer, &Irp->IoStatus)) {
                // The caller was not prepared to block and data is not immediately
                // available in the system cache
                try_return(RC = STATUS_PENDING);
            }

            UDFUnlockCallersBuffer(IrpContext, Irp, SystemBuffer);
            // We have the data
            RC = Irp->IoStatus.Status;
            NumberBytesRead = Irp->IoStatus.Information;

            try_return(RC);

        } else {

            MmPrint(("    Read NonCachedIo\n"));

#if 1
            if((ULONG_PTR)TopIrp == FSRTL_MOD_WRITE_TOP_LEVEL_IRP) {
                UDFPrint(("FSRTL_MOD_WRITE_TOP_LEVEL_IRP => CanWait\n"));
                CanWait = TRUE;
            } else
            if((ULONG_PTR)TopIrp == FSRTL_CACHE_TOP_LEVEL_IRP) {
                UDFPrint(("FSRTL_CACHE_TOP_LEVEL_IRP => CanWait\n"));
                CanWait = TRUE;
            }

            if(Fcb->AcqSectionCount || Fcb->AcqFlushCount) {
                MmPrint(("    AcqCount (%d/%d)=> CanWait ?\n", Fcb->AcqSectionCount, Fcb->AcqFlushCount));
                CanWait = TRUE;
            } else
            {}
/*            if((TopIrp != Irp)) {
                UDFPrint(("(TopIrp != Irp) => CanWait\n"));
                CanWait = TRUE;
            } else*/
#endif
            if(!CanWait && UDFIsFileCached__(Vcb, Fcb->FileInfo, ByteOffset.QuadPart, TruncatedLength, FALSE)) {
                MmPrint(("    Locked => CanWait\n"));
                CacheLocked = TRUE;
                CanWait = TRUE;
            }

            // Send the request to lower level drivers
            if(!CanWait) {
                try_return(RC = STATUS_PENDING);
            }

//                ASSERT(NT_SUCCESS(RC));

            RC = UDFLockUserBuffer(IrpContext, TruncatedLength, IoWriteAccess);
            if(!NT_SUCCESS(RC)) {
                try_return(RC);
            }

            SystemBuffer = UDFMapUserBuffer(Irp);
            if(!SystemBuffer) {
                try_return(RC = STATUS_INVALID_USER_BUFFER);
            }

            // Start by zeroing any part of the read after Valid Data

            LARGE_INTEGER ValidDataLength = Fcb->Header.ValidDataLength;

            if (ByteOffset.QuadPart + TruncatedLength > ValidDataLength.QuadPart) {

                if (ByteOffset.QuadPart < ValidDataLength.QuadPart) {

                    ULONG LBS = Vcb->LBlockSize;
                    ULONG ZeroingOffset = ((ValidDataLength.QuadPart - ByteOffset.QuadPart) + (LBS - 1)) & ~(LBS - 1);

                    // If the offset is at or above the byte count, no harm: just means
                    // that the read ends in the last sector and the zeroing will be
                    // done at completion.

                    if (TruncatedLength > ZeroingOffset) {

                        SafeZeroMemory((PUCHAR)SystemBuffer + ZeroingOffset, TruncatedLength - ZeroingOffset);
                    }
                } else {

                    //  All we have to do now is sit here and zero the
                    //  user's buffer, no reading is required.

                    SafeZeroMemory(SystemBuffer, TruncatedLength);
                    NumberBytesRead = TruncatedLength;
                    UDFUnlockCallersBuffer(IrpContext, Irp, SystemBuffer);
                    try_return(STATUS_SUCCESS);
                }
            }

            RC = UDFReadFile__(Vcb, Fcb->FileInfo, ByteOffset.QuadPart, TruncatedLength,
                           CacheLocked, (PCHAR)SystemBuffer, &NumberBytesRead);
/*                // AFAIU, CacheManager wants this:
            if(!NT_SUCCESS(RC)) {
                NumberBytesRead = 0;
            }*/

            UDFUnlockCallersBuffer(IrpContext, Irp, SystemBuffer);

#if 0
            if(PagingIo) {
                CollectStatistics(Vcb, UserDiskReads);
            } else {
                CollectStatistics2(Vcb, NonCachedDiskReads);
            }
#endif

            try_return(RC);

            // For paging-io, the FSD has to trust the VMM to do the right thing

            // Here is a common method used by Windows NT native file systems
            // that are in the process of sending a request to the disk driver.
            // First, mark the IRP as pending, then invoke the lower level driver
            // after setting a completion routine.
            // Meanwhile, this particular thread can immediately return a
            // STATUS_PENDING return code.
            // The completion routine is then responsible for completing the IRP
            // and unlocking appropriate resources

            // Also, at this point, the FSD might choose to utilize the
            // information contained in the ValidDataLength field to simply
            // return zeroes to the caller for reads extending beyond current
            // valid data length.

        }

try_exit:   NOTHING;

    } _SEH2_FINALLY {

        if(CacheLocked) {
            WCacheEODirect__(&(Vcb->FastCache), Vcb);
        }

        // Release any resources acquired here ...
        if (PagingIoResourceAcquired) {
            UDFReleaseResource(&Fcb->PagingIoResource);
        }

        if (MainResourceAcquired) {
            UDF_CHECK_PAGING_IO_RESOURCE(Fcb);
            UDFReleaseResource(&Fcb->MainResource);
        }

        if (VcbAcquired) {
            UDFReleaseResource(&Vcb->VCBResource);
        }

        // Post IRP if required
        if(RC == STATUS_PENDING) {

            // Lock the callers buffer here. Then invoke a common routine to
            // perform the post operation.
            if (!(IrpSp->MinorFunction & IRP_MN_MDL)) {
                RC = UDFLockUserBuffer(IrpContext, ReadLength, IoWriteAccess);
                ASSERT(NT_SUCCESS(RC));
            }

            // Perform the post operation which will mark the IRP pending
            // and will return STATUS_PENDING back to us
            RC = UDFPostRequest(IrpContext, Irp);

        } else {
            // For synchronous I/O, the FSD must maintain the current byte offset
            // Do not do this however, if I/O is marked as paging-io
            if (SynchronousIo && !PagingIo && NT_SUCCESS(RC)) {
                FileObject->CurrentByteOffset.QuadPart = ByteOffset.QuadPart + NumberBytesRead;
            }
            // If the read completed successfully and this was not a paging-io
            // operation, set a flag in the CCB that indicates that a read was
            // performed and that the file time should be updated at cleanup
            if (NT_SUCCESS(RC) && !PagingIo) {
                FileObject->Flags |= FO_FILE_FAST_IO_READ;
                Ccb->CCBFlags |= UDF_CCB_ACCESSED;
            }

            if(!_SEH2_AbnormalTermination()) {
                Irp->IoStatus.Status = RC;
                Irp->IoStatus.Information = NumberBytesRead;
                UDFPrint(("    NumberBytesRead = %x\n", NumberBytesRead));
                // complete the IRP
                MmPrint(("    Complete Irp, MDL=%x\n", Irp->MdlAddress));
                if(Irp->MdlAddress) {
                    UDFTouch(Irp->MdlAddress);
                }
				if (!IrpContext->IrpCompleted) {
			    IrpContext->IrpCompleted = TRUE;
                IoCompleteRequest(Irp, IO_DISK_INCREMENT);}
            }
        } // can we complete the IRP ?
    } _SEH2_END; // end of "__finally" processing

    return(RC);
} // end UDFCommonRead()


#ifdef UDF_DBG
ULONG LockBufferCounter = 0;
#endif //UDF_DBG

/*************************************************************************
*
* Function: UDFMapUserBuffer()
*
* Description:
*   Obtain a pointer to the caller's buffer.
*
* Expected Interrupt Level (for execution) :
*
*  IRQL_PASSIVE_LEVEL
*
* Return Value: STATUS_SUCCESS/Error
*
*************************************************************************/
PVOID
UDFMapUserBuffer(
    PIRP Irp
    )
{
    // If there is no Mdl, then we must be in the Fsd, and we can simply
    // return the UserBuffer field from the Irp.

    if(Irp->MdlAddress == NULL) {

        return Irp->UserBuffer;

    } else {

        return MmGetSystemAddressForMdlSafe(Irp->MdlAddress, NormalPagePriority | MdlMappingNoExecute);
    }

} // end UDFMapUserBuffer()

/*************************************************************************
*
* Function: UDFLockUserBuffer()
*
* Description:
*   Obtain a MDL that describes the buffer. Lock pages for I/O
*
* Expected Interrupt Level (for execution) :
*
*  IRQL_PASSIVE_LEVEL
*
* Return Value: STATUS_SUCCESS/Error
*
*************************************************************************/
NTSTATUS
UDFLockUserBuffer(
    PIRP_CONTEXT IrpContext,
    ULONG BufferLength,
    LOCK_OPERATION LockOperation
    )
{
    NTSTATUS            RC = STATUS_SUCCESS;
    PMDL                Mdl = NULL;

    ASSERT_IRP_CONTEXT(IrpContext);
    ASSERT_IRP(IrpContext->Irp);

    // Is a MDL already present in the IRP
    if (!IrpContext->Irp->MdlAddress) {

        // This will place allocated Mdl to Irp
        if (!(Mdl = IoAllocateMdl(IrpContext->Irp->UserBuffer, BufferLength, FALSE, FALSE, IrpContext->Irp))) {

            return(RC = STATUS_INSUFFICIENT_RESOURCES);
        }

        // Probe and lock the pages described by the MDL
        // We could encounter an exception doing so, swallow the exception
        // NOTE: The exception could be due to an unexpected (from our
        // perspective), invalidation of the virtual addresses that comprise
        // the passed in buffer

        _SEH2_TRY {

            MmProbeAndLockPages(Mdl, IrpContext->Irp->RequestorMode, LockOperation);

        } _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {

            IoFreeMdl(Mdl);
            IrpContext->Irp->MdlAddress = NULL;
            RC = STATUS_INVALID_USER_BUFFER;

        } _SEH2_END;
    }

    return(RC);
} // end UDFLockUserBuffer()

/*************************************************************************
*
* Function: UDFUnlockCallersBuffer()
*
* Description:
*   Obtain a MDL that describes the buffer. Lock pages for I/O
*
* Expected Interrupt Level (for execution) :
*
*  IRQL_PASSIVE_LEVEL
*
* Return Value: STATUS_SUCCESS/Error
*
*************************************************************************/
NTSTATUS
UDFUnlockCallersBuffer(
    PIRP_CONTEXT IrpContext,
    PIRP    Irp,
    PVOID   SystemBuffer
    )
{
    NTSTATUS            RC = STATUS_SUCCESS;

    UDFPrint(("UDFUnlockCallersBuffer: \n"));

    ASSERT(Irp);

    _SEH2_TRY {

        if(Irp->MdlAddress) {

            KeFlushIoBuffers( Irp->MdlAddress,
                              ((IoGetCurrentIrpStackLocation(Irp))->MajorFunction) == IRP_MJ_READ,
                              FALSE );
        }

    } _SEH2_FINALLY {
        NOTHING;
    } _SEH2_END;

    return(RC);
} // end UDFUnlockCallersBuffer()

/*************************************************************************
*
* Function: UDFMdlComplete()
*
* Description:
*   Tell Cache Manager to release MDL (and possibly flush).
*
* Expected Interrupt Level (for execution) :
*
*  IRQL_PASSIVE_LEVEL
*
* Return Value: None.
*
*************************************************************************/
VOID UDFMdlComplete(
PIRP_CONTEXT IrpContext,
PIRP                        Irp,
PIO_STACK_LOCATION          IrpSp,
BOOLEAN                     ReadCompletion)
{
    NTSTATUS                RC = STATUS_SUCCESS;
    PFILE_OBJECT            FileObject = NULL;

    UDFPrint(("UDFMdlComplete: \n"));

    FileObject = IrpSp->FileObject;
    ASSERT(FileObject);

    UDFTouch(Irp->MdlAddress);
    // Not much to do here.
    if (ReadCompletion) {
        MmPrint(("    CcMdlReadComplete() MDL=%x\n", Irp->MdlAddress));
        CcMdlReadComplete(FileObject, Irp->MdlAddress);
    } else {
        // The Cache Manager needs the byte offset in the I/O stack location.
        MmPrint(("    CcMdlWriteComplete() MDL=%x\n", Irp->MdlAddress));
        CcMdlWriteComplete(FileObject, &(IrpSp->Parameters.Write.ByteOffset), Irp->MdlAddress);
    }

    // Clear the MDL address field in the IRP so the IoCompleteRequest()
    // does not __try to play around with the MDL.
    Irp->MdlAddress = NULL;

    // Complete the IRP.
    Irp->IoStatus.Status = RC;
    Irp->IoStatus.Information = 0;
	if (!IrpContext->IrpCompleted) {
	IrpContext->IrpCompleted = TRUE;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);}

    return;
}
