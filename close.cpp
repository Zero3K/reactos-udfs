////////////////////////////////////////////////////////////////////
// Copyright (C) Alexander Telyatnikov, Ivan Keliukh, Yegor Anchishkin, SKIF Software, 1999-2013. Kiev, Ukraine
// All rights reserved
// This file was released under the GPLv2 on June 2015.
////////////////////////////////////////////////////////////////////
/*************************************************************************
*
* File: Close.cpp
*
* Module: UDF File System Driver (Kernel mode execution only)
*
* Description:
*   Contains code to handle the "Close" dispatch entry point.
*
*************************************************************************/

#include            "udffs.h"

// define the file specific bug-check id
#define         UDF_BUG_CHECK_ID                UDF_FILE_CLOSE

typedef BOOLEAN      (*PCHECK_TREE_ITEM) (IN PUDF_FILE_INFO   FileInfo);
#define TREE_ITEM_LIST_GRAN 32

NTSTATUS
UDFBuildTreeItemsList(
    IN PVCB               Vcb,
    IN PUDF_FILE_INFO     FileInfo,
    IN PCHECK_TREE_ITEM   CheckItemProc,
    IN PUDF_DATALOC_INFO** PassedList,
    IN PULONG             PassedListSize,
    IN PUDF_DATALOC_INFO** FoundList,
    IN PULONG             FoundListSize);

// callbacks, can't be __fastcall
BOOLEAN
UDFIsInDelayedCloseQueue(
    PUDF_FILE_INFO FileInfo);

BOOLEAN
UDFIsLastClose(
    PUDF_FILE_INFO FileInfo);

/*************************************************************************
*
* Function: UDFClose()
*
* Description:
*   The I/O Manager will invoke this routine to handle a close
*   request
*
* Expected Interrupt Level (for execution) :
*
*  IRQL_PASSIVE_LEVEL (invocation at higher IRQL will cause execution
*   to be deferred to a worker thread context)
*
* Return Value: STATUS_SUCCESS
*
*************************************************************************/
NTSTATUS
NTAPI
UDFClose(
    PDEVICE_OBJECT  DeviceObject,  // the logical volume device object
    PIRP            Irp            // I/O Request Packet
    )
{
    NTSTATUS            RC = STATUS_SUCCESS;
    PIRP_CONTEXT IrpContext = NULL;
    BOOLEAN             AreWeTopLevel = FALSE;

    AdPrint(("UDFClose: \n"));

    FsRtlEnterFileSystem();
    ASSERT(DeviceObject);
    ASSERT(Irp);

    //  If we were called with our file system device object instead of a
    //  volume device object, just complete this request with STATUS_SUCCESS
        if (UDFIsFSDevObj(DeviceObject)) {
        // this is a close of the FSD itself
        Irp->IoStatus.Status = RC;
        Irp->IoStatus.Information = 0;

        // IrpContext is always NULL here, do not reference it!
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        FsRtlExitFileSystem();
        return(RC);
    }

    // set the top level context
    AreWeTopLevel = UDFIsIrpTopLevel(Irp);

    _SEH2_TRY {

        // get an IRP context structure and issue the request
        IrpContext = UDFCreateIrpContext(Irp, DeviceObject);
        ASSERT(IrpContext);

        RC = UDFCommonClose(IrpContext, Irp, FALSE);

    } _SEH2_EXCEPT(UDFExceptionFilter(IrpContext, _SEH2_GetExceptionInformation())) {

        RC = UDFProcessException(IrpContext, Irp);

        UDFLogEvent(UDF_ERROR_INTERNAL_ERROR, RC);
    } _SEH2_END;

    if (AreWeTopLevel) {
        IoSetTopLevelIrp(NULL);
    }

    FsRtlExitFileSystem();

    return(RC);
}




/*************************************************************************
*
* Function: UDFCommonClose()
*
* Description:
*   The actual work is performed here. This routine may be invoked in one'
*   of the two possible contexts:
*   (a) in the context of a system worker thread
*   (b) in the context of the original caller
*
* Expected Interrupt Level (for execution) :
*
*  IRQL_PASSIVE_LEVEL
*
* Return Value: must be STATUS_SUCCESS
*
*************************************************************************/
NTSTATUS
UDFCommonClose(
    PIRP_CONTEXT IrpContext,
    PIRP             Irp,
    BOOLEAN          CanWait
    )
{
    NTSTATUS                RC = STATUS_SUCCESS;
    PIO_STACK_LOCATION      IrpSp = NULL;
    PFILE_OBJECT            FileObject = NULL;
    PFCB                    Fcb = NULL;
    PCCB                    Ccb = NULL;
    PVCB                    Vcb = NULL;
//    PERESOURCE              PtrResourceAcquired = NULL;
    BOOLEAN                 AcquiredVcb = FALSE;
    BOOLEAN                 AcquiredGD = FALSE;
    PUDF_FILE_INFO          fi;
    ULONG                   i = 0;
//    ULONG                   clean_stat = 0;

//    BOOLEAN                 CompleteIrp = TRUE;
    BOOLEAN                 PostRequest = FALSE;

#ifdef UDF_DBG
    UNICODE_STRING          CurName;
    PDIR_INDEX_HDR          DirNdx;
#endif

    AdPrint(("UDFCommonClose: \n"));

    _SEH2_TRY {
        if (Irp) {

            // If this is the first (IOManager) request
            // First, get a pointer to the current I/O stack location
            IrpSp = IoGetCurrentIrpStackLocation(Irp);
            ASSERT(IrpSp);

            FileObject = IrpSp->FileObject;
            ASSERT(FileObject);

            //  No work to do for unopened file objects.
            if (!FileObject->FsContext) {
                try_return(RC = STATUS_SUCCESS);
            }

            // Get the FCB and CCB pointers
            Ccb = (PCCB)FileObject->FsContext2;
            ASSERT(Ccb);
            Fcb = Ccb->Fcb;
        } else {
            // If this is a queued call (for our dispatch)
            // Get saved Fcb address
            Fcb = IrpContext->Fcb;
            i = IrpContext->TreeLength;
        }

        ASSERT(Fcb);
        Vcb = (PVCB)(IrpContext->TargetDeviceObject->DeviceExtension);
        ASSERT(Vcb);
        ASSERT(Vcb->NodeIdentifier.NodeTypeCode == UDF_NODE_TYPE_VCB);
//        Vcb->VCBFlags |= UDF_VCB_SKIP_EJECT_CHECK;

        // Steps we shall take at this point are:
        // (a) Acquire the VCB shared
        // (b) Acquire the FCB's CCB list exclusively
        // (c) Delete the CCB structure (free memory)
        // (d) If this is the last close, release the FCB structure
        //       (unless we keep these around for "delayed close" functionality.
        // Note that it is often the case that the close dispatch entry point is invoked
        // in the most inconvenient of situations (when it is not possible, for example,
        // to safely acquire certain required resources without deadlocking or waiting).
        // Therefore, be extremely careful in implementing this close dispatch entry point.
        // Also note that we do not have the option of returning a failure code from the
        // close dispatch entry point; the system expects that the close will always succeed.

        UDFAcquireResourceShared(&(Vcb->VCBResource), TRUE);
        AcquiredVcb = TRUE;

        // Is this is the first (IOManager) request ?
        if (Irp) {
            IrpContext->TreeLength =
            i = Ccb->TreeLength;
            // remember the number of incomplete Close requests
            InterlockedIncrement((PLONG)&(Fcb->CcbCount));
            // we can release CCB in any case
            UDFCleanUpCCB(Ccb);
            FileObject->FsContext2 = NULL;
#ifdef DBG
/*        } else {
            ASSERT(Fcb->NTRequiredFCB);
            if(Fcb->NTRequiredFCB) {
                ASSERT(Fcb->NTRequiredFCB->FileObject);
                if(Fcb->NTRequiredFCB->FileObject) {
                    ASSERT(!Fcb->NTRequiredFCB->FileObject->FsContext2);
                }
            }*/
#endif //DBG
        }

#ifdef UDF_DELAYED_CLOSE
        // check if this is the last Close (no more Handles)
        // and try to Delay it....
        if((Fcb->FCBFlags & UDF_FCB_DELAY_CLOSE) &&
           Vcb->VcbCondition == VcbMounted &&
          !(Vcb->VCBFlags & VCB_STATE_NO_DELAYED_CLOSE) &&
          !(Fcb->OpenHandleCount)) {
            UDFReleaseResource(&(Vcb->VCBResource));
            AcquiredVcb = FALSE;
            if((RC = UDFQueueDelayedClose(IrpContext,Fcb)) == STATUS_SUCCESS)
                try_return(RC = STATUS_SUCCESS);
            // do standard Close if we can't Delay this opeartion
            AdPrint(("   Cant queue Close Irp, status=%x\n", RC));
        }
#endif //UDF_DELAYED_CLOSE

        if(Irp) {
            // We should post actual procesing if the caller does not want to block
            if (!CanWait) {
                AdPrint(("   post Close Irp\n"));
                PostRequest = TRUE;
                try_return(RC = STATUS_SUCCESS);
            }
        }

        // Close request is near completion, Vcb is acquired.
        // Now we can safely decrease CcbCount, because no Rename
        // operation can run until Vcb release.
        InterlockedDecrement((PLONG)&(Fcb->CcbCount));

        UDFInterlockedDecrement((PLONG)&(Vcb->VCBOpenCount));

        if (Ccb && FlagOn(Ccb->CCBFlags, UDF_CCB_READ_ONLY)) {

            UDFInterlockedDecrement((PLONG)&(Vcb->VCBOpenCountRO));
        }

        if(!i || (Fcb->NodeIdentifier.NodeTypeCode == UDF_NODE_TYPE_VCB)) {

            AdPrint(("UDF: Closing volume\n"));
            AdPrint(("UDF: ReferenceCount:  %x\n",Fcb->ReferenceCount));

            if (Vcb->VCBOpenCount > UDF_RESIDUAL_REFERENCE) {
                ASSERT(Fcb->NodeIdentifier.NodeTypeCode == UDF_NODE_TYPE_VCB);
                UDFInterlockedDecrement((PLONG)&Fcb->ReferenceCount);
                ASSERT(Fcb);
                UDFInterlockedDecrement((PLONG)&Fcb->CommonRefCount);

                try_return(RC = STATUS_SUCCESS);
            }

            UDFInterlockedIncrement((PLONG)&Vcb->VCBOpenCount);

            if(AcquiredVcb) {
                UDFReleaseResource(&(Vcb->VCBResource));
                AcquiredVcb = FALSE;
            } else {
                BrutePoint();
            }
            // Acquire GlobalDataResource
            UDFAcquireResourceExclusive(&UDFGlobalData.GlobalDataResource, TRUE);
            AcquiredGD = TRUE;
//            // Acquire Vcb
            UDFAcquireResourceExclusive(&Vcb->VCBResource, TRUE);
            AcquiredVcb = TRUE;

            UDFInterlockedDecrement((PLONG)&Vcb->VCBOpenCount);


            ASSERT(Fcb->NodeIdentifier.NodeTypeCode == UDF_NODE_TYPE_VCB);
            UDFInterlockedDecrement((PLONG)&Fcb->ReferenceCount);
            ASSERT(Fcb);
            UDFInterlockedDecrement((PLONG)&Fcb->CommonRefCount);

            //AdPrint(("UDF: Closing volume, reset driver (e.g. stop BGF)\n"));
            //UDFResetDeviceDriver(Vcb, Vcb->TargetDeviceObject, FALSE);

            if(Vcb->VcbCondition == VcbDismountInProgress ||
               Vcb->VcbCondition == VcbInvalid ||
             ((Vcb->VcbCondition == VcbNotMounted) && (Vcb->VCBOpenCount <= UDF_RESIDUAL_REFERENCE))) {
                // Try to KILL dismounted volume....
                // w2k requires this, NT4 - recomends
                AcquiredVcb = UDFCheckForDismount(IrpContext, Vcb, TRUE);
            }

            try_return(RC = STATUS_SUCCESS);
        }

        fi = Fcb->FileInfo;
#ifdef UDF_DBG
        if(!fi) {
            BrutePoint();
        }

        DirNdx = UDFGetDirIndexByFileInfo(fi);
        if(DirNdx) {
            CurName = UDFDirIndex(DirNdx,fi->Index)->FName;
            if(CurName.Length) {
                AdPrint(("Closing file: %wZ %8.8x\n", &CurName, FileObject));
            } else {
                AdPrint(("Closing file: ??? \n"));
            }
        }
        AdPrint(("UDF: ReferenceCount:  %x\n",Fcb->ReferenceCount));
#endif // UDF_DBG
        // try to clean up as long chain as it is possible
        UDFCleanUpFcbChain(Vcb, fi, i, TRUE);

try_exit: NOTHING;

    } _SEH2_FINALLY {

        if(AcquiredVcb) {
            UDFReleaseResource(&(Vcb->VCBResource));
        }
        if(AcquiredGD) {
            UDFReleaseResource(&(UDFGlobalData.GlobalDataResource));
        }

        // Post IRP if required
        if (PostRequest) {

            // Perform the post operation & complete the IRP
            // if this is first call of UDFCommonClose
            // and will return STATUS_SUCCESS back to us
            IrpContext->Irp = NULL;
            IrpContext->Fcb = Fcb;
            UDFPostRequest(IrpContext, NULL);
        }

        if (!_SEH2_AbnormalTermination()) {
            // If this is not async close complete the IRP
            if (Irp) {
/*                if( FileObject ) {
                    if(clean_stat & UDF_CLOSE_NTREQFCB_DELETED) {
//                        ASSERT(!FileObject->FsContext2);
                        FileObject->FsContext = NULL;
#ifdef DBG
                    } else {
                        UDFNTRequiredFCB*  NtReqFcb = ((UDFNTRequiredFCB*)(FileObject->FsContext));
                        if(NtReqFcb->FileObject == FileObject) {
                            NtReqFcb->FileObject = NULL;
                        }
#endif //DBG
                    }
                }*/
				if (!IrpContext->IrpCompleted) {
                Irp->IoStatus.Status = STATUS_SUCCESS;
                Irp->IoStatus.Information = 0;				
				IrpContext->IrpCompleted = TRUE;
                IoCompleteRequest(Irp, IO_DISK_INCREMENT);}
            }
		}

    } _SEH2_END; // end of "__finally" processing

    return STATUS_SUCCESS ;
} // end UDFCommonClose()

/*
    This routine walks through the tree to RootDir & kills all unreferenced
    structures....
    imho, Useful feature
 */
ULONG
UDFCleanUpFcbChain(
    IN PVCB Vcb,
    IN PUDF_FILE_INFO fi,
    IN ULONG TreeLength,
    IN BOOLEAN VcbAcquired
    )
{
    ULONG ret_val = 0;
    BOOLEAN Delete = FALSE;

    ValidateFileInfo(fi);
    AdPrint(("UDFCleanUpFcbChain\n"));

    ASSERT(TreeLength);

    // we can't process Tree until we can acquire Vcb
    if(!VcbAcquired)
        UDFAcquireResourceShared(&(Vcb->VCBResource), TRUE);

    while (fi) {
        BOOLEAN AcquiredParent = FALSE;
        PUDF_FILE_INFO NextFI = NULL;
        PFCB ThisFcb = fi->Fcb;
        PFCB ThisParentFcb = NULL;
        PUDF_FILE_INFO ThisParentFI = fi->ParentFile;
        LONG RefCount = 0, ComRefCount = 0;

        if (ThisParentFI) {
            ThisParentFcb = ThisFcb->ParentFcb;
            UDF_CHECK_PAGING_IO_RESOURCE(ThisParentFcb);
            UDFAcquireResourceExclusive(&ThisParentFcb->MainResource, TRUE);
            AcquiredParent = TRUE;
        } else if (!VcbAcquired) {
            UDFAcquireResourceShared(&Vcb->VCBResource, TRUE);
        }

        UDF_CHECK_PAGING_IO_RESOURCE(ThisFcb);
        UDFAcquireResourceExclusive(&ThisFcb->MainResource, TRUE);

        // Only decrement counts if TreeLength > 0
        if (TreeLength) {
            ASSERT(ThisFcb->ReferenceCount);
            ASSERT(ThisFcb->CommonRefCount);
            RefCount = UDFInterlockedDecrement((PLONG)&ThisFcb->ReferenceCount);
            ComRefCount = UDFInterlockedDecrement((PLONG)&ThisFcb->CommonRefCount);
            TreeLength--;
        } else {
            RefCount = ThisFcb->ReferenceCount;
            ComRefCount = ThisFcb->CommonRefCount;
        }

        if (!RefCount && !ThisFcb->OpenHandleCount) {
            if (Vcb->VCBFlags & VCB_STATE_RAW_DISK) {
                // do nothing
            } else if (Delete) {
                UDFReferenceFile__(fi);
                ASSERT(ThisFcb->ReferenceCount < fi->RefCount);
                UDFFlushFile__(Vcb, fi);
                UDFUnlinkFile__(Vcb, fi, TRUE);
                UDFCloseFile__(Vcb, fi);
                ASSERT(ThisFcb->ReferenceCount == fi->RefCount);
                ThisFcb->FCBFlags |= UDF_FCB_DELETED;
                Delete = FALSE;
            } else if (!(ThisFcb->FCBFlags & UDF_FCB_DELETED)) {
                UDFFlushFile__(Vcb, fi);
            }

            if (ThisFcb->FCBFlags & UDF_FCB_DELETE_PARENT)
                Delete = TRUE;

            fi->Fcb = NULL;
            if (!ComRefCount) {
                fi->Dloc->CommonFcb = NULL;
            }

            if (UDFCleanUpFile__(Vcb, fi) == (UDF_FREE_FILEINFO | UDF_FREE_DLOC)) {
                ThisFcb->FileInfo = NULL;
                if (ThisFcb->FileLock != NULL) {
                    FsRtlFreeFileLock(ThisFcb->FileLock);
                }
                FsRtlTeardownPerStreamContexts(&ThisFcb->Header);
                UDF_CHECK_PAGING_IO_RESOURCE(ThisFcb);
                UDFReleaseResource(&ThisFcb->MainResource);
                if (ThisFcb->Header.Resource) {
                    UDFDeleteResource(&ThisFcb->MainResource);
                    UDFDeleteResource(&ThisFcb->PagingIoResource);
                }
                ThisFcb->Header.Resource = ThisFcb->Header.PagingIoResource = NULL;
                UDFPrint(("UDFRelease Fcb: %x\n", ThisFcb));
                ret_val |= UDF_CLOSE_NTREQFCB_DELETED;

                ThisFcb->ParentFcb = NULL;
                UDFCleanUpFCB(ThisFcb);

                NextFI = ThisParentFI;
                PFCB OldParentFcb = ThisParentFcb;
                MyFreePool__(fi);
                ret_val |= UDF_CLOSE_FCB_DELETED;
                fi = NULL;
                // Release in the correct order
                if (AcquiredParent && OldParentFcb) {
                    UDF_CHECK_PAGING_IO_RESOURCE(OldParentFcb);
                    UDFReleaseResource(&OldParentFcb->MainResource);
                } else if (!AcquiredParent && !NextFI && !VcbAcquired) {
                    UDFReleaseResource(&Vcb->VCBResource);
                }
                fi = NextFI;
            } else {
                // Restore pointers
                fi->Fcb = ThisFcb;
                fi->Dloc->CommonFcb = ThisFcb;
                UDF_CHECK_PAGING_IO_RESOURCE(ThisFcb);
                UDFReleaseResource(&ThisFcb->MainResource);
                if (AcquiredParent && ThisParentFcb) {
                    UDF_CHECK_PAGING_IO_RESOURCE(ThisParentFcb);
                    UDFReleaseResource(&ThisParentFcb->MainResource);
                } else if (!AcquiredParent && !ThisParentFI && !VcbAcquired) {
                    UDFReleaseResource(&Vcb->VCBResource);
                }
                if (!TreeLength)
                    break;
                fi = ThisParentFI;
            }
        } else {
            // Just release and walk up
            UDF_CHECK_PAGING_IO_RESOURCE(ThisFcb);
            UDFReleaseResource(&ThisFcb->MainResource);
            if (AcquiredParent && ThisParentFcb) {
                UDF_CHECK_PAGING_IO_RESOURCE(ThisParentFcb);
                UDFReleaseResource(&ThisParentFcb->MainResource);
            } else if (!AcquiredParent && !ThisParentFI && !VcbAcquired) {
                UDFReleaseResource(&Vcb->VCBResource);
            }
            Delete = FALSE;
            if (!TreeLength)
                break;
            fi = ThisParentFI;
        }
    }

    if (!VcbAcquired)
        UDFReleaseResource(&Vcb->VCBResource);
    return ret_val;
} // end UDFCleanUpFcbChain()

VOID
UDFDoDelayedClose(
    IN PIRP_CONTEXT_LITE NextIrpContextLite
    )
{
    IRP_CONTEXT StackIrpContext;

    AdPrint(("  UDFDoDelayedClose\n"));
    UDFInitializeStackIrpContextFromLite(&StackIrpContext, NextIrpContextLite);
    MyFreePool__(NextIrpContextLite);
    StackIrpContext.Fcb->IrpContextLite = NULL;
    StackIrpContext.Fcb->FCBFlags &= ~UDF_FCB_DELAY_CLOSE;
    UDFCommonClose(&StackIrpContext, NULL, TRUE);
} // end UDFDoDelayedClose()

/*
    This routine removes request from Delayed Close queue.
    It operates until reach lower threshold
 */
VOID
NTAPI
UDFDelayedClose(
    PVOID unused
    )
{
    PLIST_ENTRY             Entry;
    PIRP_CONTEXT_LITE NextIrpContextLite;

    AdPrint(("  UDFDelayedClose\n"));
    // Acquire DelayedCloseResource
    UDFAcquireResourceExclusive(&(UDFGlobalData.DelayedCloseResource), TRUE);

    while (UDFGlobalData.ReduceDelayedClose &&
          (UDFGlobalData.DelayedCloseCount > UDFGlobalData.MinDelayedCloseCount)) {

        Entry = UDFGlobalData.DelayedCloseQueue.Flink;

        if (!IsListEmpty(Entry)) {
            //  Extract the IrpContext.
            NextIrpContextLite = CONTAINING_RECORD( Entry,
                                                    IRP_CONTEXT_LITE,
                                                    DelayedCloseLinks );

            RemoveEntryList( Entry );
            UDFGlobalData.DelayedCloseCount--;
            UDFDoDelayedClose(NextIrpContextLite);
        } else {
            BrutePoint();
        }
    }

    while (UDFGlobalData.ReduceDirDelayedClose &&
          (UDFGlobalData.DirDelayedCloseCount > UDFGlobalData.MinDirDelayedCloseCount)) {

        Entry = UDFGlobalData.DirDelayedCloseQueue.Flink;

        if (!IsListEmpty(Entry)) {
            //  Extract the IrpContext.
            NextIrpContextLite = CONTAINING_RECORD( Entry,
                                                    IRP_CONTEXT_LITE,
                                                    DelayedCloseLinks );

            RemoveEntryList( Entry );
            UDFGlobalData.DirDelayedCloseCount--;
            UDFDoDelayedClose(NextIrpContextLite);
        } else {
            BrutePoint();
        }
    }

    UDFGlobalData.FspCloseActive = FALSE;
    UDFGlobalData.ReduceDelayedClose = FALSE;
    UDFGlobalData.ReduceDirDelayedClose = FALSE;

    // Release DelayedCloseResource
    UDFReleaseResource(&(UDFGlobalData.DelayedCloseResource));

    return;
} // end UDFDelayedClose()

/*
    This routine performs Close operation for all files from
    Delayed Close queue.
 */
VOID
UDFCloseAllDelayed(
    IN PVCB Vcb
    )
{
    PLIST_ENTRY             Entry;
    PIRP_CONTEXT_LITE NextIrpContextLite;
    BOOLEAN                 GlobalDataAcquired = FALSE;

    AdPrint(("  UDFCloseAllDelayed\n"));
    // Acquire DelayedCloseResource
    if (!ExIsResourceAcquiredExclusive(&UDFGlobalData.GlobalDataResource)) {
        UDFAcquireResourceExclusive(&(UDFGlobalData.DelayedCloseResource), TRUE);
        GlobalDataAcquired = TRUE;
    }

    Entry = UDFGlobalData.DelayedCloseQueue.Flink;

    while (Entry != &UDFGlobalData.DelayedCloseQueue) {
        //  Extract the IrpContext.
        NextIrpContextLite = CONTAINING_RECORD( Entry,
                                                IRP_CONTEXT_LITE,
                                                DelayedCloseLinks );
        Entry = Entry->Flink;
        if (NextIrpContextLite->Fcb->Vcb == Vcb) {
            RemoveEntryList( &(NextIrpContextLite->DelayedCloseLinks) );
            UDFGlobalData.DelayedCloseCount--;
            UDFDoDelayedClose(NextIrpContextLite);
        }
    }

    Entry = UDFGlobalData.DirDelayedCloseQueue.Flink;

    while (Entry != &UDFGlobalData.DirDelayedCloseQueue) {
        //  Extract the IrpContext.
        NextIrpContextLite = CONTAINING_RECORD(Entry,
                                               IRP_CONTEXT_LITE,
                                               DelayedCloseLinks);
        Entry = Entry->Flink;
        if (NextIrpContextLite->Fcb->Vcb == Vcb) {
            RemoveEntryList( &(NextIrpContextLite->DelayedCloseLinks) );
            UDFGlobalData.DirDelayedCloseCount--;
            UDFDoDelayedClose(NextIrpContextLite);
        }
    }

    // Release DelayedCloseResource
    if(GlobalDataAcquired)
        UDFReleaseResource(&(UDFGlobalData.DelayedCloseResource));

} // end UDFCloseAllDelayed()

NTSTATUS
UDFBuildTreeItemsList(
    IN PVCB               Vcb,
    IN PUDF_FILE_INFO     FileInfo,
    IN PCHECK_TREE_ITEM   CheckItemProc,
    IN PUDF_FILE_INFO**   PassedList,
    IN PULONG             PassedListSize,
    IN PUDF_FILE_INFO**   FoundList,
    IN PULONG             FoundListSize
    )
{
    PDIR_INDEX_HDR     hDirNdx;
    PUDF_FILE_INFO     SDirInfo;
    ULONG              i;

    UDFPrint(("    UDFBuildTreeItemsList():\n"));
    if(!(*PassedList) || !(*FoundList)) {

        (*PassedList) = (PUDF_FILE_INFO*)
            MyAllocatePool__(NonPagedPool, sizeof(PUDF_FILE_INFO)*TREE_ITEM_LIST_GRAN);
        if(!(*PassedList))
            return STATUS_INSUFFICIENT_RESOURCES;
        (*PassedListSize) = 0;

        (*FoundList) = (PUDF_FILE_INFO*)
            MyAllocatePool__(NonPagedPool, sizeof(PUDF_FILE_INFO)*TREE_ITEM_LIST_GRAN);
        if(!(*FoundList)) {
            MyFreePool__(*PassedList);
            *PassedList = NULL;
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        (*FoundListSize) = 0;
    }

    // check if already passed
    for(i=0;i<(*PassedListSize);i++) {
        if( ((*PassedList)[i]) == FileInfo )
            return STATUS_SUCCESS;
    }
    // remember passed object
    // we should not proceed linked objects twice
    (*PassedListSize)++;
    if( !((*PassedListSize) & (TREE_ITEM_LIST_GRAN - 1)) ) {
        if(!MyReallocPool__((PCHAR)(*PassedList), (*PassedListSize)*sizeof(PUDF_FILE_INFO),
                         (PCHAR*)PassedList, ((*PassedListSize)+TREE_ITEM_LIST_GRAN)*sizeof(PUDF_FILE_INFO))) {
            return STATUS_INSUFFICIENT_RESOURCES;
        }
    }
    (*PassedList)[(*PassedListSize)-1] = FileInfo;

    // check if this object matches our conditions
    if(CheckItemProc(FileInfo)) {
        // remember matched object
        (*FoundListSize)++;
        if( !((*FoundListSize) & (TREE_ITEM_LIST_GRAN - 1)) ) {
            if(!MyReallocPool__((PCHAR)(*FoundList), (*FoundListSize)*sizeof(PUDF_DATALOC_INFO),
                             (PCHAR*)FoundList, ((*FoundListSize)+TREE_ITEM_LIST_GRAN)*sizeof(PUDF_DATALOC_INFO))) {
                return STATUS_INSUFFICIENT_RESOURCES;
            }
        }
        (*FoundList)[(*FoundListSize)-1] = FileInfo;
    }

    // walk through SDir (if any)
    if((SDirInfo = FileInfo->Dloc->SDirInfo))
        UDFBuildTreeItemsList(Vcb, SDirInfo, CheckItemProc,
                 PassedList, PassedListSize, FoundList, FoundListSize);

    // walk through subsequent objects (if any)
    if((hDirNdx = FileInfo->Dloc->DirIndex)) {

        // scan DirIndex
        UDF_DIR_SCAN_CONTEXT ScanContext;
        PDIR_INDEX_ITEM DirNdx;
        PUDF_FILE_INFO CurFileInfo;

        if(UDFDirIndexInitScan(FileInfo, &ScanContext, 2)) {
            while((DirNdx = UDFDirIndexScan(&ScanContext, &CurFileInfo))) {
                if(!CurFileInfo)
                    continue;
                UDFBuildTreeItemsList(Vcb, CurFileInfo, CheckItemProc,
                         PassedList, PassedListSize, FoundList, FoundListSize);
            }
        }

    }
    return STATUS_SUCCESS;
} // end UDFBuildTreeItemsList()

BOOLEAN
UDFIsInDelayedCloseQueue(
    PUDF_FILE_INFO FileInfo)
{
    ASSERT(FileInfo);
    return (FileInfo->Fcb && FileInfo->Fcb->IrpContextLite);
} // end UDFIsInDelayedCloseQueue()

BOOLEAN
UDFIsLastClose(
    PUDF_FILE_INFO FileInfo)
{
    ASSERT(FileInfo);
    PFCB Fcb = FileInfo->Fcb;
    if( Fcb &&
       !Fcb->OpenHandleCount &&
        Fcb->ReferenceCount &&
        Fcb->SectionObject.DataSectionObject) {
        return TRUE;
    }
    return FALSE;
} // UDFIsLastClose()

NTSTATUS
UDFCloseAllXXXDelayedInDir(
    IN PVCB             Vcb,
    IN PUDF_FILE_INFO   FileInfo,
    IN BOOLEAN          System
    )
{
    PUDF_FILE_INFO*         PassedList = NULL;
    ULONG                   PassedListSize = 0;
    PUDF_FILE_INFO*         FoundList = NULL;
    ULONG                   FoundListSize = 0;
    NTSTATUS                RC;
    ULONG                   i;
    _SEH2_VOLATILE BOOLEAN  ResAcq = FALSE;
    _SEH2_VOLATILE BOOLEAN  AcquiredVcb = FALSE;
    PUDF_FILE_INFO          CurFileInfo;
    PFE_LIST_ENTRY          CurListPtr;
    PFE_LIST_ENTRY*         ListPtrArray = NULL;

    _SEH2_TRY {

        UDFPrint(("    UDFCloseAllXXXDelayedInDir(): Acquire DelayedCloseResource\n"));
        // Acquire DelayedCloseResource
        UDFAcquireResourceExclusive(&(UDFGlobalData.DelayedCloseResource), TRUE);
        ResAcq = TRUE;

        UDFAcquireResourceExclusive(&(Vcb->VCBResource), TRUE);
        AcquiredVcb = TRUE;

        RC = UDFBuildTreeItemsList(Vcb, FileInfo,
                System ? UDFIsLastClose : UDFIsInDelayedCloseQueue,
                &PassedList, &PassedListSize, &FoundList, &FoundListSize);

        if(!NT_SUCCESS(RC)) {
            UDFPrint(("    UDFBuildTreeItemsList(): error %x\n", RC));
            try_return(RC);
        }

        if(!FoundList || !FoundListSize) {
            try_return(RC = STATUS_SUCCESS);
        }

        // build array of referenced pointers
        ListPtrArray = (PFE_LIST_ENTRY*)(MyAllocatePool__(NonPagedPool, FoundListSize*sizeof(PFE_LIST_ENTRY)));
        if(!ListPtrArray) {
            UDFPrint(("    Can't alloc ListPtrArray for %x items\n", FoundListSize));
            try_return(RC = STATUS_INSUFFICIENT_RESOURCES);
        }

        for(i=0;i<FoundListSize;i++) {

            _SEH2_TRY {

                CurFileInfo = FoundList[i];
                if(!CurFileInfo->ListPtr) {
                    CurFileInfo->ListPtr = (PFE_LIST_ENTRY)(MyAllocatePool__(NonPagedPool, sizeof(FE_LIST_ENTRY)));
                    if(!CurFileInfo->ListPtr) {
                        UDFPrint(("    Can't alloc ListPtrEntry for items %x\n", i));
                        try_return(RC = STATUS_INSUFFICIENT_RESOURCES);
                    }
                    CurFileInfo->ListPtr->FileInfo = CurFileInfo;
                    CurFileInfo->ListPtr->EntryRefCount = 0;
                }
                CurFileInfo->ListPtr->EntryRefCount++;
                ListPtrArray[i] = CurFileInfo->ListPtr;

            } _SEH2_EXCEPT (EXCEPTION_EXECUTE_HANDLER) {
                BrutePoint();
            } _SEH2_END;
        }

        UDFReleaseResource(&(Vcb->VCBResource));
        AcquiredVcb = FALSE;

        if(System) {
            // Remove from system queue
            PFCB Fcb;
            IO_STATUS_BLOCK IoStatus;
            BOOLEAN NoDelayed = (Vcb->VCBFlags & VCB_STATE_NO_DELAYED_CLOSE) ?
                                     TRUE : FALSE;

            Vcb->VCBFlags |= VCB_STATE_NO_DELAYED_CLOSE;
            for(i=FoundListSize;i>0;i--) {
                UDFAcquireResourceExclusive(&(Vcb->VCBResource), TRUE);
                AcquiredVcb = TRUE;
                _SEH2_TRY {

                    CurListPtr = ListPtrArray[i-1];
                    CurFileInfo = CurListPtr->FileInfo;
                    if(CurFileInfo &&
                       (Fcb = CurFileInfo->Fcb)) {
                        ASSERT((ULONG_PTR)Fcb > 0x1000);
//                            ASSERT((ULONG)(Fcb->SectionObject) > 0x1000);
                        if(!(Fcb->NtReqFCBFlags & UDF_NTREQ_FCB_DELETED) &&
                            (Fcb->NtReqFCBFlags & UDF_NTREQ_FCB_MODIFIED)) {
                            MmPrint(("    CcFlushCache()\n"));
                            CcFlushCache(&Fcb->SectionObject, NULL, 0, &IoStatus);
                        }
                        if(Fcb->SectionObject.ImageSectionObject) {
                            MmPrint(("    MmFlushImageSection()\n"));
                            MmFlushImageSection(&Fcb->SectionObject, MmFlushForWrite);
                        }
                        if(Fcb->SectionObject.DataSectionObject) {
                            MmPrint(("    CcPurgeCacheSection()\n"));
                            CcPurgeCacheSection(&Fcb->SectionObject, NULL, 0, FALSE);
                        }
                    } else {
                        MmPrint(("    Skip item: deleted\n"));
                    }
                    CurListPtr->EntryRefCount--;
                    if(!CurListPtr->EntryRefCount) {
                        if(CurListPtr->FileInfo)
                            CurListPtr->FileInfo->ListPtr = NULL;
                        MyFreePool__(CurListPtr);
                    }
                } _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
                    BrutePoint();
                } _SEH2_END;
                UDFReleaseResource(&(Vcb->VCBResource));
                AcquiredVcb = FALSE;
            }
            if(!NoDelayed)
                Vcb->VCBFlags &= ~VCB_STATE_NO_DELAYED_CLOSE;
        } else {
            // Remove from internal queue
            PIRP_CONTEXT_LITE NextIrpContextLite;

            for(i=FoundListSize;i>0;i--) {

                UDFAcquireResourceExclusive(&(Vcb->VCBResource), TRUE);
                AcquiredVcb = TRUE;

                CurListPtr = ListPtrArray[i-1];
                CurFileInfo = CurListPtr->FileInfo;

                if(CurFileInfo &&
                   CurFileInfo->Fcb &&
                    (NextIrpContextLite = CurFileInfo->Fcb->IrpContextLite)) {
                    RemoveEntryList( &(NextIrpContextLite->DelayedCloseLinks) );
                    if (NextIrpContextLite->Fcb->FCBFlags & UDF_FCB_DIRECTORY) {
//                            BrutePoint();
                        UDFGlobalData.DirDelayedCloseCount--;
                    } else {
                        UDFGlobalData.DelayedCloseCount--;
                    }
                    UDFDoDelayedClose(NextIrpContextLite);
                }
                CurListPtr->EntryRefCount--;
                if(!CurListPtr->EntryRefCount) {
                    if(CurListPtr->FileInfo)
                        CurListPtr->FileInfo->ListPtr = NULL;
                    MyFreePool__(CurListPtr);
                }
                UDFReleaseResource(&(Vcb->VCBResource));
                AcquiredVcb = FALSE;
            }
        }
        RC = STATUS_SUCCESS;

try_exit: NOTHING;

    } _SEH2_FINALLY {
        // release Vcb
        if(AcquiredVcb)
            UDFReleaseResource(&(Vcb->VCBResource));
        // Release DelayedCloseResource
        if(ResAcq)
            UDFReleaseResource(&(UDFGlobalData.DelayedCloseResource));

        if(ListPtrArray)
            MyFreePool__(ListPtrArray);
        if(PassedList)
            MyFreePool__(PassedList);
        if(FoundList)
            MyFreePool__(FoundList);
    } _SEH2_END;

    return RC;
} // end UDFCloseAllXXXDelayedInDir(


/*
    This routine adds request to Delayed Close queue.
    If number of queued requests exceeds higher threshold it fires
    UDFDelayedClose()
 */
NTSTATUS
UDFQueueDelayedClose(
    PIRP_CONTEXT IrpContext,
    PFCB                Fcb
    )
{
    PIRP_CONTEXT_LITE IrpContextLite;
    BOOLEAN                 StartWorker = FALSE;
    _SEH2_VOLATILE BOOLEAN  AcquiredVcb = FALSE;
    NTSTATUS                RC;

    AdPrint(("  UDFQueueDelayedClose\n"));

    _SEH2_TRY {
        // Acquire DelayedCloseResource
        UDFAcquireResourceExclusive(&(UDFGlobalData.DelayedCloseResource), TRUE);

        UDFAcquireResourceShared(&Fcb->Vcb->VCBResource, TRUE);
        AcquiredVcb = TRUE;

        if(Fcb->FCBFlags & UDF_FCB_DELETE_ON_CLOSE) {
            try_return(RC = STATUS_DELETE_PENDING);
        }

        if(Fcb->IrpContextLite ||
           Fcb->FCBFlags & UDF_FCB_POSTED_RENAME) {
//            BrutePoint();
            try_return(RC = STATUS_UNSUCCESSFUL);
        }

        if(!NT_SUCCESS(RC = UDFInitializeIrpContextLite(&IrpContextLite,IrpContext,Fcb))) {
            try_return(RC);
        }

        if(Fcb->FCBFlags & UDF_FCB_DIRECTORY) {
            InsertTailList( &UDFGlobalData.DirDelayedCloseQueue,
                            &IrpContextLite->DelayedCloseLinks );
            UDFGlobalData.DirDelayedCloseCount++;
        } else {
            InsertTailList( &UDFGlobalData.DelayedCloseQueue,
                            &IrpContextLite->DelayedCloseLinks );
            UDFGlobalData.DelayedCloseCount++;
        }
        Fcb->IrpContextLite = IrpContextLite;

        //  If we are above our threshold then start the delayed
        //  close operation.
        if(UDFGlobalData.DelayedCloseCount > UDFGlobalData.MaxDelayedCloseCount) {

            UDFGlobalData.ReduceDelayedClose = TRUE;

            if(!UDFGlobalData.FspCloseActive) {

                UDFGlobalData.FspCloseActive = TRUE;
                StartWorker = TRUE;
            }
        }
        //  If we are above our threshold then start the delayed
        //  close operation.
        if(UDFGlobalData.DirDelayedCloseCount > UDFGlobalData.MaxDirDelayedCloseCount) {

            UDFGlobalData.ReduceDirDelayedClose = TRUE;

            if(!UDFGlobalData.FspCloseActive) {

                UDFGlobalData.FspCloseActive = TRUE;
                StartWorker = TRUE;
            }
        }
        // Start the FspClose thread if we need to.
        if(StartWorker) {
            ExQueueWorkItem( &UDFGlobalData.CloseItem, CriticalWorkQueue );
        }
        RC = STATUS_SUCCESS;

try_exit:    NOTHING;

    } _SEH2_FINALLY {

        if(!NT_SUCCESS(RC)) {
            Fcb->FCBFlags &= ~UDF_FCB_DELAY_CLOSE;
        }
        if(AcquiredVcb) {
            UDFReleaseResource(&(Fcb->Vcb->VCBResource));
        }
        // Release DelayedCloseResource
        UDFReleaseResource(&(UDFGlobalData.DelayedCloseResource));
    } _SEH2_END;
    return RC;
} // end UDFQueueDelayedClose()

