// encoding: UTF-8
/******************************************************************************
*                                                                             *
*                                                                             *
* Notepad3                                                                    *
*                                                                             *
* TabManager.c                                                                *
*   Multi-Tab Document Manager Implementation                                 *
*   Uses Scintilla's native multi-document API:                              *
*     SCI_CREATEDOCUMENT / SCI_SETDOCPOINTER / SCI_RELEASEDOCUMENT           *
*                                                                             *
*                                                  (c) Rizonesoft 2008-2026   *
*                                                    https://rizonesoft.com   *
*                                                                             *
*                                                                             *
*******************************************************************************/

#include "TypeDefs.h"
#include "Notepad3.h"
#include "TabManager.h"
#include "SciCall.h"
#include "Helpers.h"
#include "PathLib.h"
#include "Encoding.h"
#include "Dialogs.h"

#include <commctrl.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <strsafe.h>
#include <stdio.h>

#ifdef D_NP3_WIN10_DARK_MODE
#include "DarkMode/DarkMode.h"
#endif

//=============================================================================
// Globals
//=============================================================================

HWND      g_hwndTabControl = NULL;
TabDoc*   g_pActiveTab = NULL;
int       g_nTabCount = 0;
TabDoc    g_Tabs[MAX_TABS];

// Drag state
static bool  s_bDragging = false;
static int   s_nDragSource = -1;
static POINT s_ptDragStart = { 0, 0 };
static bool  s_bDragOut = false;    // cursor left tab band -> spawn new window

//=============================================================================
// Internal Helpers
//=============================================================================

static void _BuildTabText(TabDoc* pTab, int nUntitledIndex)
{
    if (!pTab) return;

    if (Path_IsNotEmpty(pTab->hFilePath)) {
        const WCHAR* pName = Path_FindFileName(pTab->hFilePath);
        StringCchPrintf(pTab->wchTabText, COUNTOF(pTab->wchTabText),
                        L"%s%s", pTab->bModified ? L"* " : L"", pName ? pName : L"Untitled");
    } else {
        StringCchPrintf(pTab->wchTabText, COUNTOF(pTab->wchTabText),
                        L"%sUntitled %i", pTab->bModified ? L"* " : L"", nUntitledIndex);
    }
}

static void _RefreshTabControlItem(int nIndex)
{
    if (!g_hwndTabControl || nIndex < 0 || nIndex >= g_nTabCount) return;

    TCITEM tci = { 0 };
    tci.mask = TCIF_TEXT;
    tci.pszText = g_Tabs[nIndex].wchTabText;
    tci.cchTextMax = (int)COUNTOF(g_Tabs[nIndex].wchTabText);
    TabCtrl_SetItem(g_hwndTabControl, nIndex, &tci);
}

// Re-run main window layout so the tab bar is shown/hidden correctly
static void _RelayoutMainWindow(HWND hwndMain)
{
    if (!hwndMain) return;
    RECT rc;
    if (!GetClientRect(hwndMain, &rc)) return;
    SendMessage(hwndMain, WM_SIZE, (WPARAM)SIZE_RESTORED,
                MAKELONG((WORD)(rc.right - rc.left), (WORD)(rc.bottom - rc.top)));
}

static void _SyncLParam(int nIndex)
{
    if (!g_hwndTabControl || nIndex < 0 || nIndex >= g_nTabCount) return;
    TCITEM tci = { 0 };
    tci.mask = TCIF_PARAM;
    tci.lParam = (LPARAM)&g_Tabs[nIndex];
    TabCtrl_SetItem(g_hwndTabControl, nIndex, &tci);
}

// Save active tab's editor state (file vars + encoding) before switch
static void _SaveActiveTabState(void)
{
    if (g_pActiveTab) {
        g_pActiveTab->fvFileVars = Globals.fvCurFile;
        g_pActiveTab->iEncoding = Encoding_Current(-1);
    }
}

// Spawn a new Notepad3 process on the file (used by drag-out)
static void _SpawnNewWindowForTab(int nIndex)
{
    if (nIndex < 0 || nIndex >= g_nTabCount) return;
    TabDoc* pTab = &g_Tabs[nIndex];

    if (!Path_IsNotEmpty(pTab->hFilePath)) return;  // untitled -> skip

    WCHAR wchModule[MAX_PATH] = { 0 };
    if (!GetModuleFileName(NULL, wchModule, COUNTOF(wchModule))) return;

    WCHAR wchCmd[MAX_PATH * 2] = { 0 };
    StringCchPrintf(wchCmd, COUNTOF(wchCmd), L"\"%s\" \"%s\"",
                    wchModule, Path_Get(pTab->hFilePath));

    STARTUPINFO si = { sizeof(si) };
    PROCESS_INFORMATION pi = { 0 };
    if (CreateProcess(NULL, wchCmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
}

//=============================================================================
// Initialization / Cleanup
//=============================================================================

void TabManager_Init(HWND hwndMain, HINSTANCE hInstance)
{
    g_hwndTabControl = CreateWindowEx(
        0,
        WC_TABCONTROL,
        NULL,
        (WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN | TCS_SINGLELINE | TCS_RIGHTJUSTIFY | TCS_TABS) & ~WS_VISIBLE,
        0, 0, 0, 0,
        hwndMain,
        (HMENU)(INT_PTR)IDC_TAB,
        hInstance,
        NULL);

    if (!g_hwndTabControl) return;

    InitWindowCommon(g_hwndTabControl, true);

#ifdef D_NP3_WIN10_DARK_MODE
    if (IsDarkModeSupported()) {
        AllowDarkModeForWindowEx(g_hwndTabControl, CheckDarkModeEnabled());
    }
#endif

    for (int i = 0; i < MAX_TABS; i++) {
        g_Tabs[i].pDocPointer = 0;
        g_Tabs[i].hFilePath = NULL;
        g_Tabs[i].bModified = false;
        g_Tabs[i].bReadOnly = false;
        g_Tabs[i].wchTabText[0] = L'\0';
        memset(&g_Tabs[i].fvFileVars, 0, sizeof(FILEVARS));
        g_Tabs[i].iEncoding = Settings.DefaultEncoding;
        g_Tabs[i].iEOLMode = Settings.DefaultEOLMode;
    }

    g_pActiveTab = NULL;
    g_nTabCount = 0;
    s_bDragging = false;
    s_nDragSource = -1;
    s_bDragOut = false;
}

void TabManager_Cleanup(HWND hwndMain)
{
    UNREFERENCED_PARAMETER(hwndMain);

    for (int i = 0; i < g_nTabCount; i++) {
        if (g_Tabs[i].pDocPointer && Globals.hwndEdit) {
            if (g_pActiveTab != &g_Tabs[i]) {
                SendMessage(Globals.hwndEdit, SCI_SETDOCPOINTER, 0, (LPARAM)g_Tabs[i].pDocPointer);
            }
            SciCall_ReleaseDocument(g_Tabs[i].pDocPointer);
        }
        if (g_Tabs[i].hFilePath) {
            Path_Release(g_Tabs[i].hFilePath);
        }
    }

    g_nTabCount = 0;
    g_pActiveTab = NULL;

    if (g_hwndTabControl) {
        DestroyWindow(g_hwndTabControl);
        g_hwndTabControl = NULL;
    }
}

void TabManager_RegisterInitialTab(HWND hwndMain)
{
    UNREFERENCED_PARAMETER(hwndMain);

    if (!Globals.hwndEdit || !g_hwndTabControl) return;

    sptr_t pDoc = (sptr_t)SendMessage(Globals.hwndEdit, SCI_GETDOCPOINTER, 0, 0);
    if (!pDoc) return;

    // Keep our own reference: Scintilla releases its ref when switching away,
    // which would destroy the document otherwise.
    SciCall_AddRefDocument(pDoc);

    int nIndex = g_nTabCount;
    TabDoc* pTab = &g_Tabs[nIndex];

    pTab->pDocPointer = pDoc;
    pTab->bModified = false;
    pTab->bReadOnly = false;
    pTab->fvFileVars = Globals.fvCurFile;
    pTab->iEncoding = Encoding_Current(-1);
    pTab->iEOLMode = Globals.fvCurFile.chMode[0] ? Globals.fvCurFile.chMode[0] : Settings.DefaultEOLMode;

    if (Path_IsNotEmpty(Paths.CurrentFile)) {
        pTab->hFilePath = Path_Copy(Paths.CurrentFile);
    } else {
        pTab->hFilePath = NULL;
    }

    _BuildTabText(pTab, nIndex + 1);

    TCITEM tci = { 0 };
    tci.mask = TCIF_TEXT | TCIF_PARAM;
    tci.pszText = pTab->wchTabText;
    tci.cchTextMax = (int)COUNTOF(pTab->wchTabText);
    tci.lParam = (LPARAM)pTab;
    TabCtrl_InsertItem(g_hwndTabControl, nIndex, &tci);

    g_nTabCount++;
    g_pActiveTab = pTab;

    ShowWindow(g_hwndTabControl, SW_SHOW);
    _RelayoutMainWindow(Globals.hwndMain);
}

//=============================================================================
// Tab Operations
//=============================================================================

TabDoc* TabManager_AddNewTab(HWND hwndMain)
{
    if (!Globals.hwndEdit || !g_hwndTabControl || g_nTabCount >= MAX_TABS) {
        return NULL;
    }

    sptr_t pNewDoc = SciCall_CreateDocument(0, SC_DOCUMENTOPTION_DEFAULT);
    if (!pNewDoc) return NULL;

    int nIndex = g_nTabCount;
    TabDoc* pTab = &g_Tabs[nIndex];

    pTab->pDocPointer = pNewDoc;
    pTab->bModified = false;
    pTab->bReadOnly = false;
    pTab->fvFileVars = Globals.fvCurFile;
    pTab->hFilePath = NULL;
    pTab->iEncoding = Settings.DefaultEncoding;
    pTab->iEOLMode = Settings.DefaultEOLMode;

    _BuildTabText(pTab, nIndex + 1);

    TCITEM tci = { 0 };
    tci.mask = TCIF_TEXT | TCIF_PARAM;
    tci.pszText = pTab->wchTabText;
    tci.cchTextMax = (int)COUNTOF(pTab->wchTabText);
    tci.lParam = (LPARAM)pTab;
    TabCtrl_InsertItem(g_hwndTabControl, nIndex, &tci);

    g_nTabCount++;

    TabManager_SwitchToTab(hwndMain, nIndex);

    ShowWindow(g_hwndTabControl, SW_SHOW);
    _RelayoutMainWindow(hwndMain);

    return pTab;
}

bool TabManager_CloseTab(int nIndex)
{
    if (!g_hwndTabControl || nIndex < 0 || nIndex >= g_nTabCount) {
        return false;
    }

    TabDoc* pTab = &g_Tabs[nIndex];

    if (pTab->pDocPointer && Globals.hwndEdit) {
        SciCall_ReleaseDocument(pTab->pDocPointer);
    }

    if (pTab->hFilePath) {
        Path_Release(pTab->hFilePath);
    }

    TabCtrl_DeleteItem(g_hwndTabControl, nIndex);

    for (int i = nIndex; i < g_nTabCount - 1; i++) {
        g_Tabs[i] = g_Tabs[i + 1];
        _SyncLParam(i);
    }

    TabDoc empty = { 0 };
    g_Tabs[g_nTabCount - 1] = empty;

    g_nTabCount--;

    if (g_nTabCount == 0) {
        // Detach from the released document: Scintilla creates a fresh one
        if (Globals.hwndEdit) {
            SendMessage(Globals.hwndEdit, SCI_SETDOCPOINTER, 0, 0);
        }
        ShowWindow(g_hwndTabControl, SW_HIDE);
        _RelayoutMainWindow(Globals.hwndMain);
        return true;
    } else {
        int nSwitchIndex = nIndex;
        if (nSwitchIndex >= g_nTabCount) {
            nSwitchIndex = g_nTabCount - 1;
        }
        TabManager_SwitchToTab(Globals.hwndMain, nSwitchIndex);
    }

    return true;
}

void TabManager_CloseAllOtherTabs(int nKeepIndex)
{
    if (nKeepIndex < 0 || nKeepIndex >= g_nTabCount) return;
    for (int i = g_nTabCount - 1; i >= 0; i--) {
        if (i != nKeepIndex) {
            TabManager_CloseTab(i);
        }
    }
}

void TabManager_CloseAllTabs(HWND hwndMain)
{
    while (g_nTabCount > 0) {
        TabManager_CloseTab(g_nTabCount - 1);
    }

    TabManager_Init(hwndMain, GetModuleHandle(NULL));
    TabManager_RegisterInitialTab(hwndMain);
}

void TabManager_SwitchToTab(HWND hwndMain, int nIndex)
{
    if (!Globals.hwndEdit || !g_hwndTabControl) return;
    if (nIndex < 0 || nIndex >= g_nTabCount) return;

    TabDoc* pNewTab = &g_Tabs[nIndex];

    SendMessage(Globals.hwndEdit, SCI_SETDOCPOINTER, 0, (LPARAM)pNewTab->pDocPointer);

    g_pActiveTab = pNewTab;

    if (pNewTab->hFilePath) {
        Path_Reset(Paths.CurrentFile, Path_Get(pNewTab->hFilePath));
    } else {
        Path_Empty(Paths.CurrentFile, false);
    }

    Encoding_Current(pNewTab->iEncoding);
    Globals.fvCurFile = pNewTab->fvFileVars;

    TabCtrl_SetCurSel(g_hwndTabControl, nIndex);

    if (hwndMain) {
        UpdateTitlebar(hwndMain);
        UpdateStatusbar(true);
        UpdateSaveSettingsCmds();
    }
}

void TabManager_MoveTab(int nFrom, int nTo)
{
    if (!g_hwndTabControl) return;
    if (nFrom < 0 || nFrom >= g_nTabCount) return;
    if (nTo < 0 || nTo >= g_nTabCount) return;
    if (nFrom == nTo) return;

    TabDoc tmp = g_Tabs[nFrom];

    if (nFrom < nTo) {
        for (int i = nFrom; i < nTo; i++) {
            g_Tabs[i] = g_Tabs[i + 1];
        }
    } else {
        for (int i = nFrom; i > nTo; i--) {
            g_Tabs[i] = g_Tabs[i - 1];
        }
    }
    g_Tabs[nTo] = tmp;

    // Rebuild tab strip order
    for (int i = 0; i < g_nTabCount; i++) {
        TabCtrl_DeleteItem(g_hwndTabControl, i);
    }
    for (int i = 0; i < g_nTabCount; i++) {
        TabDoc* p = &g_Tabs[i];
        TCITEM tci = { 0 };
        tci.mask = TCIF_TEXT | TCIF_PARAM;
        tci.pszText = p->wchTabText;
        tci.cchTextMax = (int)COUNTOF(p->wchTabText);
        tci.lParam = (LPARAM)p;
        TabCtrl_InsertItem(g_hwndTabControl, i, &tci);
    }

    // Keep selection on the moved tab
    TabCtrl_SetCurSel(g_hwndTabControl, nTo);
    if (g_pActiveTab) {
        // g_pActiveTab is a pointer into g_Tabs; find its new index
        for (int i = 0; i < g_nTabCount; i++) {
            if (&g_Tabs[i] == g_pActiveTab) { /* already tracked */ break; }
        }
    }
}

//=============================================================================
// Queries
//=============================================================================

int TabManager_GetActiveTabIndex(void)
{
    if (!g_hwndTabControl) return -1;
    return TabCtrl_GetCurSel(g_hwndTabControl);
}

TabDoc* TabManager_GetTab(int nIndex)
{
    if (nIndex < 0 || nIndex >= g_nTabCount) return NULL;
    return &g_Tabs[nIndex];
}

//=============================================================================
// UI Helpers
//=============================================================================

void TabManager_UpdateTabText(int nIndex)
{
    if (nIndex < 0 || nIndex >= g_nTabCount) return;
    _BuildTabText(&g_Tabs[nIndex], nIndex + 1);
    _RefreshTabControlItem(nIndex);
}

void TabManager_UpdateActiveTabText(void)
{
    int n = TabManager_GetActiveTabIndex();
    if (n >= 0) {
        TabManager_UpdateTabText(n);
    }
}

//=============================================================================
// Drag-and-Drop (reorder within bar + drag-out to new window)
//=============================================================================

static int _HitTestTab(POINT pt)
{
    if (!g_hwndTabControl) return -1;
    TCHITTESTINFO hti = { 0 };
    hti.pt = pt;
    return (int)SendMessage(g_hwndTabControl, TCM_HITTEST, 0, (LPARAM)&hti);
}

static void _OnLButtonDown(HWND hwndTab, WPARAM wParam, LPARAM lParam)
{
    UNREFERENCED_PARAMETER(wParam);
    POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
    int nTab = _HitTestTab(pt);
    if (nTab >= 0) {
        s_bDragging = true;
        s_nDragSource = nTab;
        s_bDragOut = false;
        s_ptDragStart = pt;
        SetCapture(hwndTab);
    }
}

static void _OnMouseMove(HWND hwndTab, WPARAM wParam, LPARAM lParam)
{
    if (!s_bDragging) return;
    UNREFERENCED_PARAMETER(wParam);

    POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };

    // Convert to screen coordinates and check if we left the tab band
    POINT ptScreen = pt;
    ClientToScreen(hwndTab, &ptScreen);

    RECT rcTab;
    GetWindowRect(hwndTab, &rcTab);
    InflateRect(&rcTab, 0, 8);  // small tolerance band

    if (!PtInRect(&rcTab, ptScreen)) {
        // Dragged outside the tab band
        if (!s_bDragOut) {
            s_bDragOut = true;
            SetCursor(LoadCursor(NULL, IDC_ARROW));
        }
        return;
    }
    s_bDragOut = false;

    // Reorder if we moved horizontally past another tab
    int nTarget = _HitTestTab(pt);
    if (nTarget >= 0 && nTarget != s_nDragSource) {
        TabManager_MoveTab(s_nDragSource, nTarget);
        s_nDragSource = nTarget;
    }
}

static void _OnLButtonUp(HWND hwndTab, WPARAM wParam, LPARAM lParam)
{
    UNREFERENCED_PARAMETER(wParam);
    UNREFERENCED_PARAMETER(lParam);

    if (!s_bDragging) return;

    int nSource = s_nDragSource;
    bool bOut = s_bDragOut;

    s_bDragging = false;
    s_nDragSource = -1;
    s_bDragOut = false;

    if (GetCapture() == hwndTab) {
        ReleaseCapture();
    }

    if (bOut && nSource >= 0) {
        // User dropped outside the tab band -> open new window
        _SpawnNewWindowForTab(nSource);
    }
}

// Subclass procedure for the tab control to intercept drag events
static WNDPROC s_pfnOrigTabProc = NULL;

static LRESULT CALLBACK _TabSubclassProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg) {
    case WM_LBUTTONDOWN:
        _OnLButtonDown(hwnd, wParam, lParam);
        break;
    case WM_MOUSEMOVE:
        _OnMouseMove(hwnd, wParam, lParam);
        break;
    case WM_LBUTTONUP:
        _OnLButtonUp(hwnd, wParam, lParam);
        break;
    case WM_CAPTURECHANGED:
        s_bDragging = false;
        s_nDragSource = -1;
        s_bDragOut = false;
        break;
    }
    return CallWindowProc(s_pfnOrigTabProc, hwnd, uMsg, wParam, lParam);
}

static void _EnsureSubclassed(void)
{
    if (g_hwndTabControl && !s_pfnOrigTabProc) {
        s_pfnOrigTabProc = (WNDPROC)SetWindowLongPtr(g_hwndTabControl, GWLP_WNDPROC, (LONG_PTR)_TabSubclassProc);
    }
}

//=============================================================================
// Notification Handler (WM_NOTIFY -> TCN_SELCHANGE etc.)
//=============================================================================

LRESULT TabManager_HandleNotify(HWND hwndMain, WPARAM wParam, LPARAM lParam)
{
    UNREFERENCED_PARAMETER(wParam);

    if (!g_hwndTabControl) return 0;

    LPNMHDR pnmh = (LPNMHDR)lParam;
    if (!pnmh || pnmh->hwndFrom != g_hwndTabControl) return 0;

    switch (pnmh->code) {
    case TCN_SELCHANGE: {
        int nIndex = TabCtrl_GetCurSel(g_hwndTabControl);
        if (nIndex >= 0 && nIndex < g_nTabCount) {
            _SaveActiveTabState();
            TabManager_SwitchToTab(hwndMain, nIndex);
        }
        return 0;
    }
    default:
        break;
    }

    return 0;
}

//=============================================================================
// Late Init Hook (called once the tab control exists)
//=============================================================================

// Called from Notepad3.c after TabManager_Init to install the drag subclass
void TabManager_InstallDragSupport(void)
{
    _EnsureSubclassed();
}
