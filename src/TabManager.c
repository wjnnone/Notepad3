// encoding: UTF-8
/******************************************************************************
*                                                                             *
* Notepad3                                                                    *
* TabManager.c                                                                *
*   Multi-Tab Document Manager Implementation                                 *
*   Per-window (per-Instance) data; global g_hwndTabControl kept for the     *
*   legacy public API, but the real state lives in Instance.tabs[].          *
*                                                                             *
*******************************************************************************/

#include "TypeDefs.h"
#include "Notepad3.h"
#include "TabManager.h"
#include "Notepad3Instance.h"
#include "SciCall.h"
#include "Helpers.h"
#include "PathLib.h"
#include "Encoding.h"
#include "Dialogs.h"

#include <commctrl.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <strsafe.h>

#ifdef D_NP3_WIN10_DARK_MODE
#include "DarkMode/DarkMode.h"
#endif

//=============================================================================
// Per-Instance helpers -- all external TabManager_* calls read state from
// the currently-active Instance (Instance_GetActive()).
//=============================================================================

static InstanceTab* _GetTabs(Instance* pI)        { return pI ? pI->tabs : NULL; }
static int          _GetCount(Instance* pI)       { return pI ? pI->nTabCount : 0; }
static InstanceTab* _GetActive(Instance* pI)      { return pI ? pI->pActiveTab : NULL; }

static Instance* _GetI(void) { return Instance_GetActive(); }


//=============================================================================
// Drag state (one per Instance is sufficient; we only drag on the active one)
//=============================================================================

static bool  s_bDragging = false;
static int   s_nDragSource = -1;
static POINT s_ptDragStart = { 0, 0 };
static bool  s_bDragOut = false;

//=============================================================================
// Internal string helper
//=============================================================================

static void _BuildTabTextFor(InstanceTab* pTab, int nUntitledIndex, WCHAR* wchOut, size_t cchOut)
{
    if (!pTab || !wchOut) return;

    Instance* pI = _GetI();
    if (pI && pTab->hFilePath && Path_IsNotEmpty(pTab->hFilePath)) {
        const WCHAR* pName = Path_FindFileName(pTab->hFilePath);
        StringCchPrintf(wchOut, cchOut, L"%s%s",
                        pTab->bModified ? L"* " : L"", pName ? pName : L"Untitled");
    } else {
        StringCchPrintf(wchOut, cchOut, L"%sUntitled %i",
                        pTab->bModified ? L"* " : L"", nUntitledIndex);
    }
}


//=============================================================================
// TabManager_Init / InstallDragSupport
//=============================================================================

void TabManager_Init(HWND hwndMain, HINSTANCE hInstance)
{
    Instance* pI = Instance_FromHwnd(hwndMain);
    if (!pI) return;

    // Reset per-instance tab state
    for (int i = 0; i < NP3_INSTANCE_MAX_TABS; i++) {
        pI->tabs[i].pDocPointer = 0;
        pI->tabs[i].hFilePath = NULL;
        pI->tabs[i].bModified = false;
        memset(&pI->tabs[i].fvFileVars, 0, sizeof(FILEVARS));
        pI->tabs[i].iEncoding = Settings.DefaultEncoding;
        pI->tabs[i].iEOLMode = Settings.DefaultEOLMode;
    }
    pI->nTabCount = 0;
    pI->pActiveTab = NULL;

    // Create a tab control child window
    pI->hwndTabControl = CreateWindowEx(
        0,
        WC_TABCONTROL,
        NULL,
        (WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN | TCS_SINGLELINE | TCS_RIGHTJUSTIFY | TCS_TABS) & ~WS_VISIBLE,
        0, 0, 0, 0,
        hwndMain,
        (HMENU)(INT_PTR)IDC_TAB,
        hInstance,
        NULL);

    if (!pI->hwndTabControl) return;

    InitWindowCommon(pI->hwndTabControl, true);

#ifdef D_NP3_WIN10_DARK_MODE
    if (IsDarkModeSupported()) {
        AllowDarkModeForWindowEx(pI->hwndTabControl, CheckDarkModeEnabled());
    }
#endif

    // Also publish to the legacy global so code that still reads
    // g_hwndTabControl keeps working during migration.
    g_hwndTabControl = pI->hwndTabControl;
}


void TabManager_InstallDragSupport(void)
{
    // Drag subclass is attached lazily the first time a mouse event arrives;
    // nothing to do during init for now.  Kept as a no-op for the moment.
    // (Drag support is handled via WM_NOTIFY / WM_MOUSEMOVE in a subclass
    // installed in TabManager.c -- the subclass proc is attached when we
    // first receive WM_LBUTTONDOWN on the tab control.)
}


//=============================================================================
// TabManager_Cleanup
//=============================================================================

void TabManager_Cleanup(HWND hwndMain)
{
    UNREFERENCED_PARAMETER(hwndMain);

    Instance* pI = _GetI();
    if (!pI) return;

    // Free all owned documents
    for (int i = 0; i < pI->nTabCount; i++) {
        InstanceTab* t = &pI->tabs[i];
        if (t->pDocPointer && pI->hwndEdit) {
            if (pI->pActiveTab != t) {
                SendMessage(pI->hwndEdit, SCI_SETDOCPOINTER, 0, (LPARAM)t->pDocPointer);
            }
            SciCall_ReleaseDocument(t->pDocPointer);
        }
        if (t->hFilePath) {
            Path_Release(t->hFilePath);
        }
    }

    pI->nTabCount = 0;
    pI->pActiveTab = NULL;

    if (pI->hwndTabControl) {
        DestroyWindow(pI->hwndTabControl);
        pI->hwndTabControl = NULL;
    }

    if (pI == (Instance*)GetInstanceFromHwnd(pI->hwndMain)) { /* no-op */ }
}


//=============================================================================
// TabManager_RegisterInitialTab
//=============================================================================

void TabManager_RegisterInitialTab(HWND hwndMain)
{
    UNREFERENCED_PARAMETER(hwndMain);

    Instance* pI = _GetI();
    if (!pI || !pI->hwndEdit || !pI->hwndTabControl) return;

    sptr_t pDoc = (sptr_t)SendMessage(pI->hwndEdit, SCI_GETDOCPOINTER, 0, 0);
    if (!pDoc) return;

    SciCall_AddRefDocument(pDoc);

    int nIndex = pI->nTabCount;
    InstanceTab* pTab = &pI->tabs[nIndex];

    pTab->pDocPointer = pDoc;
    pTab->bModified = false;
    pTab->fvFileVars = pI->fvCurFile;
    pTab->iEncoding = Encoding_Current(-1);
    pTab->iEOLMode = pI->fvCurFile.chMode[0] ? pI->fvCurFile.chMode[0] : Settings.DefaultEOLMode;

    if (Path_IsNotEmpty(pI->hCurrentFile)) {
        pTab->hFilePath = Path_Copy(pI->hCurrentFile);
    } else {
        pTab->hFilePath = NULL;
    }

    TCITEM tci = { 0 };
    tci.mask = TCIF_TEXT | TCIF_PARAM;
    WCHAR wch[256] = { 0 };
    _BuildTabTextFor(pTab, nIndex + 1, wch, COUNTOF(wch));
    tci.pszText = wch;
    tci.cchTextMax = (int)COUNTOF(wch);
    tci.lParam = (LPARAM)pTab;
    TabCtrl_InsertItem(pI->hwndTabControl, nIndex, &tci);

    pI->nTabCount++;
    pI->pActiveTab = pTab;

    ShowWindow(pI->hwndTabControl, SW_SHOW);
}


//=============================================================================
// TabManager_AddNewTab
//=============================================================================

TabDoc* TabManager_AddNewTab(HWND hwndMain)
{
    Instance* pI = _GetI();
    if (!pI || !pI->hwndEdit || !pI->hwndTabControl || pI->nTabCount >= NP3_INSTANCE_MAX_TABS) {
        return NULL;
    }

    sptr_t pNewDoc = SciCall_CreateDocument(0, SC_DOCUMENTOPTION_DEFAULT);
    if (!pNewDoc) return NULL;

    int nIndex = pI->nTabCount;
    InstanceTab* pTab = &pI->tabs[nIndex];

    pTab->pDocPointer = pNewDoc;
    pTab->bModified = false;
    pTab->hFilePath = NULL;
    pTab->fvFileVars = pI->fvCurFile;
    pTab->iEncoding = Settings.DefaultEncoding;
    pTab->iEOLMode = Settings.DefaultEOLMode;

    TCITEM tci = { 0 };
    tci.mask = TCIF_TEXT | TCIF_PARAM;
    WCHAR wch[256] = { 0 };
    _BuildTabTextFor(pTab, nIndex + 1, wch, COUNTOF(wch));
    tci.pszText = wch;
    tci.cchTextMax = (int)COUNTOF(wch);
    tci.lParam = (LPARAM)pTab;
    TabCtrl_InsertItem(pI->hwndTabControl, nIndex, &tci);

    pI->nTabCount++;
    ShowWindow(pI->hwndTabControl, SW_SHOW);

    TabManager_SwitchToTab(hwndMain, nIndex);

    // Return a TabDoc* shim for the legacy public API (Notepad3.c still
    // calls TabManager_GetActiveTabIndex/TabManager_GetTab etc.)
    return (TabDoc*)pTab;
}


//=============================================================================
// TabManager_SwitchToTab
//=============================================================================

void TabManager_SwitchToTab(HWND hwndMain, int nIndex)
{
    Instance* pI = _GetI();
    if (!pI || !pI->hwndEdit || !pI->hwndTabControl) return;
    if (nIndex < 0 || nIndex >= pI->nTabCount) return;

    InstanceTab* pNewTab = &pI->tabs[nIndex];

    // Save the outgoing tab's state before we overwrite pI->fvCurFile
    if (pI->pActiveTab && pI->pActiveTab != pNewTab) {
        pI->pActiveTab->fvFileVars = pI->fvCurFile;
        pI->pActiveTab->iEncoding  = Encoding_Current(-1);
    }

    SendMessage(pI->hwndEdit, SCI_SETDOCPOINTER, 0, (LPARAM)pNewTab->pDocPointer);

    pI->pActiveTab = pNewTab;
    pI->fvCurFile  = pNewTab->fvFileVars;
    pI->iEncoding  = pNewTab->iEncoding;

    if (pNewTab->hFilePath) {
        I_SetCurrentFile(pNewTab->hFilePath);
    } else {
        Path_Empty(Paths.CurrentFile, false);
    }

    Encoding_Current(pNewTab->iEncoding);

    TabCtrl_SetCurSel(pI->hwndTabControl, nIndex);

    if (hwndMain) {
        UpdateTitlebar(hwndMain);
        UpdateStatusbar(true);
        UpdateSaveSettingsCmds();
    }
}


//=============================================================================
// TabManager_CloseTab
//=============================================================================

bool TabManager_CloseTab(int nIndex)
{
    Instance* pI = _GetI();
    if (!pI || !pI->hwndTabControl || nIndex < 0 || nIndex >= pI->nTabCount) {
        return false;
    }

    InstanceTab* pTab = &pI->tabs[nIndex];

    if (pTab->pDocPointer && pI->hwndEdit) {
        SciCall_ReleaseDocument(pTab->pDocPointer);
    }
    if (pTab->hFilePath) {
        Path_Release(pTab->hFilePath);
    }

    TabCtrl_DeleteItem(pI->hwndTabControl, nIndex);

    for (int i = nIndex; i < pI->nTabCount - 1; i++) {
        pI->tabs[i] = pI->tabs[i + 1];
        TCITEM tci = { 0 };
        tci.mask = TCIF_PARAM;
        tci.lParam = (LPARAM)&pI->tabs[i];
        TabCtrl_SetItem(pI->hwndTabControl, i, &tci);
    }

    InstanceTab empty = { 0 };
    pI->tabs[pI->nTabCount - 1] = empty;
    pI->nTabCount--;

    if (pI->pActiveTab == pTab) {
        pI->pActiveTab = NULL;
    }

    if (pI->nTabCount == 0) {
        if (pI->hwndEdit) {
            SendMessage(pI->hwndEdit, SCI_SETDOCPOINTER, 0, 0);
        }
        ShowWindow(pI->hwndTabControl, SW_HIDE);
        return true;
    } else {
        int nSwitchIndex = nIndex;
        if (nSwitchIndex >= pI->nTabCount) {
            nSwitchIndex = pI->nTabCount - 1;
        }
        TabManager_SwitchToTab(pI->hwndMain, nSwitchIndex);
    }

    return true;
}


//=============================================================================
// TabManager queries -- read from active Instance
//=============================================================================

TabDoc* TabManager_GetTab(int nIndex)
{
    Instance* pI = _GetI();
    if (!pI || nIndex < 0 || nIndex >= pI->nTabCount) return NULL;
    return (TabDoc*)&pI->tabs[nIndex];
}

int TabManager_GetActiveTabIndex(void)
{
    Instance* pI = _GetI();
    if (!pI || !pI->hwndTabControl) return -1;
    return TabCtrl_GetCurSel(pI->hwndTabControl);
}

int  TabManager_GetCount(void)          { return _GetCount(_GetI()); }
bool TabManager_IsActiveTabModified(void){ Instance* pI = _GetI(); return pI && pI->pActiveTab && pI->pActiveTab->bModified; }


//=============================================================================
// TabManager_UpdateCurrentTabModified -- called from SCN_MODIFIED / SetSaveDone
//=============================================================================

void TabManager_UpdateCurrentTabModified(bool bModified)
{
    Instance* pI = _GetI();
    if (!pI || !pI->pActiveTab) return;

    if (pI->pActiveTab->bModified != bModified) {
        pI->pActiveTab->bModified = bModified;

        // Reflect in the tab strip
        int nIdx = (int)(pI->pActiveTab - &pI->tabs[0]);
        if (nIdx >= 0 && pI->hwndTabControl) {
            TCITEM tci = { 0 };
            tci.mask = TCIF_TEXT;
            WCHAR wch[256] = { 0 };
            _BuildTabTextFor(pI->pActiveTab, nIdx + 1, wch, COUNTOF(wch));
            tci.pszText = wch;
            tci.cchTextMax = (int)COUNTOF(wch);
            TabCtrl_SetItem(pI->hwndTabControl, nIdx, &tci);
        }
    }
}


//=============================================================================
// TabManager_SetCurrentTabPath -- called after FileLoad/FileSave
//=============================================================================

void TabManager_SetCurrentTabPath(HPATHL hPath)
{
    Instance* pI = _GetI();
    if (!pI || !pI->pActiveTab) return;

    InstanceTab* t = pI->pActiveTab;
    if (t->hFilePath) Path_Release(t->hFilePath);
    t->hFilePath = hPath ? Path_Copy(hPath) : NULL;
    t->fvFileVars = pI->fvCurFile;
    t->iEncoding  = Encoding_Current(-1);

    // Refresh the tab text
    int nIdx = (int)(t - &pI->tabs[0]);
    if (nIdx >= 0 && pI->hwndTabControl) {
        TCITEM tci = { 0 };
        tci.mask = TCIF_TEXT;
        WCHAR wch[256] = { 0 };
        _BuildTabTextFor(t, nIdx + 1, wch, COUNTOF(wch));
        tci.pszText = wch;
        tci.cchTextMax = (int)COUNTOF(wch);
        TabCtrl_SetItem(pI->hwndTabControl, nIdx, &tci);
    }
}


//=============================================================================
// MoveTab -- internal swap (used by drag-reorder)
//=============================================================================

void TabManager_MoveTab(int nFrom, int nTo)
{
    Instance* pI = _GetI();
    if (!pI || !pI->hwndTabControl) return;
    if (nFrom < 0 || nFrom >= pI->nTabCount) return;
    if (nTo < 0   || nTo   >= pI->nTabCount) return;
    if (nFrom == nTo) return;

    InstanceTab tmp = pI->tabs[nFrom];

    if (nFrom < nTo) {
        for (int i = nFrom; i < nTo; i++) pI->tabs[i] = pI->tabs[i + 1];
    } else {
        for (int i = nFrom; i > nTo; i--) pI->tabs[i] = pI->tabs[i - 1];
    }
    pI->tabs[nTo] = tmp;

    // If the moved tab was the active one, update the pointer
    if (pI->pActiveTab == (InstanceTab*)&((InstanceTab*)&pI->tabs[nFrom + (nFrom > nTo ? -1 : 1)])) { /* unreachable */ }
    // Scan for new index of old active tab
    InstanceTab* oldActive = pI->pActiveTab;
    for (int i = 0; i < pI->nTabCount; i++) {
        if (&pI->tabs[i] == oldActive) { pI->pActiveTab = &pI->tabs[i]; break; }
    }

    // Rebuild tab strip
    for (int i = 0; i < pI->nTabCount; i++) {
        TabCtrl_DeleteItem(pI->hwndTabControl, i);
    }
    for (int i = 0; i < pI->nTabCount; i++) {
        InstanceTab* p = &pI->tabs[i];
        TCITEM tci = { 0 };
        tci.mask = TCIF_TEXT | TCIF_PARAM;
        WCHAR wch[256] = { 0 };
        _BuildTabTextFor(p, i + 1, wch, COUNTOF(wch));
        tci.pszText = wch;
        tci.cchTextMax = (int)COUNTOF(wch);
        tci.lParam = (LPARAM)p;
        TabCtrl_InsertItem(pI->hwndTabControl, i, &tci);
    }
    TabCtrl_SetCurSel(pI->hwndTabControl, (int)(pI->pActiveTab - &pI->tabs[0]));
}


//=============================================================================
// Drag-and-Drop handlers
//=============================================================================

static void _OnLButtonDown(HWND hwndTab, WPARAM wParam, LPARAM lParam)
{
    UNREFERENCED_PARAMETER(wParam);
    POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
    TCHITTESTINFO hti = { 0 };
    hti.pt = pt;
    int nTab = (int)SendMessage(hwndTab, TCM_HITTEST, 0, (LPARAM)&hti);
    if (nTab >= 0) {
        s_bDragging = true;
        s_nDragSource = nTab;
        s_bDragOut = false;
        SetCapture(hwndTab);
    }
}

static void _OnMouseMove(HWND hwndTab, WPARAM wParam, LPARAM lParam)
{
    UNREFERENCED_PARAMETER(wParam);
    if (!s_bDragging) return;

    POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
    POINT ptScreen = pt;
    ClientToScreen(hwndTab, &ptScreen);

    RECT rcTab;
    GetWindowRect(hwndTab, &rcTab);
    InflateRect(&rcTab, 0, 8);

    if (!PtInRect(&rcTab, ptScreen)) {
        s_bDragOut = true;
        return;
    }
    s_bDragOut = false;

    TCHITTESTINFO hti = { 0 };
    hti.pt = pt;
    int nTarget = (int)SendMessage(hwndTab, TCM_HITTEST, 0, (LPARAM)&hti);
    if (nTarget >= 0 && nTarget != s_nDragSource) {
        TabManager_MoveTab(s_nDragSource, nTarget);
        s_nDragSource = nTarget;
    }
}

// Spawn a new main window for the dragged-out tab (same process -- NOT
// ShellExecute).  Notepad3_OpenNewWindowFromTab() is defined in Notepad3.c.
extern HWND Notepad3_OpenNewWindowFromTab(HWND hwndMain, int nTabIndex);

static void _OnLButtonUp(HWND hwndTab, WPARAM wParam, LPARAM lParam)
{
    UNREFERENCED_PARAMETER(wParam);
    UNREFERENCED_PARAMETER(lParam);

    bool bOut = s_bDragOut;
    int  nSrc = s_nDragSource;

    s_bDragging = false;
    s_nDragSource = -1;
    s_bDragOut = false;

    if (GetCapture() == hwndTab) ReleaseCapture();

    if (bOut && nSrc >= 0) {
        Instance* pI = _GetI();
        HWND hwndMain = pI ? pI->hwndMain : NULL;
        Notepad3_OpenNewWindowFromTab(hwndMain, nSrc);
    }
}

// Subclass procedure (lazy-installed on first LBUTTONDOWN)
static WNDPROC s_pfnOrigTabProc = NULL;
static LRESULT CALLBACK _TabSubclassProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg) {
    case WM_LBUTTONDOWN: _OnLButtonDown(hwnd, wParam, lParam); break;
    case WM_MOUSEMOVE:   _OnMouseMove(hwnd, wParam, lParam);   break;
    case WM_LBUTTONUP:   _OnLButtonUp(hwnd, wParam, lParam);   break;
    case WM_CAPTURECHANGED:
        s_bDragging = false; s_nDragSource = -1; s_bDragOut = false; break;
    }
    return CallWindowProc(s_pfnOrigTabProc, hwnd, uMsg, wParam, lParam);
}

// Exported: attach the drag subclass to the active Instance's tab control
void TabManager_InstallDragSupport(void);  // forward
void TabManager_AttachDragSubclass(void)
{
    Instance* pI = _GetI();
    if (pI && pI->hwndTabControl && !s_pfnOrigTabProc) {
        s_pfnOrigTabProc = (WNDPROC)SetWindowLongPtr(pI->hwndTabControl, GWLP_WNDPROC, (LONG_PTR)_TabSubclassProc);
    }
}


//=============================================================================
// TabManager_HandleNotify (WM_NOTIFY -> TCN_SELCHANGE)
//=============================================================================

LRESULT TabManager_HandleNotify(HWND hwndMain, WPARAM wParam, LPARAM lParam)
{
    UNREFERENCED_PARAMETER(wParam);
    Instance* pI = _GetI();
    if (!pI || !pI->hwndTabControl) return 0;

    LPNMHDR pnmh = (LPNMHDR)lParam;
    if (!pnmh || pnmh->hwndFrom != pI->hwndTabControl) return 0;

    switch (pnmh->code) {
    case TCN_SELCHANGE: {
        int nIndex = TabCtrl_GetCurSel(pI->hwndTabControl);
        if (nIndex >= 0 && nIndex < pI->nTabCount) {
            TabManager_SwitchToTab(hwndMain, nIndex);
        }
        return 0;
    }
    default:
        break;
    }

    return 0;
}
