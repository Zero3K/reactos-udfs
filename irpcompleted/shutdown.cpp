////////////////////////////////////////////////////////////////////
// Copyright (C) Alexander Telyatnikov, Ivan Keliukh, Yegor Anchishkin, SKIF Software, 1999-2013. Kiev, Ukraine
// All rights reserved
// This file was released under the GPLv2 on June 2015.
////////////////////////////////////////////////////////////////////
/*************************************************************************
*
* File: Shutdown.cpp
*
* Module: UDF File System Driver (Kernel mode execution only)
*
* Description:
*   Contains code to handle the "shutdown notification" dispatch entry point.
*
*************************************************************************/

#include            "udffs.h"

// define the file specific bug-check id
#define         UDF_BUG_CHECK_ID                UDF_FILE_SHUTDOWN



/*************************************************************************
*
* Function: UDFShutdown()
*
* Description:
*   All disk-based FSDs can expect to receive this shutdown notification
*   request whenever the system is about to be halted gracefully. If you
*   design and implement a network redirector, you must register explicitly
*   for shutdown notification by invoking the IoRegisterShutdownNotification()
*   routine from your driver entry.
*
*   Note that drivers that register to receive shutdown notification get
*   invoked BEFORE disk-based FSDs are told about the shutdown notification.
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
UDFShutdown(
    PDEVICE_OBJECT   DeviceObject,       // the logical volume device object
    PIRP             Irp                 // I/O Request Packet
    )
{
    NTSTATUS         RC = STATUS_SUCCESS;
    PIRP_CONTEXT IrpContext = NULL;
    BOOLEAN          AreWeTopLevel = FALSE;

    UDFPrint(("UDFShutDown\n"));
//    BrutePoint();

    FsRtlEnterFileSystem();
    ASSERT(DeviceObject);
    ASSERT(Irp);

    // set the top level context
    AreWeTopLevel = UDFIsIrpTopLevel(Irp);
    //ASSERT(!UDFIsFSDevObj(DeviceObject));

    _SEH2_TRY {

        // get an IRP context structure and issue the request
        IrpContext = UDFCreateIrpContext(Irp, DeviceObject);
        if(IrpContext) {
            RC = UDFCommonShutdown(IrpContext, Irp);
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
} // end UDFShutdown()


/*************************************************************************
*
* Function: UDFCommonShutdown()
*
* Description:
*   The actual work is performed here. Basically, all we do here is
*   internally invoke a flush on all mounted logical volumes. This, in
*   tuen, will result in all open file streams being flushed to disk.
*
* Expected Interrupt Level (for execution) :
*
*  IRQL_PASSIVE_LEVEL
*
* Return Value: Irrelevant
*
*************************************************************************/
NTSTATUS
UDFCommonShutdown(
    PIRP_CONTEXT IrpContext,
    PIRP             Irp
    )
{
    NTSTATUS            RC = STATUS_SUCCESS;
    PIO_STACK_LOCATION  IrpSp = NULL;
    PVCB Vcb;
    PLIST_ENTRY Link;
    LARGE_INTEGER delay;

    UDFPrint(("UDFCommonShutdown\n"));

    // Initialize an event for doing calls down to our target device objects.
    KEVENT Event;
    KeInitializeEvent(&Event, NotificationEvent, FALSE);

    _SEH2_TRY {
        // First, get a pointer to the current I/O stack location
        IrpSp = IoGetCurrentIrpStackLocation(Irp);
        ASSERT(IrpSp);

        // (a) Block all new "mount volume" requests by acquiring an appropriate
        //       global resource/lock.
        // (b) Go through your linked list of mounted logical volumes and for
        //       each such volume, do the following:
        //       (i) acquire the volume resource exclusively
        //       (ii) invoke UDFFlushLogicalVolume() (internally) to flush the
        //              open data streams belonging to the volume from the system
        //              cache
        //       (iii) Invoke the physical/virtual/logical target device object
        //              on which the volume is mounted and inform this device
        //              about the shutdown request (Use IoBuildSynchronouFsdRequest()
        //              to create an IRP with MajorFunction = IRP_MJ_SHUTDOWN that you
        //              will then issue to the target device object).
        //       (iv) Wait for the completion of the shutdown processing by the target
        //              device object
        //       (v) Release the VCB resource we will have acquired in (i) above.

        // Acquire GlobalDataResource
        UDFAcquireResourceExclusive(&(UDFGlobalData.GlobalDataResource), TRUE);
        // Walk through all of the Vcb's attached to the global data.
        Link = UDFGlobalData.VCBQueue.Flink;

        while (Link != &(UDFGlobalData.VCBQueue)) {
            // Get 'next' Vcb
            Vcb = CONTAINING_RECORD( Link, VCB, NextVCB );
            // Move to the next link now since the current Vcb may be deleted.
            Link = Link->Flink;
            ASSERT(Link != Link->Flink);

            if(Vcb->VCBFlags & VCB_STATE_SHUTDOWN) {
                continue;
            }

#ifdef UDF_DELAYED_CLOSE
            UDFAcquireResourceExclusive(&(Vcb->VCBResource), TRUE);
            UDFPrint(("    UDFCommonShutdown:     set VCB_STATE_NO_DELAYED_CLOSE\n"));
            Vcb->VCBFlags |= VCB_STATE_NO_DELAYED_CLOSE;
            UDFReleaseResource(&(Vcb->VCBResource));
#endif //UDF_DELAYED_CLOSE

            if(Vcb->RootDirFCB && Vcb->RootDirFCB->FileInfo) {
                UDFPrint(("    UDFCommonShutdown:     UDFCloseAllSystemDelayedInDir\n"));
                RC = UDFCloseAllSystemDelayedInDir(Vcb, Vcb->RootDirFCB->FileInfo);
                ASSERT(OS_SUCCESS(RC));
            }

#ifdef UDF_DELAYED_CLOSE
            UDFCloseAllDelayed(Vcb);
#endif //UDF_DELAYED_CLOSE

            // Acquire Vcb resource
            UDFAcquireResourceExclusive(&(Vcb->VCBResource), TRUE);

            ASSERT(!Vcb->OverflowQueueCount);

            {
            _SEH2_TRY {

                IO_STATUS_BLOCK Iosb;

                PIRP NewIrp = IoBuildSynchronousFsdRequest(IRP_MJ_SHUTDOWN,
                                                           Vcb->TargetDeviceObject,
                                                           NULL,
                                                           0,
                                                           NULL,
                                                           &Event,
                                                           &Iosb);

                if (NewIrp != NULL) {

                    if (NT_SUCCESS(IoCallDriver( Vcb->TargetDeviceObject, NewIrp ))) {

                        (VOID) KeWaitForSingleObject(&Event,
                                                     Executive,
                                                     KernelMode,
                                                     FALSE,
                                                     NULL);

                        KeClearEvent(&Event);
                    }
                }

            } _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {

            } _SEH2_END;
            }

            ASSERT(!Vcb->OverflowQueueCount);

            if(!(Vcb->VCBFlags & VCB_STATE_SHUTDOWN)) {

                UDFDoDismountSequence(Vcb, FALSE);
                if(Vcb->VCBFlags & VCB_STATE_REMOVABLE_MEDIA) {
                    // let drive flush all data before reset
                    delay.QuadPart = -10000000; // 1 sec
                    KeDelayExecutionThread(KernelMode, FALSE, &delay);
                }
                Vcb->VCBFlags |= (VCB_STATE_SHUTDOWN |
                                  VCB_STATE_VOLUME_READ_ONLY);
            }

            UDFReleaseResource(&(Vcb->VCBResource));
        }

        // Once we have processed all the mounted logical volumes, we can release
        // all acquired global resources and leave (in peace :-)
        UDFReleaseResource( &(UDFGlobalData.GlobalDataResource) );

        // Now, delete any device objects, etc. we may have created
        IoUnregisterFileSystem(UDFGlobalData.UDFDeviceObject_CD);
        if (UDFGlobalData.UDFDeviceObject_CD) {
            IoDeleteDevice(UDFGlobalData.UDFDeviceObject_CD);
            UDFGlobalData.UDFDeviceObject_CD = NULL;
        }
        IoUnregisterFileSystem(UDFGlobalData.UDFDeviceObject_HDD);
        if (UDFGlobalData.UDFDeviceObject_HDD) {
            IoDeleteDevice(UDFGlobalData.UDFDeviceObject_HDD);
            UDFGlobalData.UDFDeviceObject_HDD = NULL;
        }

        // free up any memory we might have reserved for zones/lookaside
        //  lists
        if (UDFGlobalData.UDFFlags & UDF_DATA_FLAGS_ZONES_INITIALIZED) {
            UDFDestroyZones();
        }

        // delete the resource we may have initialized
        if (UDFGlobalData.UDFFlags & UDF_DATA_FLAGS_RESOURCE_INITIALIZED) {
            // un-initialize this resource
            UDFDeleteResource(&(UDFGlobalData.GlobalDataResource));
            ClearFlag(UDFGlobalData.UDFFlags, UDF_DATA_FLAGS_RESOURCE_INITIALIZED);
        }

        RC = STATUS_SUCCESS;

    } _SEH2_FINALLY {

        if(!_SEH2_AbnormalTermination()) {
            Irp->IoStatus.Status = RC;
            Irp->IoStatus.Information = 0;
            // Free up the Irp Context
            UDFCleanupIrpContext(IrpContext);
                // complete the IRP
            IoCompleteRequest(Irp, IO_DISK_INCREMENT);
        }

    } _SEH2_END; // end of "__finally" processing

    return(RC);
} // end UDFCommonShutdown()
