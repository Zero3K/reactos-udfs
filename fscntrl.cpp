////////////////////////////////////////////////////////////////////
// Copyright (C) Alexander Telyatnikov, Ivan Keliukh, Yegor Anchishkin, SKIF Software, 1999-2013. Kiev, Ukraine
// All rights reserved
// This file was released under the GPLv2 on June 2015.
////////////////////////////////////////////////////////////////////
/*

 Module Name: FsCntrl.cpp

 Abstract:

    Contains code to handle the "File System IOCTL" dispatch entry point.

 Environment:

    Kernel mode only

*/

#include            "udffs.h"

// define the file specific bug-check id
#define         UDF_BUG_CHECK_ID    UDF_FILE_FS_CONTROL

NTSTATUS UDFBlankMount(IN PVCB Vcb);

PDIR_INDEX_HDR UDFDirIndexAlloc(IN uint_di i);

/*
 Function: UDFFSControl()

 Description:
    The I/O Manager will invoke this routine to handle a File System
    Control request (this is IRP_MJ_FILE_SYSTEM_CONTROL dispatch point)

*/
NTSTATUS
NTAPI
UDFFSControl(
    PDEVICE_OBJECT      DeviceObject,      // the logical volume device object
    PIRP                Irp                // I/O Request Packet
    )
{
    NTSTATUS            RC = STATUS_SUCCESS;
    PIRP_CONTEXT IrpContext = NULL;
    BOOLEAN             AreWeTopLevel = FALSE;

    UDFPrint(("\nUDFFSControl: \n\n"));

    FsRtlEnterFileSystem();
    ASSERT(DeviceObject);
    ASSERT(Irp);

    // set the top level context
    AreWeTopLevel = UDFIsIrpTopLevel(Irp);

    _SEH2_TRY {

        // get an IRP context structure and issue the request
        IrpContext = UDFCreateIrpContext(Irp, DeviceObject);
        if(IrpContext) {
            RC = UDFCommonFSControl(IrpContext, Irp);
        } else {
            RC = STATUS_INSUFFICIENT_RESOURCES;
            Irp->IoStatus.Status = RC;
            Irp->IoStatus.Information = 0;
            // complete the IRP
            IoCompleteRequest(Irp, IO_DISK_INCREMENT);
        }

    } _SEH2_EXCEPT(UDFExceptionFilter(IrpContext, _SEH2_GetExceptionInformation())) {

        UDFPrintErr(("UDFFSControl: exception ***"));
        RC = UDFProcessException(IrpContext, Irp);

        UDFLogEvent(UDF_ERROR_INTERNAL_ERROR, RC);
    } _SEH2_END;

    if(AreWeTopLevel) {
        IoSetTopLevelIrp(NULL);
    }

    FsRtlExitFileSystem();

    return(RC);
} // end UDFFSControl()

/*
 Function: UDFCommonFSControl()

 Description:
    The actual work is performed here.

 Expected Interrupt Level (for execution) :
  IRQL_PASSIVE_LEVEL (invocation at higher IRQL will cause execution
    to be deferred to a worker thread context)

 Return Value: STATUS_SUCCESS/Error
*/

NTSTATUS
NTAPI
UDFCommonFSControl(
    PIRP_CONTEXT IrpContext,
    PIRP                Irp                // I/O Request Packet
    )
{
    NTSTATUS                RC = STATUS_UNRECOGNIZED_VOLUME;
    PIO_STACK_LOCATION      IrpSp = NULL;
//    PDEVICE_OBJECT          PtrTargetDeviceObject = NULL;

    UDFPrint(("\nUDFCommonFSControl\n\n"));
//    BrutePoint();

    _SEH2_TRY {

        IrpSp = IoGetCurrentIrpStackLocation(Irp);
        ASSERT(IrpSp);

        switch ((IrpSp)->MinorFunction)
        {
        case IRP_MN_USER_FS_REQUEST:
            UDFPrint(("  UDFFSControl: UserFsReq request ....\n"));

            RC = UDFUserFsCtrlRequest(IrpContext,Irp);
            break;
        case IRP_MN_MOUNT_VOLUME:

            UDFPrint(("  UDFFSControl: MOUNT_VOLUME request ....\n"));

            RC = UDFMountVolume(IrpContext,Irp);
            break;
        case IRP_MN_VERIFY_VOLUME:

            UDFPrint(("  UDFFSControl: VERIFY_VOLUME request ....\n"));

            RC = UDFVerifyVolume(Irp);
            break;
        default:
            UDFPrintErr(("  UDFFSControl: STATUS_INVALID_DEVICE_REQUEST MinorFunction %x\n", (IrpSp)->MinorFunction));
            RC = STATUS_INVALID_DEVICE_REQUEST;

            Irp->IoStatus.Status = RC;
            Irp->IoStatus.Information = 0;
            // complete the IRP
            IoCompleteRequest(Irp, IO_DISK_INCREMENT);
            break;
        }

//try_exit:   NOTHING;
    } _SEH2_FINALLY {
        if (!_SEH2_AbnormalTermination()) {
            // Free up the Irp Context
            UDFPrint(("  UDFCommonFSControl: finally\n"));
        } else {
            UDFPrint(("  UDFCommonFSControl: finally after exception ***\n"));
        }
    } _SEH2_END;

    return(RC);
} // end UDFCommonFSControl()

/*
Routine Description:
    This is the common routine for implementing the user's requests made
    through NtFsControlFile.

Arguments:
    Irp - Supplies the Irp being processed

Return Value:
    NTSTATUS - The return status for the operation

*/
NTSTATUS
NTAPI
UDFUserFsCtrlRequest(
    PIRP_CONTEXT IrpContext,
    PIRP             Irp
    )
{
    NTSTATUS RC;
    PEXTENDED_IO_STACK_LOCATION IrpSp = (PEXTENDED_IO_STACK_LOCATION) IoGetCurrentIrpStackLocation( Irp );

    //  Case on the control code.
    switch ( IrpSp->Parameters.FileSystemControl.FsControlCode ) {

    case FSCTL_REQUEST_OPLOCK_LEVEL_1 :
    case FSCTL_REQUEST_OPLOCK_LEVEL_2 :
    case FSCTL_REQUEST_BATCH_OPLOCK :
    case FSCTL_OPLOCK_BREAK_ACKNOWLEDGE :
    case FSCTL_OPBATCH_ACK_CLOSE_PENDING :
    case FSCTL_OPLOCK_BREAK_NOTIFY :
    case FSCTL_OPLOCK_BREAK_ACK_NO_2 :
    case FSCTL_REQUEST_FILTER_OPLOCK :

        UDFPrint(("UDFUserFsCtrlRequest: OPLOCKS\n"));
        RC = STATUS_INVALID_DEVICE_REQUEST;

        Irp->IoStatus.Information = 0;
        Irp->IoStatus.Status = STATUS_INVALID_DEVICE_REQUEST;
        break;
/*
        RC = UDFOplockRequest( IrpContext, Irp );
        break;
*/
    case FSCTL_INVALIDATE_VOLUMES :

        RC = UDFInvalidateVolumes( IrpContext, Irp );
        break;
/*
    case FSCTL_MOVE_FILE:

    case FSCTL_QUERY_ALLOCATED_RANGES:
    case FSCTL_SET_ZERO_DATA:
    case FSCTL_SET_SPARSE:

    case FSCTL_MARK_VOLUME_DIRTY:

        RC = UDFDirtyVolume( IrpContext, Irp );
        break;

  */
    case FSCTL_IS_VOLUME_DIRTY:

        RC = UDFIsVolumeDirty(IrpContext, Irp);
        break;

    case FSCTL_ALLOW_EXTENDED_DASD_IO:

        UDFPrint(("UDFUserFsCtrlRequest: FSCTL_ALLOW_EXTENDED_DASD_IO\n"));
        // DASD i/o is always permitted
        // So, no-op this call
        RC = STATUS_SUCCESS;

        Irp->IoStatus.Information = 0;
        Irp->IoStatus.Status = STATUS_SUCCESS;
        break;

    case FSCTL_DISMOUNT_VOLUME:

        RC = UDFDismountVolume( IrpContext, Irp );
        break;

    case FSCTL_IS_VOLUME_MOUNTED:

        RC = UDFIsVolumeMounted( IrpContext, Irp );
        break;

    case FSCTL_FILESYSTEM_GET_STATISTICS:

        RC = UDFGetStatistics( IrpContext, Irp );
        break;

    case FSCTL_LOCK_VOLUME:

        RC = UDFLockVolume( IrpContext, Irp );
        break;

    case FSCTL_UNLOCK_VOLUME:

        RC = UDFUnlockVolume( IrpContext, Irp );
        break;

    case FSCTL_IS_PATHNAME_VALID:

        RC = UDFIsPathnameValid( IrpContext, Irp );
        break;

    case FSCTL_GET_VOLUME_BITMAP:

        UDFPrint(("UDFUserFsCtrlRequest: FSCTL_GET_VOLUME_BITMAP\n"));
        RC = UDFGetVolumeBitmap( IrpContext, Irp );
        break;

    case FSCTL_GET_RETRIEVAL_POINTERS:

        UDFPrint(("UDFUserFsCtrlRequest: FSCTL_GET_RETRIEVAL_POINTERS\n"));
        RC = UDFGetRetrievalPointers( IrpContext, Irp, 0 );
        break;


    //  We don't support any of the known or unknown requests.
    default:

        UDFPrintErr(("UDFUserFsCtrlRequest: STATUS_INVALID_DEVICE_REQUEST for %x\n",
            IrpSp->Parameters.FileSystemControl.FsControlCode));
        RC = STATUS_INVALID_DEVICE_REQUEST;

        Irp->IoStatus.Information = 0;
        Irp->IoStatus.Status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }

    IoCompleteRequest(Irp,IO_DISK_INCREMENT);
    return RC;

} // end UDFUserFsCtrlRequest()


/*
Routine Description:
    This is the common routine for implementing the mount requests

Arguments:
    Irp - Supplies the Irp being processed

Return Value:
    NTSTATUS - The return status for the operation

*/
NTSTATUS
NTAPI
UDFMountVolume(
    IN PIRP_CONTEXT IrpContext,
    IN PIRP Irp
    )
{
    NTSTATUS                RC;
    PIO_STACK_LOCATION      IrpSp = IoGetCurrentIrpStackLocation(Irp);
    PDEVICE_OBJECT          TargetDeviceObject = NULL;
    PDEVICE_OBJECT          fsDeviceObject;
    PVPB                    Vpb = IrpSp->Parameters.MountVolume.Vpb;
    PVCB                    Vcb = NULL;
    PDEVICE_OBJECT          VolDo = NULL;
    IO_STATUS_BLOCK         Iosb;
    ULONG                   MediaChangeCount = 0;
    DEVICE_TYPE             FsDeviceType;
    BOOLEAN                 RestoreDoVerify = FALSE;
    BOOLEAN                 WrongMedia = FALSE;
    BOOLEAN                 RemovableMedia = TRUE;
    BOOLEAN                 CompleteIrp = FALSE;
    ULONG                   Mode;
    BOOLEAN                 VcbAcquired = FALSE;
    BOOLEAN                 DeviceNotTouched = TRUE;
    BOOLEAN                 Locked = FALSE;
    int8*                   ioBuf = NULL;

    ASSERT(IrpSp);
    UDFPrint(("\n !!! UDFMountVolume\n"));

    fsDeviceObject = IrpContext->TargetDeviceObject;
    UDFPrint(("Mount on device object %x\n", fsDeviceObject));

    PFILTER_DEV_EXTENSION filterDevExt = (PFILTER_DEV_EXTENSION)fsDeviceObject->DeviceExtension;
    if (filterDevExt->NodeIdentifier.NodeTypeCode == UDF_NODE_TYPE_FILTER_DEVOBJ &&
        filterDevExt->NodeIdentifier.NodeByteSize == sizeof(FILTER_DEV_EXTENSION)) {
        CompleteIrp = FALSE;
    } else if (filterDevExt->NodeIdentifier.NodeTypeCode == UDF_NODE_TYPE_UDFFS_DEVOBJ &&
        filterDevExt->NodeIdentifier.NodeByteSize == sizeof(UDFFS_DEV_EXTENSION)) {
        CompleteIrp = TRUE;
    } else {
        UDFPrintErr(("Invalid node type in FS or FILTER DeviceObject\n"));
        ASSERT(FALSE);
    }

    // Get a pointer to the target physical/virtual device object.
    TargetDeviceObject = IrpSp->Parameters.MountVolume.DeviceObject;

    if (FlagOn(TargetDeviceObject->Characteristics, FILE_FLOPPY_DISKETTE) ||
        FlagOn(UDFGlobalData.UDFFlags, UDF_DATA_FLAGS_BEING_UNLOADED)) {

        WrongMedia = TRUE;

    } else {

        RemovableMedia = FlagOn(TargetDeviceObject->Characteristics, FILE_REMOVABLE_MEDIA);

        if(TargetDeviceObject->DeviceType == FILE_DEVICE_CD_ROM) {
            FsDeviceType = FILE_DEVICE_CD_ROM_FILE_SYSTEM;
        } else
        if (TargetDeviceObject->DeviceType == FILE_DEVICE_DISK) {
            FsDeviceType = FILE_DEVICE_DISK_FILE_SYSTEM;
        } else {
            WrongMedia = TRUE;
        }
    }

    // Acquire GlobalDataResource
    UDFAcquireResourceExclusive(&(UDFGlobalData.GlobalDataResource), TRUE);

    _SEH2_TRY {

        UDFScanForDismountedVcb(IrpContext);

        if(WrongMedia) try_return(RC = STATUS_UNRECOGNIZED_VOLUME);

        if(RemovableMedia) {
            UDFPrint(("UDFMountVolume: removable media\n"));

            // Now we can get device state via GET_EVENT (if supported)
            // or still one TEST_UNIT_READY command
            RC = UDFPhSendIOCTL( IOCTL_STORAGE_CHECK_VERIFY,
                                 TargetDeviceObject,
                                 NULL,0,
                                 &MediaChangeCount,sizeof(ULONG),
                                 FALSE,&Iosb );

            if(RC == STATUS_IO_DEVICE_ERROR) {
                UDFPrint(("UDFMountVolume: retry check verify\n"));
                RC = UDFPhSendIOCTL( IOCTL_STORAGE_CHECK_VERIFY,
                                     TargetDeviceObject,
                                     NULL,0,
                                     &MediaChangeCount,sizeof(ULONG),
                                     FALSE,&Iosb );
            }

            if(!NT_SUCCESS(RC) && (RC != STATUS_VERIFY_REQUIRED))
                try_return(RC);

            //  Be safe about the count in case the driver didn't fill it in
            if(Iosb.Information != sizeof(ULONG)) {
                MediaChangeCount = 0;
            }

            if(FsDeviceType == FILE_DEVICE_CD_ROM_FILE_SYSTEM) {
                // Check if device is busy before locking tray and performing
                // further geomentry discovery. This is needed to avoid streaming
                // loss during CD-R recording. Note, that some recording tools
                // work with device via SPTI bypassing FS/Device driver layers.

                ioBuf = (int8*)MyAllocatePool__(NonPagedPool,4096);
                if(!ioBuf) {
                    try_return(RC = STATUS_INSUFFICIENT_RESOURCES);
                }
                RC = UDFPhSendIOCTL(IOCTL_CDROM_GET_DRIVE_GEOMETRY,TargetDeviceObject,
                    ioBuf,sizeof(DISK_GEOMETRY),
                    ioBuf,sizeof(DISK_GEOMETRY),
                    FALSE, NULL );

                if(RC == STATUS_DEVICE_NOT_READY) {
                    // probably, the device is really busy, may be by CD/DVD recording
                    UserPrint(("  busy (*)\n"));
                    try_return(RC);
                }
            }
        }

        // Now before we can initialize the Vcb we need to set up the
        // Get our device object and alignment requirement.
        // Device extension == VCB
        UDFPrint(("UDFMountVolume: create device\n"));
        RC = IoCreateDevice( UDFGlobalData.DriverObject,
                                 sizeof(VCB),
                                 NULL,
                                 FsDeviceType,
                                 0,
                                 FALSE,
                                 &VolDo );

        if(!NT_SUCCESS(RC)) try_return(RC);

        // Our alignment requirement is the larger of the processor alignment requirement
        // already in the volume device object and that in the DeviceObjectWeTalkTo
        if(TargetDeviceObject->AlignmentRequirement > VolDo->AlignmentRequirement) {
            VolDo->AlignmentRequirement = TargetDeviceObject->AlignmentRequirement;
        }

        VolDo->Flags &= ~DO_DEVICE_INITIALIZING;

        // device object field in the VPB to point to our new volume device
        // object.
        Vpb->DeviceObject = (PDEVICE_OBJECT) VolDo;

        // We must initialize the stack size in our device object before
        // the following reads, because the I/O system has not done it yet.
        ((PDEVICE_OBJECT)VolDo)->StackSize = (CCHAR) (TargetDeviceObject->StackSize + 1);

        Vcb = (PVCB)VolDo->DeviceExtension;

        // Initialize the Vcb.  This routine will raise on an allocation
        // failure.
        RC = UDFInitializeVCB(VolDo,TargetDeviceObject,Vpb);
        if(!NT_SUCCESS(RC)) {
            Vcb = NULL;
            try_return(RC);
        }

        VolDo = NULL;
        Vpb = NULL;

        // lock media for now
        if(RemovableMedia) {

            UDFToggleMediaEjectDisable(Vcb, TRUE);
            Locked = TRUE;
        }

        UDFAcquireResourceExclusive(&(Vcb->VCBResource), TRUE );
        VcbAcquired = TRUE;

        // Let's reference the Vpb to make sure we are the one to
        // have the last dereference.
        Vcb->Vpb->ReferenceCount ++;

        Vcb->MediaChangeCount = MediaChangeCount;
        Vcb->FsDeviceType = FsDeviceType;

        // Clear the verify bit for the start of mount.
        if(Vcb->Vpb->RealDevice->Flags & DO_VERIFY_VOLUME) {
            Vcb->Vpb->RealDevice->Flags &= ~DO_VERIFY_VOLUME;
            RestoreDoVerify = TRUE;
        }

        DeviceNotTouched = FALSE;
        RC = UDFGetDiskInfo(TargetDeviceObject,Vcb);
        if(!NT_SUCCESS(RC)) try_return(RC);

        //     ****  Read registry settings  ****
        UDFReadRegKeys(Vcb, FALSE, FALSE);

        Vcb->MountPhErrorCount = 0;

#ifdef UDF_USE_WCACHE
        // Initialize internal cache
        Mode = WCACHE_MODE_ROM;
        RC = WCacheInit__(&(Vcb->FastCache),
                          Vcb->WCacheMaxFrames,
                          Vcb->WCacheMaxBlocks,
                          Vcb->WriteBlockSize,
                          5, Vcb->BlockSizeBits,
                          Vcb->WCacheBlocksPerFrameSh,
                          0/*Vcb->FirstLBA*/, Vcb->LastPossibleLBA, Mode,
                              0/*WCACHE_CACHE_WHOLE_PACKET*/ |
                              (Vcb->DoNotCompareBeforeWrite ? WCACHE_DO_NOT_COMPARE : 0) |
                              (Vcb->CacheChainedIo ? WCACHE_CHAINED_IO : 0) |
                              WCACHE_MARK_BAD_BLOCKS | WCACHE_RO_BAD_BLOCKS,  // this will be cleared after mount
                          Vcb->WCacheFramesToKeepFree,
//                          UDFTWrite, UDFTRead,
                          UDFTWriteVerify, UDFTReadVerify,
#ifdef UDF_ASYNC_IO
                          UDFTWriteAsync, UDFTReadAsync,
#else  //UDF_ASYNC_IO
                          NULL, NULL,
#endif //UDF_ASYNC_IO
                          UDFIsBlockAllocated,
                          UDFUpdateVAT,
                          UDFWCacheErrorHandler);
        if(!NT_SUCCESS(RC)) try_return(RC);
#endif //UDF_USE_WCACHE

        RC = UDFVInit(Vcb);
        if(!NT_SUCCESS(RC)) try_return(RC);

        UDFAcquireResourceExclusive(&(Vcb->BitMapResource1),TRUE);
        RC = UDFGetDiskInfoAndVerify(TargetDeviceObject,Vcb);
        UDFReleaseResource(&(Vcb->BitMapResource1));

        ASSERT(!Vcb->Modified);
        WCacheChFlags__(&(Vcb->FastCache),
                        WCACHE_CACHE_WHOLE_PACKET, // enable cache whole packet
                        WCACHE_MARK_BAD_BLOCKS | WCACHE_RO_BAD_BLOCKS);  // let user retry request on Bad Blocks

        if(!NT_SUCCESS(RC)) {
            UDFPrint(("UDFMountVolume: try raw mount\n"));
            if(Vcb->NSRDesc & VRS_ISO9660_FOUND) {
                UDFPrint(("UDFMountVolume: block raw mount due to ISO9660 presence\n"));
                Vcb->VCBFlags &= ~VCB_STATE_RAW_DISK;
                try_return(RC);
            }
try_raw_mount:
            UDFPrint(("UDFMountVolume: try raw mount (2)\n"));
            if(Vcb->VCBFlags & VCB_STATE_RAW_DISK) {

                UDFPrint(("UDFMountVolume: trying raw mount...\n"));
                Vcb->VolIdent.Length =
                (Vcb->VolIdent.MaximumLength = sizeof(UDF_BLANK_VOLUME_LABEL)) - 2;
                if(Vcb->VolIdent.Buffer)
                    MyFreePool__(Vcb->VolIdent.Buffer);
                Vcb->VolIdent.Buffer = (PWCHAR)MyAllocatePool__(NonPagedPool, sizeof(UDF_BLANK_VOLUME_LABEL));
                if(!Vcb->VolIdent.Buffer)
                    try_return(STATUS_INSUFFICIENT_RESOURCES);
                RtlCopyMemory(Vcb->VolIdent.Buffer, UDF_BLANK_VOLUME_LABEL, sizeof(UDF_BLANK_VOLUME_LABEL));

                RC = UDFBlankMount(Vcb);
                if(!NT_SUCCESS(RC)) try_return(RC);

            } else {
//                Vcb->VCBFlags &= ~VCB_STATE_RAW_DISK;
                try_return(RC);
            }
        } else {
            Vcb->MountPhErrorCount = -1;

            // set cache mode according to media type
            if(!(Vcb->VCBFlags & VCB_STATE_MEDIA_WRITE_PROTECT)) {
                UDFPrint(("UDFMountVolume: writable volume\n"));
                if(!Vcb->CDR_Mode) {
                    if(FsDeviceType == FILE_DEVICE_DISK_FILE_SYSTEM) {
                        UDFPrint(("UDFMountVolume: RAM mode\n"));
                        Mode = WCACHE_MODE_RAM;
                    } else {
                        UDFPrint(("UDFMountVolume: RW mode\n"));
                        Mode = WCACHE_MODE_RW;
                    }
/*                    if(FsDeviceType == FILE_DEVICE_CD_ROM_FILE_SYSTEM) {
                    } else {
                        Vcb->WriteSecurity = TRUE;
                    }*/
                } else {
                    UDFPrint(("UDFMountVolume: R mode\n"));
                    Mode = WCACHE_MODE_R;
                }
                // we can't record ACL on old format disks
                if(!UDFNtAclSupported(Vcb)) {
                    UDFPrint(("UDFMountVolume: NO ACL and ExtFE support\n"));
                    Vcb->WriteSecurity = FALSE;
                    Vcb->UseExtendedFE = FALSE;
                }
            }
#ifdef UDF_USE_WCACHE
            WCacheSetMode__(&(Vcb->FastCache), Mode);
#endif //UDF_USE_WCACHE

            // Complete mount operations: create root FCB
            UDFAcquireResourceExclusive(&(Vcb->BitMapResource1),TRUE);
            RC = UDFCompleteMount(Vcb);
            UDFReleaseResource(&(Vcb->BitMapResource1));
            if(!NT_SUCCESS(RC)) {
                // We must have Vcb->VCBOpenCount = 1 for UDFBlankMount()
                // Thus, we should not decrement it here
                // Also, if we shall not perform BlankMount,
                // but simply cleanup and return error, Vcb->VCBOpenCount
                // will be decremented during cleanup. Thus anyway it must
                // stay 1 unchanged here
                //UDFInterlockedDecrement((PLONG)&(Vcb->VCBOpenCount));
                UDFCloseResidual(Vcb);
                Vcb->VCBOpenCount = 1;
                if(FsDeviceType == FILE_DEVICE_CD_ROM_FILE_SYSTEM)
                    Vcb->VCBFlags |= VCB_STATE_RAW_DISK;
                goto try_raw_mount;
            }
            Vcb->VCBFlags &= ~VCB_STATE_RAW_DISK;
        }

        if((Vcb->VCBFlags & VCB_STATE_MEDIA_WRITE_PROTECT)) {
            UDFPrint(("UDFMountVolume: RO mount\n"));
            Vcb->VCBFlags |= VCB_STATE_VOLUME_READ_ONLY;
        }

        Vcb->Vpb->SerialNumber = Vcb->PhSerialNumber;
        Vcb->Vpb->VolumeLabelLength = Vcb->VolIdent.Length;
        RtlCopyMemory( Vcb->Vpb->VolumeLabel,
                       Vcb->VolIdent.Buffer,
                       Vcb->VolIdent.Length );

        Vcb->VcbCondition = VcbMounted;

        UDFInterlockedDecrement((PLONG)&(Vcb->VCBOpenCount));
        Vcb->TotalAllocUnits = UDFGetTotalSpace(Vcb);
        Vcb->FreeAllocUnits = UDFGetFreeSpace(Vcb);

        // unlock media
        if(RemovableMedia) {
            if(Vcb->VCBFlags & VCB_STATE_MEDIA_WRITE_PROTECT || 
               Vcb->VCBFlags & VCB_STATE_VOLUME_READ_ONLY) {

                UDFPrint(("UDFMountVolume: unlock media on RO volume\n"));
                UDFToggleMediaEjectDisable(Vcb, FALSE);
            }
        }

        if (UDFGlobalData.MountEvent)
        {
            Vcb->IsVolumeJustMounted = TRUE;
            KeSetEvent(UDFGlobalData.MountEvent, 0, FALSE);
        }

        //  The new mount is complete.
        UDFReleaseResource( &(Vcb->VCBResource) );
        VcbAcquired = FALSE;
        Vcb = NULL;

        RC = STATUS_SUCCESS;

try_exit: NOTHING;
    } _SEH2_FINALLY {

        UDFPrint(("UDFMountVolume: RC = %x\n", RC));

        if(ioBuf) {
            MyFreePool__(ioBuf);
        }

        if(!NT_SUCCESS(RC)) {

            if(RemovableMedia && Locked) {

                UDFPrint(("UDFMountVolume: unlock media\n"));
                UDFToggleMediaEjectDisable(Vcb, FALSE);
            }
/*            if((RC != STATUS_DEVICE_NOT_READY) &&
               (RC != STATUS_NO_MEDIA_IN_DEVICE) ) {*/

            if(RC == STATUS_CRC_ERROR || RC == STATUS_FILE_CORRUPT_ERROR) {
                UDFPrint(("UDFMountVolume: status -> STATUS_UNRECOGNIZED_VOLUME\n"));
                RC = STATUS_UNRECOGNIZED_VOLUME;
            }

            // If we didn't complete the mount then cleanup any remaining structures.
            if(Vpb) {
               Vpb->DeviceObject = NULL;
            }

            if(Vcb) {
                // Restore the verify bit.
                if(RestoreDoVerify) {
                    Vcb->Vpb->RealDevice->Flags |= DO_VERIFY_VOLUME;
                }
                // Make sure there is no Vcb since it could go away
                if(Vcb->VCBOpenCount)
                    UDFInterlockedDecrement((PLONG)&(Vcb->VCBOpenCount));
                // This procedure will also delete the volume device object
                if(UDFDismountVcb( Vcb, VcbAcquired )) {
                    UDFReleaseResource( &(Vcb->VCBResource) );
                }
            } else if(VolDo) {
                IoDeleteDevice( VolDo );
            }
        }
        // Release the global resource.
        UDFReleaseResource( &(UDFGlobalData.GlobalDataResource) );

        if (CompleteIrp || NT_SUCCESS(RC)) {
            if(!_SEH2_AbnormalTermination()) {
                // Set mount event

                UDFPrint(("UDFMountVolume: complete req RC %x\n", RC));
                //TODO: fix BSOD with IRQL
                //FsRtlNotifyVolumeEvent(IrpSp->FileObject, FSRTL_VOLUME_MOUNT);
                // Complete the IRP.
                Irp->IoStatus.Status = RC;
                IoCompleteRequest(Irp, IO_NO_INCREMENT);
            }
        } else {
            // Pass Irp to lower driver (CDFS)

            // Get this driver out of the driver stack and get to the next driver as
            // quickly as possible.
            Irp->CurrentLocation++;
            Irp->Tail.Overlay.CurrentStackLocation++;

            // Now call the appropriate file system driver with the request.
            RC = IoCallDriver(filterDevExt->lowerFSDeviceObject, Irp);

        }

    } _SEH2_END;

    UDFPrint(("UDFMountVolume: final RC = %x\n", RC));
    return RC;

} // end UDFMountVolume()

NTSTATUS
UDFCompleteMount(
    IN PVCB    Vcb
    )
{
    NTSTATUS                    RC;// = STATUS_SUCCESS;
    UNICODE_STRING              LocalPath;
    PtrUDFObjectName            RootName;
    PFCB                        RootFcb;

    UDFPrint(("UDFCompleteMount:\n"));
    Vcb->ZBuffer = (PCHAR)DbgAllocatePoolWithTag(NonPagedPool, max(Vcb->LBlockSize, PAGE_SIZE), 'zNWD');
    if(!Vcb->ZBuffer) return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(Vcb->ZBuffer, Vcb->LBlockSize);

    UDFPrint(("UDFCompleteMount: alloc Root FCB\n"));
    // Create the root index and reference it in the Vcb.
    RootFcb =
    Vcb->RootDirFCB = UDFAllocateFCB();
    if (!RootFcb) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    UDFPrint(("UDFCompleteMount: alloc Root ObjName\n"));
    // Allocate and set root FCB unique name
    RootName = UDFAllocateObjectName();
    if(!RootName) {
        UDFCleanUpFCB(RootFcb);
        Vcb->RootDirFCB = NULL;
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RC = MyInitUnicodeString(&(RootName->ObjectName),UDF_ROOTDIR_NAME);
    if(!NT_SUCCESS(RC))
        goto insuf_res_1;

    RootFcb->FileInfo = (PUDF_FILE_INFO)MyAllocatePool__(NonPagedPool,sizeof(UDF_FILE_INFO));
    if(!RootFcb->FileInfo) {
        RC = STATUS_INSUFFICIENT_RESOURCES;
insuf_res_1:
        MyFreePool__(RootName->ObjectName.Buffer);
        UDFReleaseObjectName(RootName);
        UDFCleanUpFCB(RootFcb);
        Vcb->RootDirFCB = NULL;
        return RC;
    }
    UDFPrint(("UDFCompleteMount: open Root Dir\n"));
    // Open Root Directory
    RC = UDFOpenRootFile__( Vcb, &(Vcb->RootLbAddr), RootFcb->FileInfo );

    if (!NT_SUCCESS(RC)) {

        UDFCleanUpFile__(Vcb, RootFcb->FileInfo);
        MyFreePool__(RootFcb->FileInfo);
        goto insuf_res_1;
    }

    RootFcb->FileInfo->Fcb = RootFcb;

    if(!RootFcb->FileInfo->Dloc->CommonFcb) {
        RootFcb->FileInfo->Dloc->CommonFcb = RootFcb;
    }

    UDFPrint(("UDFCompleteMount: init FCB\n"));
    RC = UDFInitializeFCB(RootFcb,Vcb,RootName,UDF_FCB_ROOT_DIRECTORY | UDF_FCB_DIRECTORY,NULL);
    if(!NT_SUCCESS(RC)) {
        // if we get here, no resources are inited
        RootFcb->OpenHandleCount =
        RootFcb->ReferenceCount  =
        RootFcb->CommonRefCount = 0;

        UDFCleanUpFile__(Vcb, RootFcb->FileInfo);
        MyFreePool__(RootFcb->FileInfo);
        UDFCleanUpFCB(RootFcb);
        Vcb->RootDirFCB = NULL;
        return RC;
    }

    // this is a part of UDF_RESIDUAL_REFERENCE
    UDFInterlockedIncrement((PLONG)&(Vcb->VCBOpenCount));
    RootFcb->OpenHandleCount =
    RootFcb->ReferenceCount  =
    RootFcb->CommonRefCount = 1;

    UDFGetFileXTime(RootFcb->FileInfo,
                  &(RootFcb->CreationTime.QuadPart),
                  &(RootFcb->LastAccessTime.QuadPart),
                  &(RootFcb->ChangeTime.QuadPart),
                  &(RootFcb->LastWriteTime.QuadPart) );

    if(Vcb->SysStreamLbAddr.logicalBlockNum) {
        Vcb->SysSDirFileInfo = (PUDF_FILE_INFO)MyAllocatePool__(NonPagedPool,sizeof(UDF_FILE_INFO));
        if(!Vcb->SysSDirFileInfo) {
            RC = STATUS_INSUFFICIENT_RESOURCES;
            goto unwind_1;
        }
        // Open System SDir Directory
        RC = UDFOpenRootFile__( Vcb, &(Vcb->SysStreamLbAddr), Vcb->SysSDirFileInfo );
        if(!NT_SUCCESS(RC)) {
            UDFCleanUpFile__(Vcb, Vcb->SysSDirFileInfo);
            MyFreePool__(Vcb->SysSDirFileInfo);
            Vcb->SysSDirFileInfo = NULL;
            goto unwind_1;
        } else {
            Vcb->SysSDirFileInfo->Dloc->DataLoc.Flags |= EXTENT_FLAG_VERIFY;
        }
    }

    // Open Unallocatable space stream
    // Generally, it should be placed in SystemStreamDirectory, but some
    // stupid apps think that RootDirectory is much better place.... :((
    RC = MyInitUnicodeString(&LocalPath, UDF_FN_NON_ALLOCATABLE);
    if(NT_SUCCESS(RC)) {
        RC = UDFOpenFile__(Vcb, FALSE, TRUE, &LocalPath, RootFcb->FileInfo, &(Vcb->NonAllocFileInfo), NULL);
        MyFreePool__(LocalPath.Buffer);
    }
    if(!NT_SUCCESS(RC) && (RC != STATUS_OBJECT_NAME_NOT_FOUND)) {

//unwind_2:
        UDFCleanUpFile__(Vcb, Vcb->NonAllocFileInfo);
        Vcb->NonAllocFileInfo = NULL;
        // this was a part of UDF_RESIDUAL_REFERENCE
        UDFInterlockedDecrement((PLONG)&(Vcb->VCBOpenCount));
unwind_1:

        // UDFCloseResidual() will clean up everything

        return RC;
    }

    /* process Non-allocatable */
    if(NT_SUCCESS(RC)) {
        UDFMarkSpaceAsXXX(Vcb, Vcb->NonAllocFileInfo->Dloc, Vcb->NonAllocFileInfo->Dloc->DataLoc.Mapping, AS_USED); // used
        UDFDirIndex(UDFGetDirIndexByFileInfo(Vcb->NonAllocFileInfo), Vcb->NonAllocFileInfo->Index)->FI_Flags |= UDF_FI_FLAG_FI_INTERNAL;
    } else {
        /* try to read Non-allocatable from alternate locations */
        RC = MyInitUnicodeString(&LocalPath, UDF_FN_NON_ALLOCATABLE_2);
        if(!NT_SUCCESS(RC)) {
            goto unwind_1;
        }
        RC = UDFOpenFile__(Vcb, FALSE, TRUE, &LocalPath, RootFcb->FileInfo, &(Vcb->NonAllocFileInfo), NULL);
        MyFreePool__(LocalPath.Buffer);
        if(!NT_SUCCESS(RC) && (RC != STATUS_OBJECT_NAME_NOT_FOUND)) {
            goto unwind_1;
        }
        if(NT_SUCCESS(RC)) {
            UDFMarkSpaceAsXXX(Vcb, Vcb->NonAllocFileInfo->Dloc, Vcb->NonAllocFileInfo->Dloc->DataLoc.Mapping, AS_USED); // used
            UDFDirIndex(UDFGetDirIndexByFileInfo(Vcb->NonAllocFileInfo), Vcb->NonAllocFileInfo->Index)->FI_Flags |= UDF_FI_FLAG_FI_INTERNAL;
        } else
        if(Vcb->SysSDirFileInfo) {
            RC = MyInitUnicodeString(&LocalPath, UDF_SN_NON_ALLOCATABLE);
            if(!NT_SUCCESS(RC)) {
                goto unwind_1;
            }
            RC = UDFOpenFile__(Vcb, FALSE, TRUE, &LocalPath, Vcb->SysSDirFileInfo , &(Vcb->NonAllocFileInfo), NULL);
            MyFreePool__(LocalPath.Buffer);
            if(!NT_SUCCESS(RC) && (RC != STATUS_OBJECT_NAME_NOT_FOUND)) {
                goto unwind_1;
            }
            if(NT_SUCCESS(RC)) {
                UDFMarkSpaceAsXXX(Vcb, Vcb->NonAllocFileInfo->Dloc, Vcb->NonAllocFileInfo->Dloc->DataLoc.Mapping, AS_USED); // used
//                    UDFDirIndex(UDFGetDirIndexByFileInfo(Vcb->NonAllocFileInfo), Vcb->NonAllocFileInfo->Index)->FI_Flags |= UDF_FI_FLAG_FI_INTERNAL;
            } else {
                RC = STATUS_SUCCESS;
            }
        } else {
            RC = STATUS_SUCCESS;
        }
    }

    /* Read SN UID mapping */
    if(Vcb->SysSDirFileInfo) {
        RC = MyInitUnicodeString(&LocalPath, UDF_SN_UID_MAPPING);
        if(!NT_SUCCESS(RC))
            goto unwind_3;
        RC = UDFOpenFile__(Vcb, FALSE, TRUE, &LocalPath, Vcb->SysSDirFileInfo , &(Vcb->UniqueIDMapFileInfo), NULL);
        MyFreePool__(LocalPath.Buffer);
        if(!NT_SUCCESS(RC) && (RC != STATUS_OBJECT_NAME_NOT_FOUND)) {
unwind_3:
//            UDFCloseFile__(Vcb, Vcb->NonAllocFileInfo);
//            UDFCleanUpFile__(Vcb, Vcb->NonAllocFileInfo);
//            if(Vcb->NonAllocFileInfo)
//                MyFreePool__(Vcb->NonAllocFileInfo);
//            Vcb->NonAllocFileInfo = NULL;
            goto unwind_1;
        } else {
            Vcb->UniqueIDMapFileInfo->Dloc->DataLoc.Flags |= EXTENT_FLAG_VERIFY;
        }
        RC = STATUS_SUCCESS;
    }

    RC = STATUS_SUCCESS;

    // clear Modified flags. It was not real modify, just
    // bitmap construction
    Vcb->BitmapModified = FALSE;
    //Vcb->Modified = FALSE;
    UDFPreClrModified(Vcb);
    UDFClrModified(Vcb);
    // this is a part of UDF_RESIDUAL_REFERENCE
    UDFInterlockedIncrement((PLONG)&Vcb->VCBOpenCount);

    // Start initializing the fields contained in the Header.

    // DisAllow fast-IO for now.
//    RootFcb->Header->IsFastIoPossible = FastIoIsNotPossible;
    RootFcb->Header.IsFastIoPossible = FastIoIsPossible;

    // Initialize the MainResource and PagingIoResource pointers in
    // the CommonFCBHeader structure to point to the ERESOURCE structures we
    // have allocated and already initialized above.
//    RootFcb->Header.Resource = &RootFcb->MainResource;
//    RootFcb->Header.PagingIoResource = &RootFcb->PagingIoResource;

    // Initialize the file size values here.
    RootFcb->Header.AllocationSize.QuadPart = 0;
    RootFcb->Header.FileSize.QuadPart = 0;

    // The following will disable ValidDataLength support.
//    RootFcb->Header.ValidDataLength.QuadPart = 0x7FFFFFFFFFFFFFFFI64;
    RootFcb->Header.ValidDataLength.QuadPart = 0;

    if(!NT_SUCCESS(RC))
        return RC;

/*
    Vcb->CDBurnerVolumeValid = true;

    len =
    Vcb->CDBurnerVolume.Length = 256;
    Vcb->CDBurnerVolume.MaximumLength = 256;
    Vcb->CDBurnerVolume.Buffer = (PWCHAR)ExAllocatePool(NonPagedPool, 256);
    RC = RegTGetStringValue(NULL, REG_CD_BURNER_KEY_NAME, REG_CD_BURNER_VOLUME_NAME, Vcb->CDBurnerVolume.Buffer,
        len);
    Vcb->CDBurnerVolume.Length = (USHORT)(wcslen(Vcb->CDBurnerVolume.Buffer)*sizeof(WCHAR));

    if(RC != STATUS_OBJECT_NAME_NOT_FOUND && !NT_SUCCESS(RC) )
        return RC;

    if (NT_SUCCESS(RC)) {
        RtlWriteRegistryValue(RTL_REGISTRY_ABSOLUTE | RTL_REGISTRY_OPTIONAL,
                              REG_CD_BURNER_KEY_NAME, REG_CD_BURNER_VOLUME_NAME,
                              REG_SZ,L"",sizeof(L"")+1);

    } else {
        Vcb->CDBurnerVolumeValid = false;
        RC = STATUS_SUCCESS;
    }
*/
    ASSERT(!Vcb->Modified);

    return RC;
} // end UDFCompleteMount()

NTSTATUS
UDFBlankMount(
    IN PVCB    Vcb
    )
{
    NTSTATUS                    RC;// = STATUS_SUCCESS;
    PtrUDFObjectName            RootName;
    PFCB                        RootFcb;
    PDIR_INDEX_HDR hDirNdx;
    PDIR_INDEX_ITEM DirNdx;

    // Create the root index and reference it in the Vcb.
    RootFcb =
    Vcb->RootDirFCB = UDFAllocateFCB();
    if(!RootFcb) return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(RootFcb,sizeof(FCB));

    // Allocate and set root FCB unique name
    RootName = UDFAllocateObjectName();
    if(!RootName) {
//bl_unwind_2:
        UDFCleanUpFCB(RootFcb);
        Vcb->RootDirFCB = NULL;
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RC = MyInitUnicodeString(&(RootName->ObjectName),UDF_ROOTDIR_NAME);
    if(!NT_SUCCESS(RC))
        goto bl_unwind_1;

    RootFcb->NodeIdentifier.NodeTypeCode = UDF_NODE_TYPE_FCB;
    RootFcb->NodeIdentifier.NodeByteSize = sizeof(FCB);

    RootFcb->FileInfo = (PUDF_FILE_INFO)MyAllocatePool__(NonPagedPool,sizeof(UDF_FILE_INFO));
    if(!RootFcb->FileInfo) {
        MyFreePool__(RootName->ObjectName.Buffer);
        RC = STATUS_INSUFFICIENT_RESOURCES;
bl_unwind_1:
        UDFReleaseObjectName(RootName);
        UDFCleanUpFCB(RootFcb);
        Vcb->RootDirFCB = NULL;
        return RC;
    }
    RtlZeroMemory(RootFcb->FileInfo, sizeof(UDF_FILE_INFO));
    if(!OS_SUCCESS(RC = UDFStoreDloc(Vcb, RootFcb->FileInfo, 1))) {
        MyFreePool__(RootFcb->FileInfo);
        RootFcb->FileInfo = NULL;
        MyFreePool__(RootName->ObjectName.Buffer);
        goto  bl_unwind_1;
    }
    RootFcb->FileInfo->NextLinkedFile =
    RootFcb->FileInfo->PrevLinkedFile = RootFcb->FileInfo;

    hDirNdx = UDFDirIndexAlloc(2);
    DirNdx = UDFDirIndex(hDirNdx,0);
    DirNdx->FileCharacteristics = FILE_DIRECTORY;
    DirNdx->FI_Flags = UDF_FI_FLAG_SYS_ATTR;
    DirNdx->SysAttr = FILE_ATTRIBUTE_READONLY;
    RtlInitUnicodeString(&DirNdx->FName, L".");
    DirNdx->FileInfo = RootFcb->FileInfo;
    DirNdx->FI_Flags |= UDFBuildHashEntry(Vcb, &(DirNdx->FName), &(DirNdx->hashes), HASH_ALL | HASH_KEEP_NAME);

    DirNdx = UDFDirIndex(hDirNdx,1);
    DirNdx->FI_Flags = UDF_FI_FLAG_SYS_ATTR;
    if(Vcb->ShowBlankCd == 2) {
        DirNdx->FI_Flags |= UDF_FI_FLAG_FI_INTERNAL;
    }
    DirNdx->SysAttr = FILE_ATTRIBUTE_READONLY;
    RtlInitUnicodeString(&DirNdx->FName, L"Blank.CD");
    DirNdx->FI_Flags |= UDFBuildHashEntry(Vcb, &(DirNdx->FName), &(DirNdx->hashes), HASH_ALL);

    RootFcb->FileInfo->Dloc->DirIndex = hDirNdx;
    RootFcb->FileInfo->Fcb = RootFcb;

    if(!RootFcb->FileInfo->Dloc->CommonFcb) {
        RootFcb->FileInfo->Dloc->CommonFcb = RootFcb;
    }

    RC = UDFInitializeFCB(RootFcb, Vcb, RootName, UDF_FCB_ROOT_DIRECTORY | UDF_FCB_DIRECTORY,NULL);
    if(!NT_SUCCESS(RC)) {
        // if we get here, no resources are inited
        RootFcb->OpenHandleCount =
        RootFcb->ReferenceCount  =
        RootFcb->CommonRefCount = 0;

        UDFCleanUpFile__(Vcb, RootFcb->FileInfo);
        MyFreePool__(RootFcb->FileInfo);
        UDFCleanUpFCB(RootFcb);
        Vcb->RootDirFCB = NULL;
        return RC;
    }

    // this is a part of UDF_RESIDUAL_REFERENCE
    UDFInterlockedIncrement((PLONG)&(Vcb->VCBOpenCount));
    RootFcb->OpenHandleCount =
    RootFcb->ReferenceCount  =
    RootFcb->CommonRefCount =
    RootFcb->FileInfo->RefCount =
    RootFcb->FileInfo->Dloc->LinkRefCount = 1;

    // this is a part of UDF_RESIDUAL_REFERENCE
    UDFInterlockedIncrement((PLONG)&(Vcb->VCBOpenCount));

    // Start initializing the fields contained in the Header.

    // DisAllow fast-IO for now.
    RootFcb->Header.IsFastIoPossible = FastIoIsNotPossible;

    // Initialize the MainResource and PagingIoResource pointers in
    // the CommonFCBHeader structure to point to the ERESOURCE structures we
    // have allocated and already initialized above.
    RootFcb->Header.Resource = &RootFcb->MainResource;
    RootFcb->Header.PagingIoResource = &RootFcb->PagingIoResource;

    // Initialize the file size values here.
    RootFcb->Header.AllocationSize.QuadPart = 0;
    RootFcb->Header.FileSize.QuadPart = 0;

    // The following will disable ValidDataLength support.
    RootFcb->Header.ValidDataLength.QuadPart = 0x7FFFFFFFFFFFFFFFLL;

    return RC;
} // end UDFBlankMount()

VOID
UDFCloseResidual(
    IN PVCB Vcb
    )
{
    //  Deinitialize Non-alloc file
    if(Vcb->VCBOpenCount)
        UDFInterlockedDecrement((PLONG)&(Vcb->VCBOpenCount));
    UDFPrint(("UDFCloseResidual: NonAllocFileInfo %x\n", Vcb->NonAllocFileInfo));
    if(Vcb->NonAllocFileInfo) {
        UDFCloseFile__(Vcb,Vcb->NonAllocFileInfo);
        UDFCleanUpFile__(Vcb, Vcb->NonAllocFileInfo);
        MyFreePool__(Vcb->NonAllocFileInfo);
        Vcb->NonAllocFileInfo = NULL;
    }
    //  Deinitialize Unique ID Mapping
    UDFPrint(("UDFCloseResidual: NonAllocFileInfo %x\n", Vcb->NonAllocFileInfo));
    if(Vcb->UniqueIDMapFileInfo) {
        UDFCloseFile__(Vcb,Vcb->UniqueIDMapFileInfo);
        UDFCleanUpFile__(Vcb, Vcb->UniqueIDMapFileInfo);
        MyFreePool__(Vcb->UniqueIDMapFileInfo);
        Vcb->UniqueIDMapFileInfo = NULL;
    }
    //  Deinitialize VAT file
    UDFPrint(("UDFCloseResidual: VatFileInfo %x\n", Vcb->VatFileInfo));
    if(Vcb->VatFileInfo) {
        UDFCloseFile__(Vcb,Vcb->VatFileInfo);
        UDFCleanUpFile__(Vcb, Vcb->VatFileInfo);
        MyFreePool__(Vcb->VatFileInfo);
        Vcb->VatFileInfo = NULL;
    }
    //  System StreamDir
    UDFPrint(("UDFCloseResidual: SysSDirFileInfo %x\n", Vcb->SysSDirFileInfo));
    if(Vcb->SysSDirFileInfo) {
        UDFCloseFile__(Vcb, Vcb->SysSDirFileInfo);
        UDFCleanUpFile__(Vcb, Vcb->SysSDirFileInfo);
        MyFreePool__(Vcb->SysSDirFileInfo);
        Vcb->SysSDirFileInfo = NULL;
    }
/*    //  Deinitialize root dir fcb
    if(Vcb->RootDirFCB) {
        UDFCloseFile__(Vcb,Vcb->RootDirFCB->FileInfo);
        UDFCleanUpFile__(Vcb, Vcb->RootDirFCB->FileInfo);
        MyFreePool__(Vcb->RootDirFCB->FileInfo);
        UDFCleanUpFCB(Vcb->RootDirFCB);
        //  Remove root FCB reference in vcb
        if(Vcb->VCBOpenCount) Vcb->VCBOpenCount--;
    }

    // Deinitialize Non-alloc file
    if(Vcb->VCBOpenCount) Vcb->VCBOpenCount--;
    if(Vcb->NonAllocFileInfo) {
        UDFCloseFile__(Vcb,Vcb->NonAllocFileInfo);
        // We must release VCB here !!!!
//        UDFCleanUpFcbChain(Vcb, Vcb->NonAllocFileInfo, 1);
        Vcb->NonAllocFileInfo = NULL;
    }
    // Deinitialize VAT file
    if(Vcb->VatFileInfo) {
        UDFCloseFile__(Vcb,Vcb->VatFileInfo);
        // We must release VCB here !!!!
//        UDFCleanUpFcbChain(Vcb, Vcb->VatFileInfo, 1);
        Vcb->VatFileInfo = NULL;
    }*/

    // Deinitialize root dir fcb
    UDFPrint(("UDFCloseResidual: RootDirFCB %x\n", Vcb->RootDirFCB));
    if(Vcb->RootDirFCB) {
        UDFCloseFile__(Vcb,Vcb->RootDirFCB->FileInfo);
        if(Vcb->RootDirFCB->OpenHandleCount)
            Vcb->RootDirFCB->OpenHandleCount--;
        UDFCleanUpFcbChain(Vcb, Vcb->RootDirFCB->FileInfo, 1, TRUE);
        // Remove root FCB reference in vcb
        if(Vcb->VCBOpenCount)
            UDFInterlockedDecrement((PLONG)&(Vcb->VCBOpenCount));
        Vcb->RootDirFCB = NULL;
    }
} // end UDFCloseResidual()

VOID
UDFCleanupVCB(
    IN PVCB Vcb
    )
{
    _SEH2_TRY {
        UDFReleaseFileIdCache(Vcb);
        UDFReleaseDlocList(Vcb);
    } _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        BrutePoint();
    } _SEH2_END;

    MyFreeMemoryAndPointer(Vcb->Partitions);
    MyFreeMemoryAndPointer(Vcb->LVid);
    MyFreeMemoryAndPointer(Vcb->Vat);
    MyFreeMemoryAndPointer(Vcb->SparingTable);

    if(Vcb->FSBM_Bitmap) {
        DbgFreePool(Vcb->FSBM_Bitmap);
        Vcb->FSBM_Bitmap = NULL;
    }
    if(Vcb->ZSBM_Bitmap) {
        DbgFreePool(Vcb->ZSBM_Bitmap);
        Vcb->ZSBM_Bitmap = NULL;
    }
    if(Vcb->BSBM_Bitmap) {
        DbgFreePool(Vcb->BSBM_Bitmap);
        Vcb->BSBM_Bitmap = NULL;
    }
#ifdef UDF_TRACK_ONDISK_ALLOCATION_OWNERS
    if(Vcb->FSBM_Bitmap_owners) {
        DbgFreePool(Vcb->FSBM_Bitmap_owners);
        Vcb->FSBM_Bitmap_owners = NULL;
    }
#endif //UDF_TRACK_ONDISK_ALLOCATION_OWNERS
    if(Vcb->FSBM_OldBitmap) {
        DbgFreePool(Vcb->FSBM_OldBitmap);
        Vcb->FSBM_OldBitmap = NULL;
    }

    MyFreeMemoryAndPointer(Vcb->Statistics);
    MyFreeMemoryAndPointer(Vcb->VolIdent.Buffer);

    if(Vcb->ZBuffer) {
        DbgFreePool(Vcb->ZBuffer);
        Vcb->ZBuffer = NULL;
    }

    if(Vcb->fZBuffer) {
        DbgFreePool(Vcb->fZBuffer);
        Vcb->fZBuffer = NULL;
    }

    MyFreeMemoryAndPointer(Vcb->TrackMap);

} // end UDFCleanupVCB()

/*

Routine Description:

    This routine walks through the list of Vcb's looking for any which may
    now be deleted.  They may have been left on the list because there were
    outstanding references.

Arguments:

Return Value:

    None

*/
VOID
UDFScanForDismountedVcb(
    IN PIRP_CONTEXT IrpContext
    )
{
    PVCB Vcb;
    PLIST_ENTRY Link;


    // Walk through all of the Vcb's attached to the global data.
    Link = UDFGlobalData.VCBQueue.Flink;

    while (Link != &(UDFGlobalData.VCBQueue)) {

        Vcb = CONTAINING_RECORD( Link, VCB, NextVCB );

        // Move to the next link now since the current Vcb may be deleted.
        Link = Link->Flink;

        // If dismount is already underway then check if this Vcb can
        // go away.
        if ((Vcb->VcbCondition == VcbDismountInProgress) ||
            (Vcb->VcbCondition == VcbInvalid) ||
            ((Vcb->VcbCondition == VcbNotMounted) && (Vcb->VCBOpenCount <= UDF_RESIDUAL_REFERENCE))) {

            UDFCheckForDismount(IrpContext, Vcb, FALSE);
        }
    }

    return;
} // end UDFScanForDismountedVcb()

/*
Routine Description:
    This routine determines if a volume is currently mounted.

Arguments:
    Irp - Supplies the Irp to process

Return Value:
    NTSTATUS - The return status for the operation

*/
NTSTATUS
UDFIsVolumeMounted(
    IN PIRP_CONTEXT IrpContext,
    IN PIRP Irp
    )
{
    PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation( Irp );

    UDFPrint(("UDFIsVolumeMounted\n"));

    PCCB Ccb = (PCCB)IrpSp->FileObject->FsContext2;
    if(!Ccb) {
        UDFPrintErr(("  !Ccb\n"));
        Irp->IoStatus.Information = 0;
        Irp->IoStatus.Status = STATUS_INVALID_PARAMETER;
        return STATUS_INVALID_PARAMETER;
    }

    PFCB Fcb = Ccb->Fcb;

    if(Fcb &&
       !(Fcb->Vcb->VCBFlags & VCB_STATE_RAW_DISK) &&
       !(Fcb->Vcb->VCBFlags & VCB_STATE_VOLUME_LOCKED) ) {

        // Disable PopUps, we want to return any error.
        IrpContext->Flags |= IRP_CONTEXT_FLAG_DISABLE_POPUPS;

        // Verify the Vcb.  This will raise in the error condition.
        UDFVerifyVcb( IrpContext, Fcb->Vcb );
    }

    Irp->IoStatus.Information = 0;
    Irp->IoStatus.Status = STATUS_SUCCESS;

    return STATUS_SUCCESS;
} // end UDFIsVolumeMounted()

/*
    This routine returns the filesystem performance counters from the
    appropriate VCB.

Arguments:
    Irp - Supplies the Irp to process

Return Value:
    NTSTATUS - The return status for the operation
*/
NTSTATUS
UDFGetStatistics(
    IN PIRP_CONTEXT IrpContext,
    IN PIRP Irp
    )
{
    PEXTENDED_IO_STACK_LOCATION IrpSp = (PEXTENDED_IO_STACK_LOCATION)IoGetCurrentIrpStackLocation( Irp );
    NTSTATUS status;
    PVCB Vcb;

    PFILE_SYSTEM_STATISTICS Buffer;
    ULONG BufferLength;
    ULONG StatsSize;
    ULONG BytesToCopy;

    UDFPrint(("UDFGetStatistics\n"));

    // Extract the buffer
    BufferLength = IrpSp->Parameters.FileSystemControl.OutputBufferLength;
    //  Get a pointer to the output buffer.
    Buffer = (PFILE_SYSTEM_STATISTICS)(Irp->AssociatedIrp.SystemBuffer);

    //  Make sure the buffer is big enough for at least the common part.
    if (BufferLength < sizeof(FILESYSTEM_STATISTICS)) {
        status = STATUS_BUFFER_TOO_SMALL;
        Irp->IoStatus.Information = 0;
        goto EO_stat;
    }

    //  Now see how many bytes we can copy.
    StatsSize = sizeof(FILE_SYSTEM_STATISTICS) * KeNumberProcessors;
    if (BufferLength < StatsSize) {
        BytesToCopy = BufferLength;
        status = STATUS_BUFFER_OVERFLOW;
    } else {
        BytesToCopy = StatsSize;
        status =  STATUS_SUCCESS;
    }

    Vcb = (PVCB)(((PDEVICE_OBJECT)IrpSp->DeviceObject)->DeviceExtension);
    //  Fill in the output buffer
    RtlCopyMemory( Buffer, Vcb->Statistics, BytesToCopy );
    Irp->IoStatus.Information = BytesToCopy;
EO_stat:
    Irp->IoStatus.Status = status;

    return status;
} // end UDFGetStatistics()


/*
    This routine determines if pathname is valid path for UDF Filesystem

Arguments:
    Irp - Supplies the Irp to process

Return Value:
    NTSTATUS - The return status for the operation
*/
NTSTATUS
UDFIsPathnameValid(
    IN PIRP_CONTEXT IrpContext,
    IN PIRP Irp
    )
{
    PEXTENDED_IO_STACK_LOCATION IrpSp = (PEXTENDED_IO_STACK_LOCATION)IoGetCurrentIrpStackLocation( Irp );
    NTSTATUS                    RC;
    PPATHNAME_BUFFER            PathnameBuffer;
    UNICODE_STRING              PathName;
    UNICODE_STRING              CurName;
    PWCHAR                      TmpBuffer;

    UDFPrint(("UDFIsPathnameValid\n"));

    // Extract the pathname
    PathnameBuffer = (PPATHNAME_BUFFER)Irp->AssociatedIrp.SystemBuffer;
    PathName.Buffer = PathnameBuffer->Name;
    PathName.Length = (USHORT)PathnameBuffer->PathNameLength;

    _SEH2_TRY {
        //  Check for an invalid buffer
        if (FIELD_OFFSET(PATHNAME_BUFFER, Name[0]) + PathnameBuffer->PathNameLength >
            IrpSp->Parameters.FileSystemControl.InputBufferLength) {
            try_return( RC = STATUS_INVALID_PARAMETER);
        }
        while (TRUE) {
            // get next path part...
            TmpBuffer = PathName.Buffer;
            PathName.Buffer = UDFDissectName(PathName.Buffer,&(CurName.Length) );
            PathName.Length -= (USHORT)((ULONG_PTR)(PathName.Buffer) - (ULONG_PTR)TmpBuffer);
            CurName.Buffer = PathName.Buffer - CurName.Length;
            CurName.Length *= sizeof(WCHAR);
            CurName.MaximumLength -= CurName.Length;

            if (CurName.Length) {
                // check path fragment size
                if (CurName.Length > UDF_NAME_LEN*sizeof(WCHAR)) {
                    try_return(RC = STATUS_OBJECT_NAME_INVALID);
                }
                if (!UDFIsNameValid(&CurName, NULL, NULL)) {
                    try_return(RC = STATUS_OBJECT_NAME_INVALID);
                }
            } else {
                try_return(RC = STATUS_SUCCESS);
            }
        }
try_exit:   NOTHING;
    } _SEH2_FINALLY {
        Irp->IoStatus.Information = 0;
        Irp->IoStatus.Status = RC;
    } _SEH2_END;

    return RC;
} // end UDFIsPathnameValid()

/*
    This routine performs the actual unlock volume operation.
    The volume must be held exclusive by the caller.

Arguments:
    Vcb - The volume being locked.
    FileObject - File corresponding to the handle locking the volume.  If this
        is not specified, a system lock is assumed.

Return Value:
    NTSTATUS - The return status for the operation
    Attempting to remove a system lock that did not exist is OK.
*/
NTSTATUS
UDFUnlockVolumeInternal (
    IN PVCB Vcb,
    IN PFILE_OBJECT FileObject OPTIONAL
    )
{
    KIRQL SavedIrql;
    NTSTATUS Status = STATUS_NOT_LOCKED;

    IoAcquireVpbSpinLock(&SavedIrql);

    if (FlagOn(Vcb->Vpb->Flags, VPB_LOCKED) && FileObject == Vcb->VolumeLockFileObject) {

        // This one locked it, unlock the volume
        ClearFlag(Vcb->Vpb->Flags, VPB_LOCKED | VPB_DIRECT_WRITES_ALLOWED);
        ClearFlag(Vcb->VCBFlags, VCB_STATE_VOLUME_LOCKED);
        Vcb->VolumeLockFileObject = NULL;

        Status = STATUS_SUCCESS;
    }

    IoReleaseVpbSpinLock(SavedIrql);

    return Status;
} // end UDFUnlockVolumeInternal()

/*
    This routine performs the lock volume operation.  It is responsible for
    either completing of enqueuing the input Irp.
Arguments:
    Irp - Supplies the Irp to process
Return Value:
    NTSTATUS - The return status for the operation
*/
NTSTATUS
UDFLockVolume(
    IN PIRP_CONTEXT IrpContext,
    IN PIRP             Irp,
    IN ULONG            PID
    )
{
    NTSTATUS RC;

    KIRQL SavedIrql;
    PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation( Irp );

    BOOLEAN VcbAcquired = FALSE;

    UDFPrint(("UDFLockVolume: PID %x\n", PID));

    //  Decode the file object, the only type of opens we accept are
    //  user volume opens.
    PCCB Ccb = (PCCB)IrpSp->FileObject->FsContext2;
    if(!Ccb) {
        UDFPrintErr(("  !Ccb\n"));
        Irp->IoStatus.Information = 0;
        Irp->IoStatus.Status = STATUS_INVALID_PARAMETER;
        return STATUS_INVALID_PARAMETER;
    }

    PFCB Fcb = Ccb->Fcb;
    PVCB Vcb = Fcb->Vcb;

    // Check for volume open
    if (Vcb != (PVCB)Fcb || !(Ccb->CCBFlags & UDF_CCB_VOLUME_OPEN)) {
        Irp->IoStatus.Information = 0;
        Irp->IoStatus.Status = STATUS_INVALID_PARAMETER;
        return STATUS_INVALID_PARAMETER;
    }

    FsRtlNotifyVolumeEvent(IrpSp->FileObject, FSRTL_VOLUME_LOCK);

    _SEH2_TRY {

        if(!(Vcb->VCBFlags & VCB_STATE_RAW_DISK))
            UDFCloseAllSystemDelayedInDir(Vcb, Vcb->RootDirFCB->FileInfo);
#ifdef UDF_DELAYED_CLOSE
        UDFCloseAllDelayed(Vcb);
#endif //UDF_DELAYED_CLOSE

        //  Acquire exclusive access to the Vcb.
        UDFAcquireResourceExclusive(&(Vcb->VCBResource), TRUE );
        VcbAcquired = TRUE;

        //  Verify the Vcb.
        UDFVerifyVcb( IrpContext, Vcb );

        //  If the volume is already locked then complete with success if this file
        //  object has the volume locked, fail otherwise.
/*        if (Vcb->VCBFlags & VCB_STATE_VOLUME_LOCKED) {

            if (Vcb->VolumeLockFileObject == IrpSp->FileObject) {
                RC = STATUS_SUCCESS;
            } else {
                RC = STATUS_ACCESS_DENIED;
            }
        //  If the open count for the volume is greater than 1 then this request
        //  will fail.
        } else if (Vcb->VCBOpenCount > UDF_RESIDUAL_REFERENCE+1) {
            RC = STATUS_ACCESS_DENIED;
        //  We will try to get rid of all of the user references.  If there is only one
        //  remaining after the purge then we can allow the volume to be locked.
        } else {
            // flush system cache
            UDFReleaseResource( &(Vcb->VCBResource) );
            VcbAcquired = FALSE;
        }*/

    } _SEH2_FINALLY {

        //  Release the Vcb.
        if(VcbAcquired) {
            UDFReleaseResource( &(Vcb->VCBResource) );
            VcbAcquired = FALSE;
        }
    } _SEH2_END;

    UDFAcquireResourceExclusive(&(Vcb->VCBResource), TRUE );
    VcbAcquired = TRUE;
    UDFFlushLogicalVolume(NULL, NULL, Vcb/*, 0*/);
    UDFReleaseResource( &(Vcb->VCBResource) );
    VcbAcquired = FALSE;
    //  Check if the Vcb is already locked, or if the open file count
    //  is greater than 1 (which implies that someone else also is
    //  currently using the volume, or a file on the volume).
    IoAcquireVpbSpinLock( &SavedIrql );

    if (!(Vcb->Vpb->Flags & VPB_LOCKED) &&
        (Vcb->VolumeLockPID == (ULONG)-1) &&
        (Vcb->VCBOpenCount <= UDF_RESIDUAL_REFERENCE+1) &&
        (Vcb->Vpb->ReferenceCount == 2)) {

        // Mark volume as locked
        if(PID == (ULONG)-1) {
            Vcb->Vpb->Flags |= VPB_LOCKED;
        }
        Vcb->VCBFlags |= VCB_STATE_VOLUME_LOCKED;
        Vcb->VolumeLockFileObject = IrpSp->FileObject;
        Vcb->VolumeLockPID        = PID;

        RC = STATUS_SUCCESS;

    } else {

        RC = STATUS_ACCESS_DENIED;
    }

    IoReleaseVpbSpinLock( SavedIrql );

    if(!NT_SUCCESS(RC)) {
        FsRtlNotifyVolumeEvent(IrpSp->FileObject, FSRTL_VOLUME_LOCK_FAILED);
    }

    //  Complete the request if there haven't been any exceptions.
    Irp->IoStatus.Information = 0;
    Irp->IoStatus.Status = RC;
    return RC;
} // end UDFLockVolume()

/*
    This routine performs the unlock volume operation.  It is responsible for
    either completing of enqueuing the input Irp.
Arguments:
    Irp - Supplies the Irp to process
Return Value:
    NTSTATUS - The return status for the operation
*/
NTSTATUS
UDFUnlockVolume(
    IN PIRP_CONTEXT IrpContext,
    IN PIRP Irp
    )
{
    NTSTATUS Status;

    PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);

    //  Decode the file object, the only type of opens we accept are
    //  user volume opens.
    PCCB Ccb = (PCCB)IrpSp->FileObject->FsContext2;
    if(!Ccb) {
        UDFPrintErr(("  !Ccb\n"));
        Irp->IoStatus.Information = 0;
        Irp->IoStatus.Status = STATUS_INVALID_PARAMETER;
        return STATUS_INVALID_PARAMETER;
    }

    PFCB Fcb = Ccb->Fcb;
    PVCB Vcb = Fcb->Vcb;

    // Check for volume open
    if(Vcb != (PVCB)Fcb || !(Ccb->CCBFlags & UDF_CCB_VOLUME_OPEN)) {
        Irp->IoStatus.Information = 0;
        Irp->IoStatus.Status = STATUS_INVALID_PARAMETER;
        return STATUS_INVALID_PARAMETER;
    }

    // Acquire the volume resource exclusive
    UDFAcquireResourceExclusive(&Vcb->VCBResource, TRUE);

    // We won't check for a valid Vcb for this request.  An unlock will always
    // succeed on a locked volume.
    Status = UDFUnlockVolumeInternal(Vcb, IrpSp->FileObject);

    // Release all of our resources
    UDFReleaseResource(&Vcb->VCBResource);

    // Send notification that the volume is avaliable.
    if (NT_SUCCESS(Status)) {

        FsRtlNotifyVolumeEvent(IrpSp->FileObject, FSRTL_VOLUME_UNLOCK);
    }

    //  Complete the request if there haven't been any exceptions.
    Irp->IoStatus.Information = 0;
    Irp->IoStatus.Status = Status;
    return Status;
} // end UDFUnlockVolume()


/*
    This routine performs the dismount volume operation.  It is responsible for
    either completing of enqueuing the input Irp.  We only dismount a volume which
    has been locked.  The intent here is that someone has locked the volume (they are the
    only remaining handle).  We set the verify bit here and the user will close his handle.
    We will dismount a volume with no user's handles in the verify path.
Arguments:
    Irp - Supplies the Irp to process
Return Value:
    NTSTATUS - The return status for the operation
*/
NTSTATUS
UDFDismountVolume(
    IN PIRP_CONTEXT IrpContext,
    IN PIRP Irp
    )
{
    NTSTATUS RC;

    PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation( Irp );
    BOOLEAN VcbAcquired = FALSE;

    UDFPrint(("\n ### UDFDismountVolume ###\n\n"));

    //  Decode the file object, the only type of opens we accept are
    //  user volume opens.
    PCCB Ccb = (PCCB)IrpSp->FileObject->FsContext2;
    if(!Ccb) {
        UDFPrintErr(("  !Ccb\n"));
        Irp->IoStatus.Information = 0;
        Irp->IoStatus.Status = STATUS_INVALID_PARAMETER;
        return STATUS_INVALID_PARAMETER;
    }

    PFCB Fcb = Ccb->Fcb;
    PVCB Vcb = Fcb->Vcb;

    // Check for volume open
    if(Vcb != (PVCB)Fcb || !(Ccb->CCBFlags & UDF_CCB_VOLUME_OPEN)) {
        Irp->IoStatus.Information = 0;
        Irp->IoStatus.Status = STATUS_INVALID_PARAMETER;
        return STATUS_INVALID_PARAMETER;
    }

    FsRtlNotifyVolumeEvent(IrpSp->FileObject, FSRTL_VOLUME_DISMOUNT);

    if(!(Vcb->VCBFlags & VCB_STATE_RAW_DISK))
        UDFCloseAllSystemDelayedInDir(Vcb, Vcb->RootDirFCB->FileInfo);
#ifdef UDF_DELAYED_CLOSE
    UDFCloseAllDelayed(Vcb);
#endif //UDF_DELAYED_CLOSE

    //  Acquire exclusive access to the Vcb.
    UDFAcquireResourceExclusive(&(Vcb->VCBResource), TRUE );
    VcbAcquired = TRUE;

    _SEH2_TRY {

        //  Mark the volume as needs to be verified, but only do it if
        //  the vcb is locked by this handle and the volume is currently mounted.

        if(Vcb->VcbCondition != VcbMounted) {

            UDFReleaseResource( &(Vcb->VCBResource) );
            VcbAcquired = FALSE;

            RC = STATUS_VOLUME_DISMOUNTED;
        } else
        if(/*!(Vcb->VCBFlags & VCB_STATE_VOLUME_MOUNTED) ||*/
           !(Vcb->VCBFlags & VCB_STATE_VOLUME_LOCKED) ||
            (Vcb->VCBOpenCount > (UDF_RESIDUAL_REFERENCE+1))) {

            RC = STATUS_NOT_LOCKED;
        } else
        if((Vcb->VolumeLockFileObject != IrpSp->FileObject)) {

            RC = STATUS_INVALID_PARAMETER;

        } else {

            Vcb->Vpb->RealDevice->Flags |= DO_VERIFY_VOLUME;
            UDFDoDismountSequence(Vcb, FALSE);

            if (Vcb->VcbCondition != VcbDismountInProgress) {
                Vcb->VcbCondition = VcbInvalid;
            }

            Vcb->WriteSecurity = FALSE;
            // disable Eject Request Waiter if any
            UDFReleaseResource( &(Vcb->VCBResource) );
            VcbAcquired = FALSE;

            //  Set flag to tell the close path that we want to force dismount
            //  the volume when this handle is closed.
            SetFlag(Ccb->CCBFlags, UDF_CCB_FLAG_DISMOUNT_ON_CLOSE);

            RC = STATUS_SUCCESS;
        }
    } _SEH2_FINALLY {
        //  Release all of our resources
        if(VcbAcquired)
            UDFReleaseResource( &(Vcb->VCBResource) );
    } _SEH2_END;

    if(!NT_SUCCESS(RC)) {
        FsRtlNotifyVolumeEvent(IrpSp->FileObject, FSRTL_VOLUME_DISMOUNT_FAILED);
    }

    //  Complete the request if there haven't been any exceptions.
    Irp->IoStatus.Information = 0;
    Irp->IoStatus.Status = RC;
    return RC;
} // end UDFDismountVolume()

/*

    This routine returns the volume allocation bitmap.

        Input = the STARTING_LCN_INPUT_BUFFER data structure is passed in
            through the input buffer.
        Output = the VOLUME_BITMAP_BUFFER data structure is returned through
            the output buffer.

    We return as much as the user buffer allows starting the specified input
    LCN (trucated to a byte).  If there is no input buffer, we start at zero.

Arguments:

    Irp - Supplies the Irp being processed.

Return Value:

    NTSTATUS - The return status for the operation.

 */
NTSTATUS
UDFGetVolumeBitmap(
    IN PIRP_CONTEXT IrpContext,
    IN PIRP Irp
    )
{
//    NTSTATUS RC;

    PEXTENDED_IO_STACK_LOCATION IrpSp =
        (PEXTENDED_IO_STACK_LOCATION)IoGetCurrentIrpStackLocation( Irp );

    UDFPrint(("UDFGetVolumeBitmap\n"));

    ULONG BytesToCopy;
    ULONG TotalClusters;
    ULONG DesiredClusters;
    ULONG StartingCluster;
    ULONG InputBufferLength;
    ULONG OutputBufferLength;
    LARGE_INTEGER StartingLcn;
    PVOLUME_BITMAP_BUFFER OutputBuffer;
    ULONG i, lim;
    PULONG FSBM;
//    PULONG Dest;
    ULONG LSh;

    // Decode the file object, the only type of opens we accept are
    // user volume opens.
    PCCB Ccb = (PCCB)IrpSp->FileObject->FsContext2;
    if(!Ccb) {
        UDFPrintErr(("  !Ccb\n"));
        Irp->IoStatus.Information = 0;
        Irp->IoStatus.Status = STATUS_INVALID_PARAMETER;
        return STATUS_INVALID_PARAMETER;
    }

    PFCB Fcb = Ccb->Fcb;
    PVCB Vcb = Fcb->Vcb;

    InputBufferLength = IrpSp->Parameters.FileSystemControl.InputBufferLength;
    OutputBufferLength = IrpSp->Parameters.FileSystemControl.OutputBufferLength;

    OutputBuffer = (PVOLUME_BITMAP_BUFFER)UDFMapUserBuffer(Irp);
    if(!OutputBuffer)
        return STATUS_INVALID_USER_BUFFER;

    // Check for a minimum length on the input and output buffers.
    if ((InputBufferLength < sizeof(STARTING_LCN_INPUT_BUFFER)) ||
        (OutputBufferLength < sizeof(VOLUME_BITMAP_BUFFER))) {

        UDFUnlockCallersBuffer(IrpContext, Irp, OutputBuffer);
        Irp->IoStatus.Information = 0;
        Irp->IoStatus.Status = STATUS_BUFFER_TOO_SMALL;
        return STATUS_BUFFER_TOO_SMALL;
    }

    //  Check if a starting cluster was specified.
    TotalClusters = Vcb->FSBM_BitCount;
    StartingLcn = ((PSTARTING_LCN_INPUT_BUFFER)IrpSp->Parameters.FileSystemControl.Type3InputBuffer)->StartingLcn;

    if (StartingLcn.HighPart || StartingLcn.LowPart >= TotalClusters) {

        UDFUnlockCallersBuffer(IrpContext, Irp, OutputBuffer);
        Irp->IoStatus.Information = 0;
        Irp->IoStatus.Status = STATUS_INVALID_PARAMETER;
        return STATUS_INVALID_PARAMETER;

    } else {

        StartingCluster = StartingLcn.LowPart & ~7;
    }

    OutputBufferLength -= FIELD_OFFSET(VOLUME_BITMAP_BUFFER, Buffer);
    DesiredClusters = TotalClusters - StartingCluster;

    if (OutputBufferLength < (DesiredClusters + 7) / 8) {

        BytesToCopy = OutputBufferLength;
//        RC = STATUS_BUFFER_OVERFLOW;

    } else {

        BytesToCopy = (DesiredClusters + 7) / 8;
//        RC = STATUS_SUCCESS;
    }

    UDFAcquireResourceExclusive(&(Vcb->VCBResource), TRUE );

    _SEH2_TRY {

        //  Fill in the fixed part of the output buffer
        OutputBuffer->StartingLcn.QuadPart = StartingCluster;
        OutputBuffer->BitmapSize.QuadPart = DesiredClusters;

        RtlZeroMemory( &OutputBuffer->Buffer[0], BytesToCopy );
        lim = BytesToCopy * 8;
        FSBM = (PULONG)(Vcb->FSBM_Bitmap);
        LSh = Vcb->LB2B_Bits;
//        Dest = (PULONG)(&OutputBuffer->Buffer[0]);

        for(i=StartingCluster & ~7; i<lim; i++) {
            if(UDFGetFreeBit(FSBM, i<<LSh))
                UDFSetFreeBit(FSBM, i);
        }

    } _SEH2_EXCEPT(UDFExceptionFilter(IrpContext, _SEH2_GetExceptionInformation())) {

        BrutePoint();
        UDFPrintErr(("UDFGetVolumeBitmap: Exception\n"));
//        UDFUnlockCallersBuffer(IrpContext, Irp, OutputBuffer);
        BrutePoint();
//        RC = UDFExceptionHandler(IrpContext, Irp);
        UDFReleaseResource(&(Vcb->VCBResource));
        UDFUnlockCallersBuffer(IrpContext, Irp, OutputBuffer);

        Irp->IoStatus.Information = 0;
        Irp->IoStatus.Status = STATUS_INVALID_USER_BUFFER;
        return STATUS_INVALID_USER_BUFFER;
    } _SEH2_END;

    UDFReleaseResource(&(Vcb->VCBResource));

    UDFUnlockCallersBuffer(IrpContext, Irp, OutputBuffer);
    Irp->IoStatus.Information = FIELD_OFFSET(VOLUME_BITMAP_BUFFER, Buffer) +
                                BytesToCopy;
    Irp->IoStatus.Status = STATUS_SUCCESS;

    return STATUS_SUCCESS;


} // end UDFGetVolumeBitmap()


NTSTATUS
UDFGetRetrievalPointers(
    IN PIRP_CONTEXT IrpContext,
    IN PIRP  Irp,
    IN ULONG Special
    )
{
    NTSTATUS RC;

    PEXTENDED_IO_STACK_LOCATION IrpSp =
        (PEXTENDED_IO_STACK_LOCATION)IoGetCurrentIrpStackLocation( Irp );
    PUDF_FILE_INFO FileInfo;

    ULONG InputBufferLength;
    ULONG OutputBufferLength;

    PRETRIEVAL_POINTERS_BUFFER OutputBuffer;
    PSTARTING_VCN_INPUT_BUFFER InputBuffer;

    LARGE_INTEGER StartingVcn;
    int64 AllocationSize;

    PEXTENT_MAP SubMapping = NULL;
    ULONG SubExtInfoSz;
    ULONG i;
    ULONG LBS;
    ULONG LBSh;
    ULONG L2BSh;

    UDFPrint(("UDFGetRetrievalPointers\n"));

    // Decode the file object, the only type of opens we accept are
    // user volume opens.
    PCCB Ccb = (PCCB)IrpSp->FileObject->FsContext2;
    if(!Ccb) {
        UDFPrintErr(("  !Ccb\n"));
        Irp->IoStatus.Information = 0;
        Irp->IoStatus.Status = STATUS_INVALID_PARAMETER;
        return STATUS_INVALID_PARAMETER;
    }

    PFCB Fcb = Ccb->Fcb;
    PVCB Vcb = Fcb->Vcb;

    //  Get the input and output buffer lengths and pointers.
    //  Initialize some variables.
    InputBufferLength = IrpSp->Parameters.FileSystemControl.InputBufferLength;
    OutputBufferLength = IrpSp->Parameters.FileSystemControl.OutputBufferLength;

    //OutputBuffer = (PRETRIEVAL_POINTERS_BUFFER)UDFGetCallersBuffer( IrpContext, Irp );
    if(Special) {
        OutputBuffer = (PRETRIEVAL_POINTERS_BUFFER)Irp->AssociatedIrp.SystemBuffer;
    } else {
        OutputBuffer = (PRETRIEVAL_POINTERS_BUFFER)Irp->UserBuffer;
    }
    InputBuffer = (PSTARTING_VCN_INPUT_BUFFER)IrpSp->Parameters.FileSystemControl.Type3InputBuffer;
    if(!InputBuffer) {
        InputBuffer = (PSTARTING_VCN_INPUT_BUFFER)OutputBuffer;
    }

    _SEH2_TRY {

        Irp->IoStatus.Information = 0;
        //  Check for a minimum length on the input and ouput buffers.
        if ((InputBufferLength < sizeof(STARTING_VCN_INPUT_BUFFER)) ||
            (OutputBufferLength < sizeof(RETRIEVAL_POINTERS_BUFFER))) {

            try_return( RC = STATUS_BUFFER_TOO_SMALL );
        }

        _SEH2_TRY {

            if (Irp->RequestorMode != KernelMode) {
                ProbeForRead( IrpSp->Parameters.FileSystemControl.Type3InputBuffer,
                              InputBufferLength,
                              sizeof(UCHAR) );
                ProbeForWrite( OutputBuffer, OutputBufferLength, sizeof(UCHAR) );
            }
            StartingVcn = InputBuffer->StartingVcn;

        } _SEH2_EXCEPT(Irp->RequestorMode != KernelMode ? EXCEPTION_EXECUTE_HANDLER: EXCEPTION_CONTINUE_SEARCH) {

            RC = _SEH2_GetExceptionCode();
            RC = FsRtlIsNtstatusExpected(RC) ?
                              RC : STATUS_INVALID_USER_BUFFER;
            try_return(RC);
        } _SEH2_END;

        switch(Special) {
        case 0:
            FileInfo = Fcb->FileInfo;
            break;
        case 1:
            FileInfo = Vcb->NonAllocFileInfo;
            break;
        default:
            try_return( RC = STATUS_INVALID_PARAMETER );
        }

        if(!FileInfo) {
            try_return( RC = STATUS_OBJECT_NAME_NOT_FOUND );
        }

        AllocationSize = UDFGetFileAllocationSize(Vcb, FileInfo);

        LBS   = Vcb->LBlockSize;
        LBSh  = Vcb->LBlockSizeBits;
        L2BSh = Vcb->LB2B_Bits;

        if (StartingVcn.HighPart ||
            StartingVcn.LowPart >= (ULONG)(AllocationSize >> LBSh)) {

            try_return( RC = STATUS_END_OF_FILE );
        }

        SubExtInfoSz = (OutputBufferLength - FIELD_OFFSET(RETRIEVAL_POINTERS_BUFFER, Extents[0])) / (sizeof(LARGE_INTEGER)*2);
        // re-use AllocationSize as NextVcn
        RC = UDFReadFileLocation__(Vcb, FileInfo, StartingVcn.QuadPart << LBSh,
                                   &SubMapping, &SubExtInfoSz, &AllocationSize);
        if(!NT_SUCCESS(RC))
            try_return(RC);

        OutputBuffer->ExtentCount = SubExtInfoSz;
        OutputBuffer->StartingVcn = StartingVcn;
        for(i=0; i<SubExtInfoSz; i++) {
            // assume, that
            // for not-allocated extents we have start Lba = -1
            // for not-recorded extents start Lba.LowPart contains real Lba, Lba.HighPart = 0x80000000
            // for recorded extents Lba.LowPart contains real Lba, Lba.HighPart = 0
            if(SubMapping[i].extLocation == LBA_NOT_ALLOCATED) {
                OutputBuffer->Extents[i].Lcn.QuadPart = (int64)(-1);
            } else
            if(SubMapping[i].extLocation & 0x80000000) {
                OutputBuffer->Extents[i].Lcn.LowPart = (SubMapping[i].extLocation & 0x7fffffff) >> L2BSh;
                OutputBuffer->Extents[i].Lcn.HighPart = 0x80000000;
            } else {
                OutputBuffer->Extents[i].Lcn.LowPart = SubMapping[i].extLocation >> L2BSh;
                OutputBuffer->Extents[i].Lcn.HighPart = 0;
            }
            // alignment for last sector
            SubMapping[i].extLength += LBS-1;
            StartingVcn.QuadPart += SubMapping[i].extLength   >> LBSh;
            OutputBuffer->Extents[i].NextVcn = StartingVcn;
        }

        Irp->IoStatus.Information = FIELD_OFFSET(RETRIEVAL_POINTERS_BUFFER, Extents[0]) + i * sizeof(LARGE_INTEGER) * 2;

try_exit:   NOTHING;
    } _SEH2_FINALLY {

        if(SubMapping)
            MyFreePool__(SubMapping);
        Irp->IoStatus.Status = RC;
    } _SEH2_END;

    return RC;
} // end UDFGetRetrievalPointers()


NTSTATUS
UDFIsVolumeDirty(
    IN PIRP_CONTEXT IrpContext,
    IN PIRP Irp
    )
{
    PULONG VolumeState;
    PEXTENDED_IO_STACK_LOCATION IrpSp =
        (PEXTENDED_IO_STACK_LOCATION)IoGetCurrentIrpStackLocation( Irp );

    UDFPrint(("UDFIsVolumeDirty\n"));

    Irp->IoStatus.Information = 0;

    if (Irp->AssociatedIrp.SystemBuffer != NULL) {
        VolumeState = (PULONG)(Irp->AssociatedIrp.SystemBuffer);
    } else if (Irp->MdlAddress != NULL) {
        VolumeState = (PULONG)MmGetSystemAddressForMdl(Irp->MdlAddress);
    } else {
        UDFPrintErr(("  STATUS_INVALID_USER_BUFFER\n"));
        Irp->IoStatus.Status = STATUS_INVALID_USER_BUFFER;
        return STATUS_INVALID_USER_BUFFER;
    }

    if (IrpSp->Parameters.FileSystemControl.OutputBufferLength < sizeof(ULONG)) {
        UDFPrintErr(("  STATUS_BUFFER_TOO_SMALL\n"));
        Irp->IoStatus.Status = STATUS_BUFFER_TOO_SMALL;
        return STATUS_BUFFER_TOO_SMALL;
    }

    (*VolumeState) = 0;

    // Decode the file object, the only type of opens we accept are
    // user volume opens.
    PCCB Ccb = (PCCB)IrpSp->FileObject->FsContext2;
    if(!Ccb) {
        UDFPrintErr(("  !Ccb\n"));
        Irp->IoStatus.Information = 0;
        Irp->IoStatus.Status = STATUS_INVALID_PARAMETER;
        return STATUS_INVALID_PARAMETER;
    }

    PFCB Fcb = Ccb->Fcb;
    PVCB Vcb = Fcb->Vcb;

    if(Vcb != (PVCB)Fcb || !(Ccb->CCBFlags & UDF_CCB_VOLUME_OPEN)) {
        UDFPrintErr(("  !Volume\n"));
        Irp->IoStatus.Status = STATUS_INVALID_PARAMETER;
        return STATUS_INVALID_PARAMETER;
    }

    if (Vcb->VcbCondition != VcbMounted) {
        UDFPrintErr(("  !Mounted\n"));
        Irp->IoStatus.Status = STATUS_VOLUME_DISMOUNTED;
        return STATUS_VOLUME_DISMOUNTED;
    }

    if(Vcb->origIntegrityType == INTEGRITY_TYPE_OPEN) {
        UDFPrint(("  Dirty\n"));
        (*VolumeState) |= VOLUME_IS_DIRTY;
    } else {
        UDFPrint(("  Clean\n"));
    }

    Irp->IoStatus.Information = sizeof(ULONG);
    Irp->IoStatus.Status = STATUS_SUCCESS;

    return STATUS_SUCCESS;

} // end UDFIsVolumeDirty()


NTSTATUS
UDFInvalidateVolumes(
    IN PIRP_CONTEXT IrpContext,
    IN PIRP Irp
    )
{
    NTSTATUS RC;
    PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);

    UDFPrint(("UDFInvalidateVolumes\n"));

    KIRQL SavedIrql;
    LUID TcbPrivilege = {SE_TCB_PRIVILEGE, 0};
    HANDLE Handle;
    PVCB Vcb;
    PLIST_ENTRY Link;
    PFILE_OBJECT FileToMarkBad;
    PDEVICE_OBJECT DeviceToMarkBad;

    Irp->IoStatus.Information = 0;

    //  Check for the correct security access.
    //  The caller must have the SeTcbPrivilege.
    if (IrpSp->MajorFunction == IRP_MJ_FILE_SYSTEM_CONTROL &&
        IrpSp->MinorFunction == IRP_MN_USER_FS_REQUEST &&
        IrpSp->Parameters.FileSystemControl.FsControlCode == FSCTL_INVALIDATE_VOLUMES &&
        !SeSinglePrivilegeCheck(TcbPrivilege, Irp->RequestorMode)) {

        Irp->IoStatus.Status = STATUS_PRIVILEGE_NOT_HELD;
        return STATUS_PRIVILEGE_NOT_HELD;
    }

    //  Try to get a pointer to the device object from the handle passed in.
#ifdef _WIN64
    if (IoIs32bitProcess(Irp)) {
        if (IrpSp->Parameters.FileSystemControl.InputBufferLength != sizeof(UINT32)) {
            UDFPrintErr(("UDFInvalidateVolumes: STATUS_INVALID_PARAMETER\n"));
            Irp->IoStatus.Status = STATUS_INVALID_PARAMETER;
            return STATUS_INVALID_PARAMETER;
        }

        Handle = (HANDLE)LongToHandle((*(UINT32*)Irp->AssociatedIrp.SystemBuffer));
    } else {
#endif
        if (IrpSp->Parameters.FileSystemControl.InputBufferLength != sizeof(HANDLE)) {
            UDFPrintErr(("UDFInvalidateVolumes: STATUS_INVALID_PARAMETER\n"));
            Irp->IoStatus.Status = STATUS_INVALID_PARAMETER;
            return STATUS_INVALID_PARAMETER;
        }

        Handle = *(HANDLE*)Irp->AssociatedIrp.SystemBuffer;
#ifdef _WIN64
    }
#endif

    RC = ObReferenceObjectByHandle( Handle,
                                    0,
                                    *IoFileObjectType,
                                    KernelMode,
                                    (PVOID*)&FileToMarkBad,
                                    NULL );

    if (!NT_SUCCESS(RC)) {
        UDFPrintErr(("UDFInvalidateVolumes: can't get handle, RC=%x\n", RC));
        Irp->IoStatus.Status = RC;
        return RC;
    }

    //  We only needed the pointer, not a reference.
    ObDereferenceObject( FileToMarkBad );

    //  Grab the DeviceObject from the FileObject.
    DeviceToMarkBad = FileToMarkBad->DeviceObject;

    // Acquire GlobalDataResource
    UDFAcquireResourceExclusive(&(UDFGlobalData.GlobalDataResource), TRUE);

    // Walk through all of the Vcb's attached to the global data.
    Link = UDFGlobalData.VCBQueue.Flink;

    while (Link != &(UDFGlobalData.VCBQueue)) {
        // Get 'next' Vcb
        Vcb = CONTAINING_RECORD( Link, VCB, NextVCB );
        // Move to the next link now since the current Vcb may be deleted.
        Link = Link->Flink;

        // Acquire Vcb resource
        UDFAcquireResourceExclusive(&(Vcb->VCBResource), TRUE);

        if (Vcb->Vpb->RealDevice == DeviceToMarkBad) {

            // Take the VPB spinlock,  and look to see if this volume is the 
            // one currently mounted on the actual device.  If it is,  pull it 
            // off immediately.
            IoAcquireVpbSpinLock(&SavedIrql);

            if (DeviceToMarkBad->Vpb == Vcb->Vpb) {

                PVPB NewVpb = Vcb->SwapVpb;

                ASSERT(FlagOn(Vcb->Vpb->Flags, VPB_MOUNTED));
                ASSERT(NewVpb);

                RtlZeroMemory(NewVpb, sizeof(VPB));

                NewVpb->Type = IO_TYPE_VPB;
                NewVpb->Size = sizeof(VPB);
                NewVpb->RealDevice = DeviceToMarkBad;
                NewVpb->Flags = FlagOn(DeviceToMarkBad->Vpb->Flags, VPB_REMOVE_PENDING);

                DeviceToMarkBad->Vpb = NewVpb;
                Vcb->SwapVpb = NULL;
            }

            IoReleaseVpbSpinLock(SavedIrql);

            if (Vcb->VcbCondition != VcbDismountInProgress) {

                Vcb->VcbCondition = VcbInvalid;
            }

#ifdef UDF_DELAYED_CLOSE
            UDFPrint(("    UDFInvalidateVolumes:     set VCB_STATE_NO_DELAYED_CLOSE\n"));
            Vcb->VCBFlags |= VCB_STATE_NO_DELAYED_CLOSE;
            UDFReleaseResource(&(Vcb->VCBResource));
#endif //UDF_DELAYED_CLOSE

            if(Vcb->RootDirFCB && Vcb->RootDirFCB->FileInfo) {
                UDFPrint(("    UDFInvalidateVolumes:     UDFCloseAllSystemDelayedInDir\n"));
                RC = UDFCloseAllSystemDelayedInDir(Vcb, Vcb->RootDirFCB->FileInfo);
                ASSERT(OS_SUCCESS(RC));
            }
#ifdef UDF_DELAYED_CLOSE
            UDFPrint(("    UDFInvalidateVolumes:     UDFCloseAllDelayed\n"));
            UDFCloseAllDelayed(Vcb);
            //ASSERT(OS_SUCCESS(RC));
#endif //UDF_DELAYED_CLOSE

            UDFAcquireResourceExclusive(&(Vcb->VCBResource), TRUE);

            UDFDoDismountSequence(Vcb, FALSE);
            UDFReleaseResource(&(Vcb->VCBResource));

            UDFPrint(("UDFInvalidateVolumes: Vcb %x dismounted\n", Vcb));
            break;
        } else {
            UDFPrint(("UDFInvalidateVolumes: skip Vcb %x\n", Vcb));
            UDFReleaseResource(&(Vcb->VCBResource));
        }

    }
    // Once we have processed all the mounted logical volumes, we can release
    // all acquired global resources and leave (in peace :-)
    UDFReleaseResource( &(UDFGlobalData.GlobalDataResource) );

    Irp->IoStatus.Status = STATUS_SUCCESS;

    // drop volume completly
    UDFPrint(("UDFInvalidateVolumes: drop volume completly\n"));
    UDFAcquireResourceExclusive(&(UDFGlobalData.GlobalDataResource), TRUE);
    UDFScanForDismountedVcb(IrpContext);
    UDFReleaseResource( &(UDFGlobalData.GlobalDataResource) );

    UDFPrint(("UDFInvalidateVolumes: done\n"));
    return STATUS_SUCCESS;

} // end UDFInvalidateVolumes()
