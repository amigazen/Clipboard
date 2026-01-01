/*
 * Clipboard
 *
 * Copyright (c) 2025 amigazen project
 * Licensed under BSD 2-Clause License
 */

#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/io.h>
#include <dos/dos.h>
#include <intuition/intuition.h>
#include <intuition/intuitionbase.h>
#include <intuition/classusr.h>
#include <intuition/classes.h>
#include <datatypes/datatypes.h>
#include <datatypes/datatypesclass.h>
#include <datatypes/textclass.h>
#include <libraries/iffparse.h>
#include <devices/clipboard.h>
#include <utility/tagitem.h>
#include <intuition/icclass.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/datatypes.h>
#include <proto/iffparse.h>
#include <proto/utility.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>

/* IFF chunk IDs */
#ifndef MAKE_ID
#define MAKE_ID(a,b,c,d) ((ULONG) (a)<<24 | (ULONG) (b)<<16 | (ULONG) (c)<<8 | (ULONG) (d))
#endif

#define ID_FTXT MAKE_ID('F','T','X','T')
#define ID_CHRS MAKE_ID('C','H','R','S')
#define ID_FORM MAKE_ID('F','O','R','M')

/* Library base pointers */
extern struct ExecBase *SysBase;
extern struct DosLibrary *DOSBase;
extern struct IntuitionBase *IntuitionBase;
extern struct Library *DataTypesBase;
extern struct Library *UtilityBase;
extern struct Library *IFFParseBase;

/* Forward declarations */
BOOL InitializeLibraries(VOID);
VOID Cleanup(VOID);
VOID ShowUsage(VOID);
LONG CopyToClipboard(STRPTR fileName, ULONG unit);
LONG PasteFromClipboard(STRPTR fileName, ULONG unit);
LONG ListClipboards(VOID);
ULONG FormIDToUnit(ULONG formID);
LONG FlushClipboard(ULONG unit);

static const char *verstag = "$VER: Clipboard 1.0 (31.12.2025)\n";
static const char *stack_cookie = "$STACK: 4096\n";
const long oslibversion = 45L;

/* Main entry point */
int main(int argc, char *argv[])
{
    struct RDArgs *rda;
    STRPTR copyFile = NULL;
    STRPTR pasteFile = NULL;
    ULONG unit = 0;
    BOOL listMode = FALSE;
    BOOL flushMode = FALSE;
    LONG args[5];
    LONG result = RETURN_FAIL;
    
    /* Check if running from CLI */
    if (argc == 0) {
        /* Running from Workbench - not supported yet */
        return RETURN_FAIL;
    }
    
    /* Initialize libraries */
    if (!InitializeLibraries()) {
        LONG errorCode = IoErr();
        if (errorCode == 0) {
            errorCode = ERROR_INVALID_RESIDENT_LIBRARY;
        }
        PrintFault(errorCode, "Clipboard");
        return RETURN_FAIL;
    }
    
    /* Parse command line arguments */
    /* Template: COPY/K,PASTE/K,CLIPUNIT/K/N,LIST/S,FLUSH/S */
    memset(args, 0, sizeof(args));
    
    rda = ReadArgs("COPY/K,PASTE/K,CLIPUNIT/K/N,LIST/S,FLUSH/S", args, NULL);
    if (!rda) {
        LONG errorCode = IoErr();
        ShowUsage();
        Cleanup();
        return RETURN_FAIL;
    }
    
    /* Extract arguments */
    if (args[0]) copyFile = (STRPTR)args[0];
    if (args[1]) pasteFile = (STRPTR)args[1];
    if (args[2]) unit = *((ULONG *)args[2]);
    if (args[3]) listMode = TRUE;
    if (args[4]) flushMode = TRUE;
    
    /* Validate unit number */
    if (unit > 255) {
        PrintFault(ERROR_BAD_NUMBER, "Clipboard");
        FreeArgs(rda);
        Cleanup();
        return RETURN_FAIL;
    }
    
    /* Check that at least one operation is specified */
    if (!copyFile && !pasteFile && !listMode && !flushMode) {
        ShowUsage();
        FreeArgs(rda);
        Cleanup();
        return RETURN_FAIL;
    }
    
    /* If LIST mode, enumerate all clipboard units */
    if (listMode) {
        result = ListClipboards();
        FreeArgs(rda);
        Cleanup();
        return result;
    }
    
    /* If FLUSH mode, clear the clipboard unit */
    if (flushMode) {
        result = FlushClipboard(unit);
        FreeArgs(rda);
        Cleanup();
        return result;
    }
    
    /* Perform the requested operation(s) */
    /* If both COPY and PASTE are specified, do COPY first, then PASTE */
    {
        if (copyFile) {
            result = CopyToClipboard(copyFile, unit);
            if (result != RETURN_OK && pasteFile) {
                /* Copy failed, but continue with paste if requested */
                result = RETURN_FAIL;
            }
        }
        
        if (pasteFile) {
            LONG pasteResult;
            pasteResult = PasteFromClipboard(pasteFile, unit);
            if (pasteResult != RETURN_OK) {
                result = pasteResult;  /* Use paste result if copy succeeded but paste failed */
            }
            /* If both operations, result is OK only if both succeeded */
        }
    }
    
    /* Cleanup */
    FreeArgs(rda);
    Cleanup();
    
    return result;
}

/* Initialize required libraries */
BOOL InitializeLibraries(VOID)
{
    /* Open intuition.library - version 39 is standard for OS 3.x */
    IntuitionBase = (struct IntuitionBase *)OpenLibrary("intuition.library", 39L);
    if (IntuitionBase == NULL) {
        SetIoErr(ERROR_INVALID_RESIDENT_LIBRARY);
        return FALSE;
    }
    
    /* Open utility.library - version 39 is standard for OS 3.x */
    UtilityBase = OpenLibrary("utility.library", 39L);
    if (UtilityBase == NULL) {
        SetIoErr(ERROR_INVALID_RESIDENT_LIBRARY);
        CloseLibrary((struct Library *)IntuitionBase);
        IntuitionBase = NULL;
        return FALSE;
    }
    
    /* Open datatypes.library - version 45 is minimum for DTM_COPY support */
    DataTypesBase = OpenLibrary("datatypes.library", 45L);
    if (DataTypesBase == NULL) {
        SetIoErr(ERROR_INVALID_RESIDENT_LIBRARY);
        CloseLibrary(UtilityBase);
        UtilityBase = NULL;
        CloseLibrary((struct Library *)IntuitionBase);
        IntuitionBase = NULL;
        return FALSE;
    }
    
    /* Open iffparse.library for paste operations - version 39 is standard */
    IFFParseBase = OpenLibrary("iffparse.library", 39L);
    if (IFFParseBase == NULL) {
        SetIoErr(ERROR_INVALID_RESIDENT_LIBRARY);
        CloseLibrary(DataTypesBase);
        DataTypesBase = NULL;
        CloseLibrary(UtilityBase);
        UtilityBase = NULL;
        CloseLibrary((struct Library *)IntuitionBase);
        IntuitionBase = NULL;
        return FALSE;
    }
    
    return TRUE;
}

/* Cleanup libraries */
VOID Cleanup(VOID)
{
    if (IFFParseBase) {
        CloseLibrary(IFFParseBase);
        IFFParseBase = NULL;
    }
    
    if (DataTypesBase) {
        CloseLibrary(DataTypesBase);
        DataTypesBase = NULL;
    }
    
    if (UtilityBase) {
        CloseLibrary(UtilityBase);
        UtilityBase = NULL;
    }
    
    if (IntuitionBase) {
        CloseLibrary((struct Library *)IntuitionBase);
        IntuitionBase = NULL;
    }
}

/* Show usage information */
VOID ShowUsage(VOID)
{
    Printf("Clipboard - Universal clipboard and file converter\n");
    Printf("Usage: Clipboard COPY=<file> [PASTE=<file>] [CLIPUNIT=<n>]\n");
    Printf("       Clipboard PASTE=<file> [CLIPUNIT=<n>]\n");
    Printf("       Clipboard LIST\n");
    Printf("\n");
    Printf("Options:\n");
    Printf("  COPY=<file>    Copy file to clipboard (converts to IFF via datatypes)\n");
    Printf("  PASTE=<file>   Paste clipboard to file (extracts text from FTXT)\n");
    Printf("  CLIPUNIT=<n>   Clipboard unit number (0-255, default 0)\n");
    Printf("  LIST           List all clipboard units with content (0-255)\n");
    Printf("  FLUSH          Clear the specified clipboard unit\n");
    Printf("\n");
    Printf("Note: COPY and PASTE can be used together. COPY is performed first.\n");
    Printf("\n");
    Printf("Examples:\n");
    Printf("  Clipboard COPY=image.jpg          # Copy image to clipboard\n");
    Printf("  Clipboard PASTE=output.txt        # Paste clipboard to file\n");
    Printf("  Clipboard COPY=file.txt PASTE=out.txt  # Copy then paste\n");
    Printf("  Clipboard COPY=file.txt CLIPUNIT=1    # Copy to clipboard unit 1\n");
    Printf("  Clipboard LIST                    # List all clipboard units\n");
    Printf("  Clipboard FLUSH                  # Clear clipboard unit 0\n");
    Printf("  Clipboard FLUSH CLIPUNIT=5       # Clear clipboard unit 5\n");
}

/* Check if datatype object supports DTM_COPY method */
BOOL SupportsDTMCopy(Object *dtObject)
{
    ULONG *methods;
    ULONG *found;
    
    if (!dtObject) {
        return FALSE;
    }
    
    /* Get list of supported methods */
    methods = GetDTMethods(dtObject);
    if (!methods) {
        return FALSE;
    }
    
    /* Search for DTM_COPY in the methods array */
    found = FindMethod(methods, DTM_COPY);
    
    /* FindMethod returns NULL if method not found, non-NULL if found */
    return (BOOL)(found != NULL);
}

/* Copy file to clipboard using datatypes API */
LONG CopyToClipboard(STRPTR fileName, ULONG unit)
{
    Object *dtObject;
    struct IFFHandle *iffh;
    struct IFFHandle *readIffh;
    struct ClipboardHandle *clipHandle;
    STRPTR tempFile;
    BPTR tempFileHandle;
    LONG bytesRead;
    LONG bytesWritten;
    LONG result = RETURN_FAIL;
    ULONG *methods;
    BOOL supportsWrite = FALSE;
    
    if (!fileName || *fileName == '\0') {
        PrintFault(ERROR_OBJECT_NOT_FOUND, "Clipboard: No file specified");
        return RETURN_FAIL;
    }
    
    /* Create datatype object from file */
    /* NewDTObject will automatically detect the file type and create appropriate object */
    /* Don't restrict GroupID - we want to accept any file type (text, picture, sound, etc.) */
    dtObject = NewDTObject((APTR)fileName,
                           TAG_END);
    
    if (!dtObject) {
        LONG errorCode = IoErr();
        if (errorCode == 0) {
            errorCode = ERROR_OBJECT_NOT_FOUND;
        }
        PrintFault(errorCode, "Clipboard: Could not create datatype object");
        return RETURN_FAIL;
    }
    
    /* Check if the datatype supports DTM_WRITE method */
    methods = GetDTMethods(dtObject);
    if (methods) {
        if (FindMethod(methods, DTM_WRITE)) {
            supportsWrite = TRUE;
        }
    }
    
    if (!supportsWrite) {
        PrintFault(ERROR_OBJECT_WRONG_TYPE, "Clipboard: File type does not support clipboard write");
        DisposeDTObject(dtObject);
        return RETURN_FAIL;
    }
    
    /* Save object to temporary RAM: file using SaveDTObjectA */
    tempFile = "RAM:clipboard_temp.iff";
    
    if (!SaveDTObjectA(dtObject, NULL, NULL, tempFile, DTWM_IFF, FALSE, TAG_END)) {
        LONG errorCode = IoErr();
        PrintFault(errorCode ? errorCode : ERROR_WRITE_PROTECTED, "Clipboard: Failed to save object to temporary file");
        DisposeDTObject(dtObject);
        return RETURN_FAIL;
    }
    DisposeDTObject(dtObject);
    dtObject = NULL;
    
    /* Temp file is ready - SaveDTObjectA with DTWM_IFF should have created a valid IFF file */
    /* We can write it directly to the clipboard using CMD_WRITE, like Copy2Clip does */
    
    clipHandle = OpenClipboard(unit);
    if (!clipHandle) {
        LONG errorCode = IoErr();
        if (errorCode == 0) {
            errorCode = ERROR_OBJECT_NOT_FOUND;
        }
        PrintFault(errorCode, "Clipboard: Could not open clipboard");
        return RETURN_FAIL;
    }
    
    /* Open temp file for reading */
    tempFileHandle = Open(tempFile, MODE_OLDFILE);
    if (!tempFileHandle) {
        LONG errorCode = IoErr();
        CloseClipboard(clipHandle);
        PrintFault(errorCode ? errorCode : ERROR_OBJECT_NOT_FOUND, "Clipboard: Could not open temporary file");
        return RETURN_FAIL;
    }
    
    /* Get file size */
    Seek(tempFileHandle, 0, OFFSET_END);
    {
        LONG fileSize = Seek(tempFileHandle, 0, OFFSET_BEGINNING);
        
        if (fileSize > 0) {
            /* Allocate buffer for file data */
            APTR fileBuffer = AllocMem(fileSize, MEMF_ANY);
            if (fileBuffer) {
                /* Read entire IFF file */
                bytesRead = Read(tempFileHandle, fileBuffer, fileSize);
                
                if (bytesRead == fileSize) {
                    /* Write to clipboard using CMD_WRITE directly */
                    /* ClipboardHandle contains cbh_Req which is an IOClipReq */
                    struct IOClipReq *ioreq = &clipHandle->cbh_Req;
                    
                    ioreq->io_Offset = 0;
                    ioreq->io_ClipID = 0;
                    ioreq->io_Command = CMD_WRITE;
                    ioreq->io_Data = fileBuffer;
                    ioreq->io_Length = fileSize;
                    
                    DoIO((struct IORequest *)ioreq);
                    
                    if (ioreq->io_Error == 0) {
                        /* Send CMD_UPDATE to finalize */
                        ioreq->io_Command = CMD_UPDATE;
                        ioreq->io_ClipID = ioreq->io_ClipID;  /* Use the ClipID from write */
                        DoIO((struct IORequest *)ioreq);
                        
                        if (ioreq->io_Error == 0) {
                            result = RETURN_OK;
                        } else {
                            PrintFault(ioreq->io_Error, "Clipboard: Could not update clipboard");
                        }
                    } else {
                        PrintFault(ioreq->io_Error, "Clipboard: Could not write to clipboard");
                    }
                } else {
                    LONG errorCode = IoErr();
                    PrintFault(errorCode ? errorCode : ERROR_READ_PROTECTED, "Clipboard: Could not read temporary file");
                }
                
                FreeMem(fileBuffer, fileSize);
            } else {
                PrintFault(ERROR_NO_FREE_STORE, "Clipboard: Could not allocate memory");
            }
        } else {
            PrintFault(ERROR_OBJECT_WRONG_TYPE, "Clipboard: Temporary file is empty");
        }
    }
    
    Close(tempFileHandle);
    CloseClipboard(clipHandle);
    
    return result;
}

/* Extract text from FTXT/CHRS chunks and write to file */
LONG ExtractTextFromClipboard(STRPTR fileName, ULONG unit)
{
    struct IFFHandle *iffh = NULL;
    struct ClipboardHandle *clipHandle = NULL;
    struct ContextNode *cn = NULL;
    BPTR outputFile = NULL;
    UBYTE *buffer = NULL;
    ULONG bufferSize = 4096;
    LONG len = 0;
    LONG result = RETURN_FAIL;
    
    /* Allocate IFF handle */
    if (!(iffh = AllocIFF())) {
        PrintFault(ERROR_NO_FREE_STORE, "Clipboard: Could not allocate IFF handle");
        return RETURN_FAIL;
    }
    
    /* Open clipboard */
    if (!(clipHandle = OpenClipboard(unit))) {
        PrintFault(ERROR_OBJECT_NOT_FOUND, "Clipboard: Could not open clipboard");
        FreeIFF(iffh);
        return RETURN_FAIL;
    }
    
    /* Initialize IFF for clipboard */
    InitIFFasClip(iffh);
    iffh->iff_Stream = (ULONG)clipHandle;
    
    /* Open IFF for reading */
    if (OpenIFF(iffh, IFFF_READ)) {
        PrintFault(ERROR_OBJECT_NOT_FOUND, "Clipboard: Could not open clipboard for reading");
        CloseClipboard(clipHandle);
        FreeIFF(iffh);
        return RETURN_FAIL;
    }
    
    /* Stop on FTXT/CHRS chunks */
    if (StopChunk(iffh, ID_FTXT, ID_CHRS)) {
        PrintFault(ERROR_OBJECT_WRONG_TYPE, "Clipboard: Could not register IFF chunk");
        CloseIFF(iffh);
        CloseClipboard(clipHandle);
        FreeIFF(iffh);
        return RETURN_FAIL;
    }
    
    /* Parse IFF to find CHRS chunk */
    if (ParseIFF(iffh, IFFPARSE_SCAN)) {
        PrintFault(ERROR_OBJECT_NOT_FOUND, "Clipboard: Clipboard is empty or contains invalid data");
        CloseIFF(iffh);
        CloseClipboard(clipHandle);
        FreeIFF(iffh);
        return RETURN_FAIL;
    }
    
    /* Get current chunk */
    if (!(cn = CurrentChunk(iffh))) {
        PrintFault(ERROR_OBJECT_WRONG_TYPE, "Clipboard: Clipboard contains invalid data");
        CloseIFF(iffh);
        CloseClipboard(clipHandle);
        FreeIFF(iffh);
        return RETURN_FAIL;
    }
    
    /* Check if it's a FTXT/CHRS chunk */
    if (cn->cn_Type != ID_FTXT || cn->cn_ID != ID_CHRS) {
        PrintFault(ERROR_OBJECT_WRONG_TYPE, "Clipboard: Clipboard does not contain text data");
        CloseIFF(iffh);
        CloseClipboard(clipHandle);
        FreeIFF(iffh);
        return RETURN_FAIL;
    }
    
    /* Allocate buffer for reading */
    bufferSize = cn->cn_Size + 1;
    if (!(buffer = AllocMem(bufferSize, MEMF_ANY | MEMF_CLEAR))) {
        PrintFault(ERROR_NO_FREE_STORE, "Clipboard: Could not allocate memory");
        CloseIFF(iffh);
        CloseClipboard(clipHandle);
        FreeIFF(iffh);
        return RETURN_FAIL;
    }
    
    /* Read CHRS chunk data */
    len = ReadChunkBytes(iffh, buffer, cn->cn_Size);
    if (len < 0) {
        PrintFault(ERROR_READ_PROTECTED, "Clipboard: Could not read clipboard data");
        FreeMem(buffer, bufferSize);
        CloseIFF(iffh);
        CloseClipboard(clipHandle);
        FreeIFF(iffh);
        return RETURN_FAIL;
    }
    
    /* Open output file */
    if (fileName && *fileName != '\0') {
        outputFile = Open(fileName, MODE_NEWFILE);
        if (!outputFile) {
            PrintFault(IoErr(), "Clipboard: Could not open output file");
            FreeMem(buffer, bufferSize);
            CloseIFF(iffh);
            CloseClipboard(clipHandle);
            FreeIFF(iffh);
            return RETURN_FAIL;
        }
    } else {
        /* No filename specified, use stdout */
        outputFile = Output();
    }
    
    /* Write text to file */
    if (Write(outputFile, buffer, len) != len) {
        PrintFault(IoErr(), "Clipboard: Could not write to file");
        result = RETURN_FAIL;
    } else {
        result = RETURN_OK;
    }
    
    /* Cleanup */
    if (outputFile && outputFile != Output()) {
        Close(outputFile);
    }
    FreeMem(buffer, bufferSize);
    CloseIFF(iffh);
    CloseClipboard(clipHandle);
    FreeIFF(iffh);
    
    return result;
}

/* Paste from clipboard to file using datatypes API */
LONG PasteFromClipboard(STRPTR fileName, ULONG unit)
{
    Object *dtObject = NULL;
    UBYTE unitStr[4];
    LONG result = RETURN_FAIL;
    BOOL isText = FALSE;
    
    if (!fileName || *fileName == '\0') {
        PrintFault(ERROR_OBJECT_NOT_FOUND, "Clipboard: No file specified");
        return RETURN_FAIL;
    }
    
    /* Convert unit number to string */
    SNPrintf(unitStr, sizeof(unitStr), "%lu", unit);
    
    /* Create datatype object from clipboard */
    /* For clipboard source, name is the unit number as string */
    /* Don't restrict GroupID - we want to accept any clipboard content type */
    dtObject = NewDTObject((APTR)unitStr,
                           DTA_SourceType, DTST_CLIPBOARD,
                           TAG_END);
    
    if (!dtObject) {
        LONG errorCode = IoErr();
        if (errorCode == 0) {
            errorCode = ERROR_OBJECT_NOT_FOUND;
        }
        PrintFault(errorCode, "Clipboard: Could not create datatype object from clipboard");
        return RETURN_FAIL;
    }
    
    /* Check if clipboard contains text data */
    /* Get DTA_DataType to determine type */
    {
        struct DataType *dt = NULL;
        if (GetDTAttrs(dtObject, DTA_DataType, (ULONG)&dt, TAG_END) > 0 && dt) {
            /* Check GroupID to determine if it's text */
            if (dt->dtn_Header->dth_GroupID == GID_TEXT) {
                isText = TRUE;
            }
        }
    }
    
    /* For text data, extract plain text from FTXT/CHRS */
    if (isText) {
        DisposeDTObject(dtObject);
        return ExtractTextFromClipboard(fileName, unit);
    }
    
    /* For non-text data, use SaveDTObjectA to save as IFF */
    /* SaveDTObjectA uses DTM_WRITE internally */
    result = SaveDTObjectA(dtObject, NULL, NULL, fileName, DTWM_IFF, FALSE, TAG_END);
    
    if (result) {
        /* Success */
        result = RETURN_OK;
    } else {
        /* Failure - check IoErr() for more details */
        LONG errorCode = IoErr();
        if (errorCode == 0) {
            errorCode = ERROR_WRITE_PROTECTED;
        }
        PrintFault(errorCode, "Clipboard: Failed to paste from clipboard");
        result = RETURN_FAIL;
    }
    
    /* Dispose of the datatype object */
    DisposeDTObject(dtObject);
    
    return result;
}

/* Map FORM ID to clipboard unit number using hash algorithm */
/* This provides a deterministic mapping from any 4-byte FORM ID to a unit (1-255) */
/* Unit 0 is reserved for the 'working copy' clipboard */
/* Uses a polynomial hash (31 is a common prime multiplier) for good distribution */
ULONG FormIDToUnit(ULONG formID)
{
    UBYTE byte0, byte1, byte2, byte3;
    ULONG hash;
    
    /* Extract individual bytes from FORM ID */
    byte0 = (UBYTE)((formID >> 24) & 0xFF);
    byte1 = (UBYTE)((formID >> 16) & 0xFF);
    byte2 = (UBYTE)((formID >> 8) & 0xFF);
    byte3 = (UBYTE)(formID & 0xFF);
    
    /* Use a combination of multiplication and XOR for good distribution */
    /* This hash function provides better distribution than simple XOR alone */
    hash = (ULONG)byte0;
    hash = (hash * 31) ^ (ULONG)byte1;
    hash = (hash * 31) ^ (ULONG)byte2;
    hash = (hash * 31) ^ (ULONG)byte3;
    
    /* Map to 1-255 range (unit 0 is reserved for working copy) */
    return (hash % 255) + 1;
}

/* List all clipboard units with content */
LONG ListClipboards(VOID)
{
    ULONG unit;
    ULONG unitsWithContent = 0;
    LONG result = RETURN_OK;
    
    Printf("Current clipboard contents:\n");
    Printf("Unit  Type    Size    Preview (FTXT only)\n");
    Printf("----  ----    ----    -------------------\n");
    
    for (unit = 0; unit <= 255; unit++) {
        struct ClipboardHandle *clipHandle = NULL;
        struct IOClipReq *ioreq = NULL;
        ULONG cbuff[4];
        ULONG formType = 0;
        ULONG formSize = 0;
        BOOL hasContent = FALSE;
        UBYTE *textPreview = NULL;
        LONG textLen = 0;
        LONG previewAllocSize = 0;
        LONG i;
        struct IFFHandle *iffh = NULL;
        struct ContextNode *cn = NULL;
        
        /* Try to open clipboard unit */
        clipHandle = OpenClipboard(unit);
        if (!clipHandle) {
            /* Unit doesn't exist or can't be opened - skip */
            continue;
        }
        
        /* Get IOClipReq from ClipboardHandle */
        ioreq = &clipHandle->cbh_Req;
        
        /* Initial set-up for Offset, Error, and ClipID */
        ioreq->io_Offset = 0;
        ioreq->io_Error = 0;
        ioreq->io_ClipID = 0;
        
        /* Read 12 bytes to check for FORM header (like CBQueryFTXT in tutorial) */
        ioreq->io_Command = CMD_READ;
        ioreq->io_Data = (STRPTR)cbuff;
        ioreq->io_Length = 12;
        
        DoIO((struct IORequest *)ioreq);
        
        /* Check to see if we have at least 12 bytes */
        if (ioreq->io_Actual == 12 && ioreq->io_Error == 0) {
            /* Check to see if it starts with "FORM" */
            if (cbuff[0] == ID_FORM) {
                formType = cbuff[2];  /* Form type (FTXT, ILBM, etc.) */
                formSize = cbuff[1];  /* Size of FORM chunk */
                hasContent = TRUE;
                
                /* If it's FTXT, try to get text preview using iffparse */
                if (formType == ID_FTXT) {
                    /* Allocate IFF handle for reading CHRS chunk */
                    iffh = AllocIFF();
                    if (iffh) {
                        InitIFFasClip(iffh);
                        iffh->iff_Stream = (ULONG)clipHandle;
                        
                        if (OpenIFF(iffh, IFFF_READ) == 0) {
                            /* Stop on FTXT/CHRS chunks */
                            if (StopChunk(iffh, ID_FTXT, ID_CHRS) == 0) {
                                /* Parse to find CHRS chunk */
                                if (ParseIFF(iffh, IFFPARSE_SCAN) == 0) {
                                    cn = CurrentChunk(iffh);
                                    if (cn && cn->cn_Type == ID_FTXT && cn->cn_ID == ID_CHRS) {
                                        /* Read up to 40 characters for preview */
                                        LONG previewSize = cn->cn_Size;
                                        if (previewSize > 40) {
                                            previewSize = 40;
                                        }
                                        
                                        if (previewSize > 0) {
                                            previewAllocSize = previewSize + 1;
                                            textPreview = AllocMem(previewAllocSize, MEMF_ANY | MEMF_CLEAR);
                                            if (textPreview) {
                                                textLen = ReadChunkBytes(iffh, textPreview, previewSize);
                                                if (textLen > 0) {
                                                    /* Ensure null termination */
                                                    textPreview[textLen] = '\0';
                                                    /* Replace non-printable characters */
                                                    for (i = 0; i < textLen; i++) {
                                                        if (textPreview[i] < 32 || textPreview[i] >= 127) {
                                                            if (textPreview[i] == '\n' || textPreview[i] == '\r' || textPreview[i] == '\t') {
                                                                textPreview[i] = ' ';
                                                            } else {
                                                                textPreview[i] = '.';
                                                            }
                                                        }
                                                    }
                                                } else {
                                                    FreeMem(textPreview, previewAllocSize);
                                                    textPreview = NULL;
                                                    previewAllocSize = 0;
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                            CloseIFF(iffh);
                        }
                        FreeIFF(iffh);
                        iffh = NULL;
                    }
                }
                
                /* Tell clipboard we are done reading (like CBReadDone) */
                {
                    UBYTE buffer[256];
                    ioreq->io_Command = CMD_READ;
                    ioreq->io_Data = (STRPTR)buffer;
                    ioreq->io_Length = 254;
                    while (ioreq->io_Actual) {
                        DoIO((struct IORequest *)ioreq);
                        if (ioreq->io_Error != 0) {
                            break;
                        }
                    }
                }
            } else {
                /* Not FORM - tell clipboard we are done reading */
                {
                    UBYTE buffer[256];
                    ioreq->io_Command = CMD_READ;
                    ioreq->io_Data = (STRPTR)buffer;
                    ioreq->io_Length = 254;
                    while (ioreq->io_Actual) {
                        DoIO((struct IORequest *)ioreq);
                        if (ioreq->io_Error != 0) {
                            break;
                        }
                    }
                }
            }
        }
        
        /* Display information if unit has content */
        if (hasContent) {
            UBYTE typeStr[5];
            ULONG mappedUnit;
            typeStr[0] = (UBYTE)((formType >> 24) & 0xFF);
            typeStr[1] = (UBYTE)((formType >> 16) & 0xFF);
            typeStr[2] = (UBYTE)((formType >> 8) & 0xFF);
            typeStr[3] = (UBYTE)(formType & 0xFF);
            typeStr[4] = '\0';
            
            /* Calculate what unit this FORM type would map to */
            mappedUnit = FormIDToUnit(formType);
            
            Printf("%4lu  %-4s   %6lu", unit, typeStr, formSize);
            
            /* Show mapped unit if different from actual unit */
            if (mappedUnit != unit) {
                Printf("  (maps to %lu)", mappedUnit);
            } else {
                Printf("  [mapped]");
            }
            
            if (textPreview && textLen > 0) {
                Printf("   %.40s", textPreview);
            }
            
            Printf("\n");
            
            unitsWithContent++;
            
            if (textPreview && previewAllocSize > 0) {
                FreeMem(textPreview, previewAllocSize);
                textPreview = NULL;
                previewAllocSize = 0;
            }
        }
        
        /* Cleanup */
        if (clipHandle) {
            CloseClipboard(clipHandle);
            clipHandle = NULL;
        }
    }
    
    if (unitsWithContent == 0) {
        Printf("No clipboard units contain data.\n");
    } else {
        Printf("\nTotal: %lu clipboard unit(s) in use.\n", unitsWithContent);
    }
    
    return result;
}

/* Clear (flush) a clipboard unit */
LONG FlushClipboard(ULONG unit)
{
    struct ClipboardHandle *clipHandle = NULL;
    struct IOClipReq *ioreq = NULL;
    LONG result = RETURN_FAIL;
    
    /* Validate unit number */
    if (unit > 255) {
        PrintFault(ERROR_BAD_NUMBER, "Clipboard");
        return RETURN_FAIL;
    }
    
    /* Open clipboard unit */
    clipHandle = OpenClipboard(unit);
    if (!clipHandle) {
        LONG errorCode = IoErr();
        if (errorCode == 0) {
            errorCode = ERROR_OBJECT_NOT_FOUND;
        }
        PrintFault(errorCode, "Clipboard: Could not open clipboard");
        return RETURN_FAIL;
    }
    
    /* Get IOClipReq from ClipboardHandle */
    ioreq = &clipHandle->cbh_Req;
    
    /* Write 0 bytes to clear the clipboard */
    /* This is the standard way to clear a clipboard unit */
    ioreq->io_Command = CMD_WRITE;
    ioreq->io_Data = NULL;
    ioreq->io_Length = 0;
    ioreq->io_Offset = 0;
    ioreq->io_ClipID = 0;
    ioreq->io_Error = 0;
    
    DoIO((struct IORequest *)ioreq);
    
    if (ioreq->io_Error == 0) {
        /* Send CMD_UPDATE to finalize the clear operation */
        ioreq->io_Command = CMD_UPDATE;
        DoIO((struct IORequest *)ioreq);
        
        if (ioreq->io_Error == 0) {
            result = RETURN_OK;
        } else {
            PrintFault(ioreq->io_Error, "Clipboard: Could not update clipboard");
        }
    } else {
        PrintFault(ioreq->io_Error, "Clipboard: Could not clear clipboard");
    }
    
    CloseClipboard(clipHandle);
    
    return result;
}

