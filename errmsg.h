////////////////////////////////////////////////////////////////////
// Copyright (C) Alexander Telyatnikov, Ivan Keliukh, Yegor Anchishkin, SKIF Software, 1999-2013. Kiev, Ukraine
// All rights reserved
// This file was released under the GPLv2 on June 2015.
////////////////////////////////////////////////////////////////////
/*************************************************************************
*
* File:     errmsg.msg
*
* Product:      UDF FSD
*
* Module:       UDF FSD Event Log Messages
*
* Description:
*     Contains error strings in a format understandable to the message compiler.
*     Please compile (using mc) with the -c option which will set the
*     "Customer" bit in all errors.
*     Use values beginning at 0xA000 (e.g. 0xA001) for the UDF FSD
*     errors.
*     Do NOT use %1 for insertion strings. The I/O manager assumes that
*     the first insertion string is the name of the driver/device.
*
*************************************************************************/
//
//  Values are 32 bit values layed out as follows:
//
//   3 3 2 2 2 2 2 2 2 2 2 2 1 1 1 1 1 1 1 1 1 1
//   1 0 9 8 7 6 5 4 3 2 1 0 9 8 7 6 5 4 3 2 1 0 9 8 7 6 5 4 3 2 1 0
//  +---+-+-+-----------------------+-------------------------------+
//  |Sev|C|R|     Facility          |               Code            |
//  +---+-+-+-----------------------+-------------------------------+
//
//  where
//
//      Sev - is the severity code
//
//          00 - Success
//          01 - Informational
//          10 - Warning
//          11 - Error
//
//      C - is the Customer code flag
//
//      R - is a reserved bit
//
//      Facility - is the facility code
//
//      Code - is the facility's status code
//
//
// Define the facility codes
//


//
// Define the severity codes
//
#define STATUS_SEVERITY_WARNING          0x2
#define STATUS_SEVERITY_SUCCESS          0x0
#define STATUS_SEVERITY_INFORMATIONAL    0x1
#define STATUS_SEVERITY_ERROR            0x3


//
// MessageId: UDF_ERROR_INTERNAL_ERROR
//
// MessageText:
//
//  The UDF FSD encountered an internal error. Please check log data information.
//
#define UDF_ERROR_INTERNAL_ERROR        ((ULONG)0xE004A001L)

