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
LONG CopyTextToClipboard(STRPTR fileName, ULONG unit, STRPTR textBuffer, ULONG textLength);
LONG PasteFromClipboard(STRPTR fileName, ULONG unit, BOOL forceOverwrite);
LONG ListClipboards(VOID);
ULONG FormIDToUnit(ULONG formID);
LONG FlushClipboard(ULONG unit);
VOID CBReadDone(struct IOClipReq *ioreq);

static const char *verstag = "$VER: Clipboard 1.2 (2.1.2026)\n";
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
    BOOL forceOverwrite = FALSE;
    LONG args[6];
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
    /* Template: COPY/K,PASTE/K,CLIPUNIT/K/N,LIST/S,FLUSH/S,FORCE/S */
    memset(args, 0, sizeof(args));
    
    rda = ReadArgs("FROM=COPY/K,TO=PASTE/K,CLIPUNIT/K/N,LIST/S,FLUSH/S,FORCE/S", args, NULL);
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
    if (args[5]) forceOverwrite = TRUE;
    
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
            pasteResult = PasteFromClipboard(pasteFile, unit, forceOverwrite);
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
    Printf("       Clipboard FLUSH\n");
    Printf("\n");
    Printf("Options:\n");
    Printf("  COPY=<file>    Copy file to clipboard (converts to IFF via datatypes)\n");
    Printf("  PASTE=<file>   Paste clipboard to file (extracts text from FTXT)\n");
    Printf("  CLIPUNIT=<n>   Clipboard unit number (0-255, default 0)\n");
    Printf("  LIST           List all clipboard units with content (0-255)\n");
    Printf("  FLUSH          Clear the specified clipboard unit\n");
    Printf("  FORCE          Overwrite existing files when pasting\n");
    Printf("\n");
    Printf("Note: COPY and PASTE can be used together. COPY is always performed first, then PASTE. This can be used to convert files to IFF format.\n");
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
    UBYTE tempFileName[31];  /* Max 30 chars + null terminator */
    STRPTR tempFile;
    BPTR tempFileHandle;
    LONG bytesRead;
    LONG bytesWritten;
    LONG result = RETURN_FAIL;
    ULONG *methods;
    BOOL supportsWrite = FALSE;
    ULONG uniqueID;
    
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
    
    /* Check if this is a text file - handle it specially using direct FTXT format */
    {
        struct DataType *dt = NULL;
        STRPTR textBuffer = NULL;
        ULONG textLength = 0;
        BOOL isText = FALSE;
        
        if (GetDTAttrs(dtObject, DTA_DataType, (ULONG)&dt, TAG_END) > 0 && dt) {
            if (dt->dtn_Header->dth_GroupID == GID_TEXT) {
                isText = TRUE;
            }
        }
        
        if (isText) {
            /* Get text content from datatype object */
            if (GetDTAttrs(dtObject, TDTA_Buffer, &textBuffer, TDTA_BufferLen, &textLength, TAG_END) >= 2) {
                if (textBuffer && textLength > 0) {
                    /* Copy text data to our own buffer before disposing object */
                    STRPTR textCopy = NULL;
                    LONG copyResult = RETURN_FAIL;
                    ULONG i;
                    
                    textCopy = (STRPTR)AllocMem(textLength, MEMF_CLEAR);
                    if (textCopy) {
                        /* Copy text data */
                        for (i = 0; i < textLength; i++) {
                            textCopy[i] = textBuffer[i];
                        }
                        
                        /* Now dispose the object - textCopy is independent */
                        DisposeDTObject(dtObject);
                        
                        /* Copy text to clipboard */
                        copyResult = CopyTextToClipboard(fileName, unit, textCopy, textLength);
                        
                        /* Free our copy */
                        FreeMem(textCopy, textLength);
                        
                        return copyResult;
                    } else {
                        /* Could not allocate memory - fall through to normal handling */
                    }
                }
            }
            /* If we can't get text buffer, fall through to normal handling */
        }
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
    
    /* Generate unique temporary filename using GetUniqueID */
    /* Format: "T:clipboard_XXXXXXXX.iff" where X is hex unique ID */
    uniqueID = GetUniqueID();
    SNPrintf(tempFileName, sizeof(tempFileName), "T:clip%08lX", uniqueID);
    tempFile = tempFileName;
    
    if (!SaveDTObjectA(dtObject, NULL, NULL, tempFile, DTWM_IFF, FALSE, TAG_END)) {
        LONG errorCode = IoErr();
        PrintFault(errorCode ? errorCode : ERROR_WRITE_PROTECTED, "Clipboard: Failed to save object to temporary file");
        DisposeDTObject(dtObject);
        return RETURN_FAIL;
    }
    DisposeDTObject(dtObject);
    dtObject = NULL;
    
    /* Temp file is ready - SaveDTObjectA with DTWM_IFF should have created a valid IFF file */
    /* We can write it directly to the clipboard using CMD_WRITE */
    
    clipHandle = OpenClipboard(unit);
    if (!clipHandle) {
        LONG errorCode = IoErr();
        if (errorCode == 0) {
            errorCode = ERROR_OBJECT_NOT_FOUND;
        }
        DeleteFile(tempFile);
        PrintFault(errorCode, "Clipboard: Could not open clipboard");
        return RETURN_FAIL;
    }
    
    /* Open temp file for reading */
    tempFileHandle = Open(tempFile, MODE_OLDFILE);
    if (!tempFileHandle) {
        LONG errorCode = IoErr();
        DeleteFile(tempFile);
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
                    ioreq->io_Error = 0;
                    ioreq->io_Command = CMD_WRITE;
                    ioreq->io_Data = fileBuffer;
                    ioreq->io_Length = fileSize;
                    
                    DoIO((struct IORequest *)ioreq);
                    
                    if (ioreq->io_Error == 0 && ioreq->io_Actual == fileSize) {
                        /* Send CMD_UPDATE to finalize */
                        ioreq->io_Command = CMD_UPDATE;
                        ioreq->io_ClipID = ioreq->io_ClipID;  /* Use the ClipID from write */
                        ioreq->io_Error = 0;
                        DoIO((struct IORequest *)ioreq);
                        
                        if (ioreq->io_Error == 0) {
                            result = RETURN_OK;
                        } else {
                            PrintFault(ioreq->io_Error, "Clipboard: Could not update clipboard");
                        }
                    } else {
                        LONG errorCode = ioreq->io_Error;
                        if (errorCode == 0) {
                            errorCode = ERROR_WRITE_PROTECTED;
                        }
                        PrintFault(errorCode, "Clipboard: Could not write to clipboard");
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
    
    /* Delete temporary file */
    DeleteFile(tempFile);
    
    CloseClipboard(clipHandle);
    
    return result;
}

/* Signal to clipboard that we are done reading */
/* This drains the clipboard by reading until io_Actual is zero */
VOID CBReadDone(struct IOClipReq *ioreq)
{
    UBYTE buffer[256];
    
    if (!ioreq) {
        return;
    }
    
    ioreq->io_Command = CMD_READ;
    ioreq->io_Data = (STRPTR)buffer;
    ioreq->io_Length = 254;
    
    while (TRUE) {
        DoIO((struct IORequest *)ioreq);
        if (ioreq->io_Actual == 0 || ioreq->io_Error != 0) {
            break;
        }
    }
}

/* Copy text directly to clipboard using FTXT format */
/* Uses IFFParse library functions for proper IFF structure handling and padding */
LONG CopyTextToClipboard(STRPTR fileName, ULONG unit, STRPTR textBuffer, ULONG textLength)
{
    struct IFFHandle *iffh = NULL;
    struct ClipboardHandle *clipHandle = NULL;
    LONG result = RETURN_FAIL;
    LONG error = 0;
    
    if (!textBuffer || textLength == 0) {
        PrintFault(ERROR_OBJECT_NOT_FOUND, "Clipboard: No text data to copy");
        return RETURN_FAIL;
    }
    
    /* Allocate IFF handle */
    if (!(iffh = AllocIFF())) {
        PrintFault(ERROR_NO_FREE_STORE, "Clipboard: Could not allocate IFF handle");
        return RETURN_FAIL;
    }
    
    /* Open clipboard */
    clipHandle = OpenClipboard(unit);
    if (!clipHandle) {
        LONG errorCode = IoErr();
        if (errorCode == 0) {
            errorCode = ERROR_OBJECT_NOT_FOUND;
        }
        PrintFault(errorCode, "Clipboard: Could not open clipboard");
        FreeIFF(iffh);
        return RETURN_FAIL;
    }
    
    /* Initialize IFF for clipboard writing */
    InitIFFasClip(iffh);
    iffh->iff_Stream = (ULONG)clipHandle;
    
    /* Open IFF for writing */
    if ((error = OpenIFF(iffh, IFFF_WRITE))) {
        PrintFault(error, "Clipboard: Could not open clipboard for writing");
        CloseClipboard(clipHandle);
        FreeIFF(iffh);
        return RETURN_FAIL;
    }
    
    /* Push FORM FTXT chunk */
    if ((error = PushChunk(iffh, ID_FTXT, ID_FORM, IFFSIZE_UNKNOWN))) {
        PrintFault(error, "Clipboard: Could not create FORM FTXT");
        CloseIFF(iffh);
        CloseClipboard(clipHandle);
        FreeIFF(iffh);
        return RETURN_FAIL;
    }
    
    /* Push CHRS chunk - IFFParse will handle padding automatically */
    if ((error = PushChunk(iffh, 0, ID_CHRS, textLength))) {
        PrintFault(error, "Clipboard: Could not create CHRS chunk");
        PopChunk(iffh);
        CloseIFF(iffh);
        CloseClipboard(clipHandle);
        FreeIFF(iffh);
        return RETURN_FAIL;
    }
    
    /* Write text data - IFFParse handles padding automatically for odd-length chunks */
    if (WriteChunkBytes(iffh, textBuffer, textLength) != textLength) {
        error = IoErr();
        if (error == 0) {
            error = IFFERR_WRITE;
        }
        PrintFault(error, "Clipboard: Could not write text data");
        PopChunk(iffh);
        PopChunk(iffh);
        CloseIFF(iffh);
        CloseClipboard(clipHandle);
        FreeIFF(iffh);
        return RETURN_FAIL;
    }
    
    /* Pop CHRS chunk */
    if ((error = PopChunk(iffh))) {
        PrintFault(error, "Clipboard: Error closing CHRS chunk");
        PopChunk(iffh);
        CloseIFF(iffh);
        CloseClipboard(clipHandle);
        FreeIFF(iffh);
        return RETURN_FAIL;
    }
    
    /* Pop FORM chunk */
    if ((error = PopChunk(iffh))) {
        PrintFault(error, "Clipboard: Error closing FORM chunk");
        CloseIFF(iffh);
        CloseClipboard(clipHandle);
        FreeIFF(iffh);
        return RETURN_FAIL;
    }
    
    /* Close IFF - this sends CMD_UPDATE automatically */
    CloseIFF(iffh);
    
    /* Cleanup */
    CloseClipboard(clipHandle);
    FreeIFF(iffh);
    
    result = RETURN_OK;
    return result;
}

/* Extract text from FTXT/CHRS chunks and write to file */
/* Handles multiple CHRS chunks properly */
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
    LONG error = 0;
    
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
    if ((error = OpenIFF(iffh, IFFF_READ))) {
        PrintFault(error, "Clipboard: Could not open clipboard for reading");
        CloseClipboard(clipHandle);
        FreeIFF(iffh);
        return RETURN_FAIL;
    }
    
    /* Stop on FTXT/CHRS chunks */
    if ((error = StopChunk(iffh, ID_FTXT, ID_CHRS))) {
        PrintFault(error, "Clipboard: Could not register IFF chunk");
        CloseIFF(iffh);
        CloseClipboard(clipHandle);
        FreeIFF(iffh);
        return RETURN_FAIL;
    }
    
    /* Open output file */
    if (fileName && *fileName != '\0') {
        outputFile = Open(fileName, MODE_NEWFILE);
        if (!outputFile) {
            LONG errorCode = IoErr();
            CloseIFF(iffh);
            CloseClipboard(clipHandle);
            FreeIFF(iffh);
            PrintFault(errorCode, "Clipboard: Could not open output file");
            return RETURN_FAIL;
        }
    } else {
        /* No filename specified, use stdout */
        outputFile = Output();
    }
    
    /* Allocate buffer for reading */
    if (!(buffer = AllocMem(bufferSize, MEMF_ANY))) {
        PrintFault(ERROR_NO_FREE_STORE, "Clipboard: Could not allocate memory");
        if (outputFile && outputFile != Output()) {
            Close(outputFile);
        }
        CloseIFF(iffh);
        CloseClipboard(clipHandle);
        FreeIFF(iffh);
        return RETURN_FAIL;
    }
    
    /* Parse IFF and extract all CHRS chunks */
    while (TRUE) {
        error = ParseIFF(iffh, IFFPARSE_SCAN);
        
        if (error == IFFERR_EOC) {
            /* End of context, continue to next */
            continue;
        } else if (error) {
            /* Error or end of file */
            if (error != IFFERR_EOF) {
                PrintFault(error, "Clipboard: Error parsing clipboard");
                result = RETURN_FAIL;
            } else {
                result = RETURN_OK;
            }
            break;
        }
        
        /* Get current chunk */
        cn = CurrentChunk(iffh);
        if (cn && cn->cn_Type == ID_FTXT && cn->cn_ID == ID_CHRS) {
            /* Read CHRS chunk data in chunks */
            while ((len = ReadChunkBytes(iffh, buffer, bufferSize)) > 0) {
                if (Write(outputFile, buffer, len) != len) {
                    PrintFault(IoErr(), "Clipboard: Could not write to file");
                    result = RETURN_FAIL;
                    break;
                }
            }
            
            if (len < 0) {
                PrintFault(len, "Clipboard: Error reading chunk");
                result = RETURN_FAIL;
                break;
            }
        }
    }
    
    /* Cleanup */
    FreeMem(buffer, bufferSize);
    if (outputFile && outputFile != Output()) {
        Close(outputFile);
    }
    CloseIFF(iffh);
    CloseClipboard(clipHandle);
    FreeIFF(iffh);
    
    return result;
}

/* Paste from clipboard to file using datatypes API */
/* For text (FTXT), extracts plain ASCII text */
/* For other types, saves as IFF format using DataTypes */
LONG PasteFromClipboard(STRPTR fileName, ULONG unit, BOOL forceOverwrite)
{
    struct ClipboardHandle *clipHandle = NULL;
    struct IOClipReq *ioreq = NULL;
    BPTR outputFile = NULL;
    BPTR testFile = NULL;
    struct {
        ULONG form;
        ULONG length;
        ULONG formtype;
    } iffHeader;
    LONG result = RETURN_FAIL;
    LONG totalWritten = 0;
    BOOL isText = FALSE;
    
    if (!fileName || *fileName == '\0') {
        PrintFault(ERROR_OBJECT_NOT_FOUND, "Clipboard: No file specified");
        return RETURN_FAIL;
    }
    
    /* Check if output file already exists */
    if (!forceOverwrite) {
        testFile = Open(fileName, MODE_OLDFILE);
        if (testFile) {
            Close(testFile);
            PrintFault(ERROR_OBJECT_EXISTS, "Clipboard: File already exists (use FORCE to overwrite)");
            return RETURN_FAIL;
        }
    }
    
    /* Open clipboard */
    clipHandle = OpenClipboard(unit);
    if (!clipHandle) {
        LONG errorCode = IoErr();
        if (errorCode == 0) {
            errorCode = ERROR_OBJECT_NOT_FOUND;
        }
        PrintFault(errorCode, "Clipboard: Could not open clipboard");
        return RETURN_FAIL;
    }
    
    ioreq = &clipHandle->cbh_Req;
    
    /* Read FORM header (12 bytes) to check if it's FTXT */
    ioreq->io_Offset = 0;
    ioreq->io_ClipID = 0;
    ioreq->io_Command = CMD_READ;
    ioreq->io_Length = 12;
    ioreq->io_Data = (APTR)&iffHeader;
    DoIO((struct IORequest *)ioreq);
    
    if (ioreq->io_Actual >= 8 && 
        iffHeader.form == ID_FORM && 
        iffHeader.formtype == MAKE_ID('F','T','X','T')) {
        /* It's FTXT - extract plain ASCII text */
        isText = TRUE;
    }
    
    if (isText) {
        /* Use IFFParse to extract text from FTXT/CHRS chunks */
        struct IFFHandle *iffh = NULL;
        struct ContextNode *cn = NULL;
        UBYTE *readBuffer = NULL;
        LONG bufferSize = 4096;
        LONG bytesRead = 0;
        LONG error = 0;
        
        /* Drain clipboard to release it for IFF parsing */
        CBReadDone(ioreq);
        
        /* Allocate IFF handle */
        iffh = AllocIFF();
        if (!iffh) {
            PrintFault(ERROR_NO_FREE_STORE, "Clipboard: Could not allocate IFF handle");
            CloseClipboard(clipHandle);
            return RETURN_FAIL;
        }
        
        /* Initialize IFF for clipboard reading */
        InitIFFasClip(iffh);
        iffh->iff_Stream = (ULONG)clipHandle;
        
        /* Open IFF for reading */
        if ((error = OpenIFF(iffh, IFFF_READ))) {
            PrintFault(error, "Clipboard: Could not open clipboard for reading");
            FreeIFF(iffh);
            CloseClipboard(clipHandle);
            return RETURN_FAIL;
        }
        
        /* Stop on FTXT/CHRS chunks */
        if ((error = StopChunk(iffh, ID_FTXT, ID_CHRS))) {
            PrintFault(error, "Clipboard: Could not register IFF chunk");
            CloseIFF(iffh);
            FreeIFF(iffh);
            CloseClipboard(clipHandle);
            return RETURN_FAIL;
        }
        
        /* Open output file */
        outputFile = Open(fileName, MODE_NEWFILE);
        if (!outputFile) {
            LONG errorCode = IoErr();
            CloseIFF(iffh);
            FreeIFF(iffh);
            CloseClipboard(clipHandle);
            PrintFault(errorCode, "Clipboard: Could not open output file");
            return RETURN_FAIL;
        }
        
        /* Allocate buffer for reading */
        readBuffer = AllocMem(bufferSize, MEMF_ANY);
        if (!readBuffer) {
            PrintFault(ERROR_NO_FREE_STORE, "Clipboard: Could not allocate buffer");
            Close(outputFile);
            CloseIFF(iffh);
            FreeIFF(iffh);
            CloseClipboard(clipHandle);
            return RETURN_FAIL;
        }
        
        /* Parse IFF and extract all CHRS chunks */
        while (TRUE) {
            error = ParseIFF(iffh, IFFPARSE_SCAN);
            
            if (error == IFFERR_EOC) {
                /* End of context, continue to next */
                continue;
            } else if (error) {
                /* Error or end of file */
                if (error != IFFERR_EOF) {
                    PrintFault(error, "Clipboard: Error parsing clipboard");
                    result = RETURN_FAIL;
                } else {
                    result = RETURN_OK;
                }
                break;
            }
            
            /* Get current chunk */
            cn = CurrentChunk(iffh);
            if (cn && cn->cn_Type == ID_FTXT && cn->cn_ID == ID_CHRS) {
                /* Read CHRS chunk data */
                while ((bytesRead = ReadChunkBytes(iffh, readBuffer, bufferSize)) > 0) {
                    if (Write(outputFile, readBuffer, bytesRead) != bytesRead) {
                        PrintFault(IoErr(), "Clipboard: Error writing file");
                        result = RETURN_FAIL;
                        break;
                    }
                    totalWritten += bytesRead;
                }
                
                if (bytesRead < 0) {
                    PrintFault(bytesRead, "Clipboard: Error reading chunk");
                    result = RETURN_FAIL;
                    break;
                }
            }
        }
        
        /* Cleanup */
        FreeMem(readBuffer, bufferSize);
        Close(outputFile);
        CloseIFF(iffh);
        FreeIFF(iffh);
        CloseClipboard(clipHandle);
    } else {
        /* Not text - use datatypes API to save as IFF */
        struct IFFHandle *iffh = NULL;
        Object *dtObject = NULL;
        
        /* Drain clipboard to release it for IFF parsing */
        CBReadDone(ioreq);
        
        /* Allocate IFF handle */
        iffh = AllocIFF();
        if (!iffh) {
            PrintFault(ERROR_NO_FREE_STORE, "Clipboard: Could not allocate IFF handle");
            CloseClipboard(clipHandle);
            return RETURN_FAIL;
        }
        
        /* Initialize IFF for clipboard reading */
        InitIFFasClip(iffh);
        iffh->iff_Stream = (ULONG)clipHandle;
        
        /* Open IFF for reading */
        if (OpenIFF(iffh, IFFF_READ)) {
            PrintFault(ERROR_OBJECT_NOT_FOUND, "Clipboard: Could not open clipboard for reading");
            FreeIFF(iffh);
            CloseClipboard(clipHandle);
            return RETURN_FAIL;
        }
        
        /* Create datatype object from clipboard using IFFHandle */
        dtObject = NewDTObject(NULL,
                               DTA_SourceType, DTST_CLIPBOARD,
                               DTA_Handle, (ULONG)iffh,
                               TAG_END);
        
        if (!dtObject) {
            LONG errorCode = IoErr();
            if (errorCode == 0) {
                errorCode = ERROR_OBJECT_NOT_FOUND;
            }
            PrintFault(errorCode, "Clipboard: Could not create datatype object from clipboard");
            CloseIFF(iffh);
            FreeIFF(iffh);
            CloseClipboard(clipHandle);
            return RETURN_FAIL;
        }
        
        /* Use SaveDTObjectA to save clipboard content to file as IFF */
        result = SaveDTObjectA(dtObject, NULL, NULL, fileName, DTWM_IFF, FALSE, TAG_END);
        
        if (result) {
            result = RETURN_OK;
        } else {
            LONG errorCode = IoErr();
            if (errorCode == 0) {
                errorCode = ERROR_WRITE_PROTECTED;
            }
            PrintFault(errorCode, "Clipboard: Failed to paste from clipboard");
            result = RETURN_FAIL;
        }
        
        /* Dispose of the datatype object - it will cleanup IFF and clipboard handles */
        DisposeDTObject(dtObject);
    }
    
    return result;
}

/* Map FORM ID to clipboard unit number using hash algorithm */
/* This provides a deterministic mapping from any 4-byte FORM ID to a unit (1-255) */
/* Unit 0 is reserved for the 'working copy' clipboard */
/* Uses a two-stage hash algorithm optimized for IFF FORM types (A-Z, space, 0-9) */
/* This algorithm achieves 94% collision-free mapping (92 unique units out of 98 known FORM types) */
ULONG FormIDToUnit(ULONG formID)
{
    UBYTE byte0, byte1, byte2, byte3;
    ULONG hash;
    
    /* Extract individual bytes from FORM ID */
    byte0 = (UBYTE)((formID >> 24) & 0xFF);
    byte1 = (UBYTE)((formID >> 16) & 0xFF);
    byte2 = (UBYTE)((formID >> 8) & 0xFF);
    byte3 = (UBYTE)(formID & 0xFF);
    
    /* Two-stage hash algorithm for optimal distribution */
    /* First stage: combine bytes using XOR with bit shifts */
    hash = byte0 ^ (byte1 << 8) ^ (byte2 << 16) ^ (byte3 << 24);
    /* Second stage: multiply by prime and mix high bits back */
    hash = (hash * 209UL) ^ (hash >> 16);
    
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
        
        /* Read 12 bytes to check for FORM header */
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
                
                /* Tell clipboard we are done reading */
                CBReadDone(ioreq);
            } else {
                /* Not FORM - tell clipboard we are done reading */
                CBReadDone(ioreq);
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

