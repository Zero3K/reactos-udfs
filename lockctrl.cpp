////////////////////////////////////////////////////////////////////
// Copyright (C) Alexander Telyatnikov, Ivan Keliukh, Yegor Anchishkin, SKIF Software, 1999-2013. Kiev, Ukraine
// All rights reserved
// This file was released under the GPLv2 on June 2015.
////////////////////////////////////////////////////////////////////
/*************************************************************************
*
* File: LockCtrl.cpp.cpp
*
* Module: UDF File System Driver (Kernel mode execution only)
*
* Description:
*   Contains code to handle the "byte-range locking" dispatch entry point.
*
*************************************************************************/

#include            "udffs.h"

// define the file specific bug-check id
#define         UDF_BUG_CHECK_ID                UDF_FILE_SHUTDOWN


/*************************************************************************
*
* Function: UDFLockControl()
*
* Description:
*
* Expected Interrupt Level (for execution) :
*
*  IRQL_PASSIVE_LEVEL
*
* Return Value: Irrelevant.
*
*************************************************************************/
NTSTATUS
NTAPI
UDFLockControl(
    IN PDEVICE_OBJECT DeviceObject,       // the logical volume device object
    IN PIRP           Irp)                // I/O Request Packet
{
    NTSTATUS            RC = STATUS_SUCCESS;
    PIRP_CONTEXT IrpContext = NULL;
    BOOLEAN             AreWeTopLevel = FALSE;

    UDFPrint(("UDFLockControl\n"));
//    BrutePoint();

    FsRtlEnterFileSystem();
    ASSERT(DeviceObject);
    ASSERT(Irp);

    // set the top level context
    AreWeTopLevel = UDFIsIrpTopLevel(Irp);
    //  Call the common Lock Control routine, with blocking allowed if
    //  synchronous
    _SEH2_TRY {

        // get an IRP context structure and issue the request
        IrpContext = UDFCreateIrpContext(Irp, DeviceObject);
        if(IrpContext) {
            RC = UDFCommonLockControl(IrpContext, Irp);
        } else {
            RC = STATUS_INSUFFICIENT_RESOURCES;
            Irp->IoStatus.Status = RC;
            Irp->IoStatus.Information = 0;
            // complete the IRP
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
} // end UDFLockControl()


/*************************************************************************
*
* Function: UDFCommonLockControl()
*
* Description:
*  This is the common routine for doing Lock control operations called
*  by both the fsd and fsp threads
*
* Expected Interrupt Level (for execution) :
*
*  IRQL_PASSIVE_LEVEL
*
* Return Value: Irrelevant
*
*************************************************************************/
NTSTATUS
NTAPI
UDFCommonLockControl(
    IN PIRP_CONTEXT IrpContext,
    IN PIRP             Irp)
{
    NTSTATUS            RC = STATUS_SUCCESS;
    PIO_STACK_LOCATION  IrpSp = NULL;
    //IO_STATUS_BLOCK     LocalIoStatus;
//    BOOLEAN             CompleteRequest = FALSE;
    BOOLEAN             PostRequest = FALSE;
    BOOLEAN             CanWait = FALSE;
    BOOLEAN             AcquiredFCB = FALSE;
    PFILE_OBJECT        FileObject = NULL;
    PFCB                Fcb = NULL;
    PCCB                Ccb = NULL;

    UDFPrint(("UDFCommonLockControl\n"));

    _SEH2_TRY {
        // First, get a pointer to the current I/O stack location.
        IrpSp = IoGetCurrentIrpStackLocation(Irp);
        ASSERT(IrpSp);

        FileObject = IrpSp->FileObject;
        ASSERT(FileObject);

        // Get the FCB and CCB pointers.
        Ccb = (PCCB)FileObject->FsContext2;
        ASSERT(Ccb);
        Fcb = Ccb->Fcb;
        ASSERT(Fcb);
        // Validate the sent-in FCB
        if ( (Fcb->NodeIdentifier.NodeTypeCode == UDF_NODE_TYPE_VCB) ||
             (Fcb->FCBFlags & UDF_FCB_DIRECTORY)) {

//            CompleteRequest = TRUE;
            try_return(RC = STATUS_INVALID_PARAMETER);
        }

        CanWait = ((IrpContext->Flags & IRP_CONTEXT_FLAG_WAIT) ? TRUE : FALSE);

        // Acquire the FCB resource shared
        UDF_CHECK_PAGING_IO_RESOURCE(Fcb);
        if (!UDFAcquireResourceExclusive(&Fcb->MainResource, CanWait)) {
            PostRequest = TRUE;
            try_return(RC = STATUS_PENDING);
        }
        AcquiredFCB = TRUE;

        // If we don't have a file lock, then get one now.
        if ((Fcb->FileLock == NULL) && !UDFCreateFileLock(NULL, Fcb, FALSE)) {

            if (!UDFCreateFileLock(NULL, Fcb, FALSE)) {

                try_return(RC = STATUS_INSUFFICIENT_RESOURCES);
            }
        }

        RC = FsRtlProcessFileLock(Fcb->FileLock, Irp, NULL);
//        CompleteRequest = TRUE;

try_exit: NOTHING;

    } _SEH2_FINALLY {

        // Release the FCB resources if acquired.
        if (AcquiredFCB) {
            UDF_CHECK_PAGING_IO_RESOURCE(Fcb);
            UDFReleaseResource(&Fcb->MainResource);
            AcquiredFCB = FALSE;
        }
        if (PostRequest) {
            // Perform appropriate post related processing here
            RC = UDFPostRequest(IrpContext, Irp);
        }
    } _SEH2_END; // end of "__finally" processing

    return(RC);
} // end UDFCommonLockControl()


/*
Routine Description:
    This is a call back routine for doing the fast lock call.
Arguments:
    FileObject - Supplies the file object used in this operation
    FileOffset - Supplies the file offset used in this operation
    Length - Supplies the length used in this operation
    ProcessId - Supplies the process ID used in this operation
    Key - Supplies the key used in this operation
    FailImmediately - Indicates if the request should fail immediately
        if the lock cannot be granted.
    ExclusiveLock - Indicates if this is a request for an exclusive or
        shared lock
    IoStatus - Receives the Status if this operation is successful

Return Value:
    BOOLEAN - TRUE if this operation completed and FALSE if caller
        needs to take the long route.
*/

BOOLEAN
NTAPI
UDFFastLock (
    IN PFILE_OBJECT FileObject,
    IN PLARGE_INTEGER FileOffset,
    IN PLARGE_INTEGER Length,
    PEPROCESS ProcessId,
    ULONG Key,
    BOOLEAN FailImmediately,
    BOOLEAN ExclusiveLock,
    OUT PIO_STATUS_BLOCK IoStatus,
    IN PDEVICE_OBJECT DeviceObject
    )
{
    BOOLEAN Results = FALSE;

//    BOOLEAN             AcquiredFCB = FALSE;
    PFCB                  Fcb = NULL;
    PCCB                  Ccb = NULL;

    UDFPrint(("UDFFastLock\n"));
    //  Decode the type of file object we're being asked to process and make
    //  sure it is only a user file open.


    // Get the FCB and CCB pointers.
    Ccb = (PCCB)FileObject->FsContext2;
    ASSERT(Ccb);
    Fcb = Ccb->Fcb;
    ASSERT(Fcb);
    // Validate the sent-in FCB
    if ( (Fcb->NodeIdentifier.NodeTypeCode == UDF_NODE_TYPE_VCB) ||
         (Fcb->FCBFlags & UDF_FCB_DIRECTORY)) {

        IoStatus->Status = STATUS_INVALID_PARAMETER;
        IoStatus->Information = 0;
        return TRUE;
    }

    //  Acquire exclusive access to the Fcb this operation can always wait

    FsRtlEnterFileSystem();

    // BUGBUG: kenr
    // (VOID) ExAcquireResourceShared( Fcb->Header.Resource, TRUE );

    _SEH2_TRY {

        //  If we don't have a file lock, then get one now.
        if ((Fcb->FileLock == NULL) && !UDFCreateFileLock(NULL, Fcb, FALSE)) {

            try_return(NOTHING);
        }

        //  We check whether we can proceed
        //  based on the state of the file oplocks.

        //  Now call the FsRtl routine to do the actual processing of the
        //  Lock request
        if ((Results = FsRtlFastLock(Fcb->FileLock,
                                     FileObject,
                                     FileOffset,
                                     Length,
                                     ProcessId,
                                     Key,
                                     FailImmediately,
                                     ExclusiveLock,
                                     IoStatus,
                                     NULL,
                                     FALSE ))) {

            //  Set the flag indicating if Fast I/O is possible
            Fcb->Header.IsFastIoPossible = UDFIsFastIoPossible(Fcb);
        }

try_exit:  NOTHING;
    } _SEH2_FINALLY {

        //  Release the Fcb, and return to our caller

        // BUGBUG: kenr
        //    UDFReleaseResource( (Fcb)->Header.Resource );

        FsRtlExitFileSystem();

    } _SEH2_END;

    return Results;
} // end UDFFastLock()


/*
Routine Description:

    This is a call back routine for doing the fast unlock single call.

Arguments:

    FileObject - Supplies the file object used in this operation
    FileOffset - Supplies the file offset used in this operation
    Length - Supplies the length used in this operation
    ProcessId - Supplies the process ID used in this operation
    Key - Supplies the key used in this operation
    Status - Receives the Status if this operation is successful

Return Value:

    BOOLEAN - TRUE if this operation completed and FALSE if caller
        needs to take the long route.
*/
BOOLEAN
NTAPI
UDFFastUnlockSingle(
    IN PFILE_OBJECT FileObject,
    IN PLARGE_INTEGER FileOffset,
    IN PLARGE_INTEGER Length,
    PEPROCESS ProcessId,
    ULONG Key,
    OUT PIO_STATUS_BLOCK IoStatus,
    IN PDEVICE_OBJECT DeviceObject
    )

{
    BOOLEAN Results = FALSE;

//    BOOLEAN             AcquiredFCB = FALSE;
    PFCB                Fcb = NULL;
    PCCB                Ccb = NULL;

    UDFPrint(("UDFFastUnlockSingle\n"));
    //  Decode the type of file object we're being asked to process and make
    //  sure it is only a user file open.

    IoStatus->Information = 0;

    // Get the FCB and CCB pointers.
    Ccb = (PCCB)FileObject->FsContext2;
    ASSERT(Ccb);
    Fcb = Ccb->Fcb;
    ASSERT(Fcb);
    // Validate the sent-in FCB
    if ( (Fcb->NodeIdentifier.NodeTypeCode == UDF_NODE_TYPE_VCB) ||
         (Fcb->FCBFlags & UDF_FCB_DIRECTORY)) {

        IoStatus->Status = STATUS_INVALID_PARAMETER;
        return TRUE;
    }

    // If there is no lock then return immediately.
    if (Fcb->FileLock == NULL) {

        IoStatus->Status = STATUS_RANGE_NOT_LOCKED;
        return TRUE;
    }

    //  Acquire exclusive access to the Fcb this operation can always wait

    FsRtlEnterFileSystem();

    // BUGBUG: kenr
    // (VOID) ExAcquireResourceShared( Fcb->Header.Resource, TRUE );

    _SEH2_TRY {

        //  We check whether we can proceed
        //  based on the state of the file oplocks.

        //  Now call the FsRtl routine to do the actual processing of the
        //  Lock request
        Results = TRUE;
        IoStatus->Status = FsRtlFastUnlockSingle(Fcb->FileLock,
                                                 FileObject,
                                                 FileOffset,
                                                 Length,
                                                 ProcessId,
                                                 Key,
                                                 NULL,
                                                 FALSE);
        //  Set the flag indicating if Fast I/O is possible
        Fcb->Header.IsFastIoPossible = UDFIsFastIoPossible(Fcb);

//try_exit:  NOTHING;
    } _SEH2_FINALLY {

        //  Release the Fcb, and return to our caller

        // BUGBUG: kenr
        //    UDFReleaseResource( (Fcb)->Header.Resource );

        FsRtlExitFileSystem();

    } _SEH2_END;

    return Results;
} // end UDFFastUnlockSingle()


/*
Routine Description:

    This is a call back routine for doing the fast unlock all call.

Arguments:
    FileObject - Supplies the file object used in this operation
    ProcessId - Supplies the process ID used in this operation
    Status - Receives the Status if this operation is successful

Return Value:

    BOOLEAN - TRUE if this operation completed and FALSE if caller
        needs to take the long route.
*/
BOOLEAN
NTAPI
UDFFastUnlockAll(
    IN PFILE_OBJECT FileObject,
    PEPROCESS ProcessId,
    OUT PIO_STATUS_BLOCK IoStatus,
    IN PDEVICE_OBJECT DeviceObject
    )

{
    BOOLEAN Results = FALSE;

//    BOOLEAN             AcquiredFCB = FALSE;
    PFCB                Fcb = NULL;
    PCCB                Ccb = NULL;

    UDFPrint(("UDFFastUnlockAll\n"));

    IoStatus->Information = 0;
    //  Decode the type of file object we're being asked to process and make
    //  sure it is only a user file open.

    // Get the FCB and CCB pointers.
    Ccb = (PCCB)FileObject->FsContext2;
    ASSERT(Ccb);
    Fcb = Ccb->Fcb;
    ASSERT(Fcb);
    // Validate the sent-in FCB
    if ( (Fcb->NodeIdentifier.NodeTypeCode == UDF_NODE_TYPE_VCB) ||
         (Fcb->FCBFlags & UDF_FCB_DIRECTORY)) {

        IoStatus->Status = STATUS_INVALID_PARAMETER;
        return TRUE;
    }

    //  Acquire shared access to the Fcb this operation can always wait

    FsRtlEnterFileSystem();

    UDF_CHECK_PAGING_IO_RESOURCE(Fcb);
    UDFAcquireResourceShared(&Fcb->MainResource, TRUE);

    _SEH2_TRY {

        //  We check whether we can proceed
        //  based on the state of the file oplocks.

        //  If we don't have a file lock, then get one now.
        if ((Fcb->FileLock == NULL) && !UDFCreateFileLock(NULL, Fcb, FALSE)) {

            _SEH2_LEAVE;
        }

        //  Now call the FsRtl routine to do the actual processing of the
        //  Lock request
        Results = TRUE;
        IoStatus->Status = FsRtlFastUnlockAll(Fcb->FileLock,
                                              FileObject,
                                              ProcessId,
                                              NULL);

        //  Set the flag indicating if Fast I/O is questionable

        Fcb->Header.IsFastIoPossible = UDFIsFastIoPossible(Fcb);

//try_exit:  NOTHING;
    } _SEH2_FINALLY {

        //  Release the Fcb, and return to our caller

        UDF_CHECK_PAGING_IO_RESOURCE(Fcb);
        UDFReleaseResource(&Fcb->MainResource);
        FsRtlExitFileSystem();

    } _SEH2_END;

    return Results;
} // end UDFFastUnlockAll()


/*
Routine Description:

    This is a call back routine for doing the fast unlock all call.

Arguments:
    FileObject - Supplies the file object used in this operation
    ProcessId - Supplies the process ID used in this operation
    Status - Receives the Status if this operation is successful

Return Value:

    BOOLEAN - TRUE if this operation completed and FALSE if caller
        needs to take the long route.
*/

BOOLEAN
NTAPI
UDFFastUnlockAllByKey(
    IN PFILE_OBJECT FileObject,
    PEPROCESS ProcessId,
    ULONG Key,
    OUT PIO_STATUS_BLOCK IoStatus,
    IN PDEVICE_OBJECT DeviceObject
    )

{
    BOOLEAN Results = FALSE;

//    BOOLEAN             AcquiredFCB = FALSE;
    PFCB                Fcb = NULL;
    PCCB                Ccb = NULL;

    UDFPrint(("UDFFastUnlockAllByKey\n"));

    IoStatus->Information = 0;
    //  Decode the type of file object we're being asked to process and make
    //  sure it is only a user file open.

    // Get the FCB and CCB pointers.
    Ccb = (PCCB)FileObject->FsContext2;
    ASSERT(Ccb);
    Fcb = Ccb->Fcb;
    ASSERT(Fcb);
    // Validate the sent-in FCB
    if ( (Fcb->NodeIdentifier.NodeTypeCode == UDF_NODE_TYPE_VCB) ||
         (Fcb->FCBFlags & UDF_FCB_DIRECTORY)) {

        IoStatus->Status = STATUS_INVALID_PARAMETER;
        return TRUE;
    }

    //  Acquire shared access to the Fcb this operation can always wait

    FsRtlEnterFileSystem();

    UDF_CHECK_PAGING_IO_RESOURCE(Fcb);
    UDFAcquireResourceShared(&Fcb->MainResource, TRUE);

    _SEH2_TRY {

        //  We check whether we can proceed
        //  based on the state of the file oplocks.

        //  If we don't have a file lock, then get one now.
        if ((Fcb->FileLock == NULL) && !UDFCreateFileLock( NULL, Fcb, FALSE )) {

            _SEH2_LEAVE;
        }

        //  Now call the FsRtl routine to do the actual processing of the
        //  Lock request
        Results = TRUE;
        IoStatus->Status = FsRtlFastUnlockAllByKey(Fcb->FileLock,
                                                   FileObject,
                                                   ProcessId,
                                                   Key,
                                                   NULL);

        //  Set the flag indicating if Fast I/O is possible

        Fcb->Header.IsFastIoPossible = UDFIsFastIoPossible(Fcb);

//try_exit:  NOTHING;
    } _SEH2_FINALLY {

        //  Release the Fcb, and return to our caller

        UDF_CHECK_PAGING_IO_RESOURCE(Fcb);
        UDFReleaseResource(&Fcb->MainResource);
        FsRtlExitFileSystem();

    } _SEH2_END;

    return Results;
} // end UDFFastUnlockAllByKey()
