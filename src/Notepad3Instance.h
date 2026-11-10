// encoding: UTF-8
/******************************************************************************
*                                                                             *
* Notepad3                                                                    *
* Notepad3Instance.h                                                          *
*   Per-window instance data and active-instance management.                  *
*   A single Notepad3.EXE process can own several main windows (one per       *
*   document group / split-window), each with its own Scintilla edit,         *
*   toolbar, statusbar and tab-strip.                                         *
*                                                                             *
*******************************************************************************/
#pragma once
#ifndef _NP3_INSTANCE_H_
#define _NP3_INSTANCE_H_

#include "TypeDefs.h"
#include "SciCall.h"

// Note: PathLib.h is intentionally NOT included here.  Pulling it in early
// (before some Windows SDK headers) breaks REFKNOWNFOLDERID parsing inside
// PathLib.h itself.  Anything needing PathLib must include it explicitly.

#ifdef __cplusplus
extern "C" {
#endif

// Max instances per process (reasonable upper bound)
enum { NP3_MAX_INSTANCES = 16 };

// Max tabs per instance
enum { NP3_INSTANCE_MAX_TABS = 64 };

struct Instance;
typedef struct Instance Instance;

// One tab (per-instance copy of TabDoc)
typedef struct InstanceTab {
    sptr_t      pDocPointer;       // Scintilla doc (process-wide pointer)
    HPATHL      hFilePath;
    bool        bModified;
    FILEVARS    fvFileVars;
    cpi_enc_t   iEncoding;
    int         iEOLMode;
} InstanceTab;

struct Instance {
    // Window handles (one set per main window)
    HWND   hwndMain;
    HWND   hwndEdit;
    HWND   hwndEditFrame;
    HWND   hwndToolbar;
    HWND   hwndRebar;
    HWND   hwndStatus;
    HWND   hwndTabControl;
    HWND   hwndFilenameStatic;
    HMENU  hMainMenu;

    // Per-window document state
    HPATHL      hCurrentFile;     // replaces Paths.CurrentFile for this window
    FILEVARS    fvCurFile;
    cpi_enc_t   iEncoding;
    int         iEOLMode;

    // Per-window tab manager
    InstanceTab tabs[NP3_INSTANCE_MAX_TABS];
    int         nTabCount;
    InstanceTab* pActiveTab;

    // Doubly-linked list of all Instances in this process
    Instance*   pNext;
};


// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------
Instance* Instance_Create(HWND hwndMain);
void      Instance_Destroy(Instance* pI);
Instance* Instance_FromHwnd(HWND hwnd);
Instance* Instance_GetActive(void);       // the window currently receiving messages
void      Instance_SetActive(Instance* pI);


// ---------------------------------------------------------------------------
// Convenience accessors
// ---------------------------------------------------------------------------
static __inline Instance* NP3_GetActive(void) { return Instance_GetActive(); }

// Write helpers that also keep Paths.CurrentFile / Globals.fvCurFile in sync
// for code paths that still read those globals directly.
void I_SetCurrentFile(HPATHL h);
void I_UpdateStateToGlobals(void);


#ifdef __cplusplus
}
#endif

#endif // _NP3_INSTANCE_H_
