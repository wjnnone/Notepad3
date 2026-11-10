// encoding: UTF-8
/******************************************************************************
*                                                                             *
*                                                                             *
* Notepad3                                                                    *
*                                                                             *
* TabManager.h                                                                *
*   Multi-Tab Document Manager                                                *
*   Uses Scintilla's native multi-document API:                              *
*     SCI_CREATEDOCUMENT / SCI_SETDOCPOINTER / SCI_RELEASEDOCUMENT           *
*                                                                             *
*                                                  (c) Rizonesoft 2008-2026   *
*                                                    https://rizonesoft.com   *
*                                                                             *
*                                                                             *
*******************************************************************************/
#pragma once
#ifndef _NP3_TABMANAGER_H_
#define _NP3_TABMANAGER_H_

#include "TypeDefs.h"
#include "SciCall.h"

#define MAX_TABS  64

struct TabDoc {
    sptr_t      pDocPointer;       // Scintilla document pointer
    HPATHL      hFilePath;         // file path (may be NULL for untitled)
    bool        bModified;
    bool        bReadOnly;
    WCHAR       wchTabText[256];
    FILEVARS    fvFileVars;
    cpi_enc_t   iEncoding;
    int         iEOLMode;
};
typedef struct TabDoc TabDoc;

extern HWND   g_hwndTabControl;
extern TabDoc* g_pActiveTab;
extern int    g_nTabCount;

void    TabManager_Init(HWND hwndMain, HINSTANCE hInstance);
void    TabManager_Cleanup(HWND hwndMain);
void    TabManager_RegisterInitialTab(HWND hwndMain);
void    TabManager_InstallDragSupport(void);

TabDoc* TabManager_AddNewTab(HWND hwndMain);
bool    TabManager_CloseTab(int nIndex);
void    TabManager_CloseAllOtherTabs(int nKeepIndex);
void    TabManager_CloseAllTabs(HWND hwndMain);
void    TabManager_SwitchToTab(HWND hwndMain, int nIndex);
void    TabManager_MoveTab(int nFrom, int nTo);

int     TabManager_GetActiveTabIndex(void);
TabDoc* TabManager_GetTab(int nIndex);

void    TabManager_UpdateTabText(int nIndex);
void    TabManager_UpdateActiveTabText(void);

LRESULT TabManager_HandleNotify(HWND hwndMain, WPARAM wParam, LPARAM lParam);

#define IDC_TAB (0xFB09)

#endif // _NP3_TABMANAGER_H_
