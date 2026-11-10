// encoding: UTF-8
/******************************************************************************
*                                                                             *
* Notepad3                                                                    *
* TabManager.h                                                                *
*   Multi-Tab Document Manager -- per-Instance state.                         *
*   Uses Scintilla's native multi-document API:                              *
*     SCI_CREATEDOCUMENT / SCI_SETDOCPOINTER / SCI_RELEASEDOCUMENT           *
*                                                                             *
*******************************************************************************/
#pragma once
#ifndef _NP3_TABMANAGER_H_
#define _NP3_TABMANAGER_H_

#include "TypeDefs.h"
#include "SciCall.h"

#ifdef __cplusplus
extern "C" {
#endif

// TabDoc -- legacy public shim.  Per-Instance state lives in InstanceTab
// (defined in Notepad3Instance.h), but Notepad3.c still expects a TabDoc*
// pointer with the original member layout, so we keep a public definition
// that matches InstanceTab field-for-field.
struct TabDoc {
    sptr_t      pDocPointer;
    HPATHL      hFilePath;
    bool        bModified;
    FILEVARS    fvFileVars;
    cpi_enc_t   iEncoding;
    int         iEOLMode;
};
typedef struct TabDoc TabDoc;

// Legacy global (kept because parts of Notepad3.c still read it)
extern HWND g_hwndTabControl;

// ---------------------------------------------------------------------------
// Lifecycle -- pass the main window HWND so the Instance can be looked up.
// ---------------------------------------------------------------------------
void TabManager_Init(HWND hwndMain, HINSTANCE hInstance);
void TabManager_Cleanup(HWND hwndMain);
void TabManager_RegisterInitialTab(HWND hwndMain);
void TabManager_InstallDragSupport(void);     // hook drag subclass onto current Instance's tab control
void TabManager_AttachDragSubclass(void);


// ---------------------------------------------------------------------------
// Tab operations
// ---------------------------------------------------------------------------
TabDoc* TabManager_AddNewTab(HWND hwndMain);
bool    TabManager_CloseTab(int nIndex);
void    TabManager_CloseAllOtherTabs(int nKeepIndex);
void    TabManager_CloseAllTabs(HWND hwndMain);
void    TabManager_SwitchToTab(HWND hwndMain, int nIndex);
void    TabManager_MoveTab(int nFrom, int nTo);   // internal helper used by drag-reorder


// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------
TabDoc* TabManager_GetTab(int nIndex);
int     TabManager_GetActiveTabIndex(void);
int     TabManager_GetCount(void);
bool    TabManager_IsActiveTabModified(void);


// ---------------------------------------------------------------------------
// Per-tab-state updates -- called from Notepad3's command handlers
// ---------------------------------------------------------------------------
void TabManager_UpdateCurrentTabModified(bool bModified);
void TabManager_SetCurrentTabPath(HPATHL hPath);


// ---------------------------------------------------------------------------
// WM_NOTIFY dispatcher (IDC_TAB case)
// ---------------------------------------------------------------------------
LRESULT TabManager_HandleNotify(HWND hwndMain, WPARAM wParam, LPARAM lParam);

#define IDC_TAB (0xFB09)


#ifdef __cplusplus
}
#endif

#endif // _NP3_TABMANAGER_H_
