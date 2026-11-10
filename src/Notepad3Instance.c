// ---------------------------------------------------------------------------
// Notepad3_OpenNewWindowFromTab
//
// Drag-out-to-new-window (same-process, NOT ShellExecute).
// Creates a fresh main window, then moves the dragged tab's document from
// the source Instance into the newly-created Instance.  The source Instance
// drops the moved tab so no document is orphaned.
//
// Called from TabManager's drag-subclass when the user releases the mouse
// outside the tab strip.
// ---------------------------------------------------------------------------
HWND Notepad3_OpenNewWindowFromTab(HWND hwndSrcMain, int nSrcTabIndex)
{
    if (!hwndSrcMain) return NULL;

    Instance* pSrc = Instance_FromHwnd(hwndSrcMain);
    if (!pSrc) return NULL;
    if (nSrcTabIndex < 0 || nSrcTabIndex >= pSrc->nTabCount) return NULL;

    // Grab the tab we're about to move BEFORE creating the new window
    // (CreateWindowEx sends WM_CREATE which sets the active Instance, and
    // we must not confuse "which Instance holds which tab" during that call).
    InstanceTab const tSaved = pSrc->tabs[nSrcTabIndex];

    // Create the new main window.  WM_CREATE will construct a new Instance
    // and attach its own Scintilla / toolbar / statusbar / tab strip.
    DWORD const dwStyle = (WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN) & ~WS_VISIBLE;

    // Offset the new window slightly so it doesn't overlap the source
    RECT rcSrc;
    GetWindowRect(hwndSrcMain, &rcSrc);
    int const offset = 24;
    int const cx = (rcSrc.right - rcSrc.left);
    int const cy = (rcSrc.bottom - rcSrc.top);
    int const x  = rcSrc.left + offset;
    int const y  = rcSrc.top  + offset;

    HWND hwndNew = CreateWindowEx(
#if defined(_M_ARM64)
        WS_EX_ACCEPTFILES | WS_EX_COMPOSITED,
#else
        WS_EX_ACCEPTFILES,
#endif
        s_wchWndClass,
        _W(SAPPNAME),
        dwStyle,
        x, y, cx, cy,
        NULL, NULL,
        Globals.hInstance,
        NULL);

    if (!hwndNew) return NULL;

    Instance* pDst = Instance_FromHwnd(hwndNew);
    if (!pDst) {
        // Instance not yet attached -- give WM_CREATE a chance to run
        for (int i = 0; i < 500 && !pDst; i++) {
            Sleep(10);
            pDst = Instance_FromHwnd(hwndNew);
        }
    }
    if (!pDst) {
        DestroyWindow(hwndNew);
        return NULL;
    }

    // Now move the saved tab into the new Instance.
    // pDst currently has one tab (its initial empty document registered
    // by TabManager_RegisterInitialTab inside WM_CREATE).  We REPLACE
    // it with our moved tab so the new window shows the dragged-out
    // document directly instead of a "new empty" tab.

    // 1. Release the fresh empty doc that WM_CREATE set up
    if (pDst->tabs[0].pDocPointer && pDst->hwndEdit) {
        SciCall_ReleaseDocument(pDst->tabs[0].pDocPointer);
    }
    if (pDst->tabs[0].hFilePath) {
        Path_Release(pDst->tabs[0].hFilePath);
    }

    // 2. Copy the moved tab data into the new Instance's first slot
    memcpy(&pDst->tabs[0], &tSaved, sizeof(InstanceTab));
    // re-AddRef -- source Instance also held an AddRef; since we're
    // deleting the source slot below we'll need one fresh AddRef for
    // the new owner.  tSaved already has one AddRef from source, but
    // we're about to release source's reference, so keep tSaved's
    // AddRef and DON'T AddRef again here.

    pDst->tabs[0].pDocPointer = tSaved.pDocPointer;   // owned (1 AddRef)

    // 3. Activate the moved tab and wire up the doc pointer
    pDst->pActiveTab = &pDst->tabs[0];
    pDst->fvCurFile  = tSaved.fvFileVars;
    pDst->iEncoding  = tSaved.iEncoding;

    Instance_SetActive(pDst);
    SendMessage(pDst->hwndEdit, SCI_SETDOCPOINTER, 0, (LPARAM)pDst->tabs[0].pDocPointer);

    // 4. Tab strip rebuild (the current strip was built for an "Untitled"
    //    document; replace with the moved tab's text)
    {
        TCITEM tci = { 0 };
        tci.mask = TCIF_TEXT | TCIF_PARAM;
        WCHAR wch[256] = { 0 };
        if (pDst->tabs[0].hFilePath && Path_IsNotEmpty(pDst->tabs[0].hFilePath)) {
            const WCHAR* pName = Path_FindFileName(pDst->tabs[0].hFilePath);
            StringCchPrintf(wch, COUNTOF(wch), L"%s%s",
                            tSaved.bModified ? L"* " : L"", pName ? pName : L"Untitled");
        } else {
            StringCchPrintf(wch, COUNTOF(wch), L"%sUntitled 1", tSaved.bModified ? L"* " : L"");
        }
        tci.pszText = wch;
        tci.cchTextMax = (int)COUNTOF(wch);
        tci.lParam = (LPARAM)&pDst->tabs[0];
        TabCtrl_SetItem(pDst->hwndTabControl, 0, &tci);
        TabCtrl_SetCurSel(pDst->hwndTabControl, 0);
    }

    // Update title bar / status bar to reflect the dragged-out document
    UpdateTitlebar(hwndNew);
    UpdateStatusbar(true);
    UpdateSaveSettingsCmds();

    // 5. Delete the moved tab from the source Instance
    Instance_SetActive(pSrc);
    TabManager_CloseTab(nSrcTabIndex);   // releases source's AddRef (we kept tSaved's one)

    // 6. Restore active Instance to the source
    Instance_SetActive(pSrc);

    // Show the new window
    ShowWindow(hwndNew, SW_SHOW);
    UpdateWindow(hwndNew);

    return hwndNew;
}
