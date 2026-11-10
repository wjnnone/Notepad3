// encoding: UTF-8
/******************************************************************************
* Notepad3Instance.c                                                          *
*   Per-window Instance lifecycle + active-instance management.               *
*******************************************************************************/

#include "TypeDefs.h"
#include "Notepad3.h"
#include "Notepad3Instance.h"
#include "PathLib.h"

// Global list of all instances, and a pointer to the one currently receiving
// window messages (MsgProc sets it at the top of each switch).
static Instance* s_pAllInstances   = NULL;
static Instance* s_pActiveInstance = NULL;


// ---------------------------------------------------------------------------
Instance* Instance_Create(HWND hwndMain)
{
    Instance* pI = (Instance*)HeapAlloc(Globals.hndlProcessHeap, HEAP_ZERO_MEMORY, sizeof(Instance));
    if (!pI) return NULL;

    pI->hwndMain = hwndMain;
    pI->pNext = s_pAllInstances;
    s_pAllInstances = pI;

    SetWindowLongPtr(hwndMain, GWLP_USERDATA, (LONG_PTR)pI);
    return pI;
}


// ---------------------------------------------------------------------------
void Instance_Destroy(Instance* pI)
{
    if (!pI) return;

    // Close all owned tabs
    for (int i = 0; i < pI->nTabCount; i++) {
        InstanceTab* t = &pI->tabs[i];
        if (t->pDocPointer && pI->hwndEdit) {
            SciCall_ReleaseDocument(t->pDocPointer);
        }
        if (t->hFilePath) {
            Path_Release(t->hFilePath);
        }
    }

    if (pI->hCurrentFile) {
        Path_Release(pI->hCurrentFile);
    }

    // Unlink from global list
    if (s_pAllInstances == pI) {
        s_pAllInstances = pI->pNext;
    } else {
        for (Instance* cur = s_pAllInstances; cur && cur->pNext; cur = cur->pNext) {
            if (cur->pNext == pI) { cur->pNext = pI->pNext; break; }
        }
    }
    if (s_pActiveInstance == pI) {
        s_pActiveInstance = s_pAllInstances;
    }

    if (pI->hwndMain) {
        SetWindowLongPtr(pI->hwndMain, GWLP_USERDATA, 0);
    }

    HeapFree(Globals.hndlProcessHeap, 0, pI);
}


// ---------------------------------------------------------------------------
Instance* Instance_FromHwnd(HWND hwnd)
{
    if (!hwnd) return NULL;
    LONG_PTR p = GetWindowLongPtr(hwnd, GWLP_USERDATA);
    return (Instance*)p;
}


// ---------------------------------------------------------------------------
Instance* Instance_GetActive(void)
{
    return s_pActiveInstance;
}


// ---------------------------------------------------------------------------
void Instance_SetActive(Instance* pI)
{
    s_pActiveInstance = pI;
    I_UpdateStateToGlobals();
}


// ---------------------------------------------------------------------------
// Convenience helpers (declared in Notepad3Instance.h).  Kept in the .c file
// because they need PathLib.h and we deliberately don't pull that into the
// header (it has to come after all Windows SDK headers).
// ---------------------------------------------------------------------------
void I_SetCurrentFile(HPATHL h)
{
    Instance* i = Instance_GetActive();
    if (i) {
        if (i->hCurrentFile) Path_Release(i->hCurrentFile);
        i->hCurrentFile = h ? Path_Copy(h) : NULL;
        Path_Reset(Paths.CurrentFile, Path_Get(h));
        // Also sync the current file vars into the Instance so that a future
        // switch back to this window can restore them.
        i->fvCurFile = Globals.fvCurFile;
    } else {
        Path_Reset(Paths.CurrentFile, Path_Get(h));
    }
}

void I_UpdateStateToGlobals(void)
{
    Instance* i = Instance_GetActive();
    if (i) {
        // Window-level handles: always sync (they are created by MsgCreate
        // and guaranteed valid once an Instance exists).
        Globals.hwndMain   = i->hwndMain;
        Globals.hwndEdit   = i->hwndEdit;
        Globals.hwndStatus = i->hwndStatus;
        Globals.hwndToolbar= (HANDLE)i->hwndToolbar;
        Globals.hwndRebar  = (HANDLE)i->hwndRebar;

        // File-level state: ONLY sync if this Instance has been given a valid
        // file path.  If hCurrentFile is NULL (zero-initialized before any
        // FileLoad), we must NOT overwrite whatever the current FileLoad has
        // placed into Paths.CurrentFile / Globals.fvCurFile.
        if (i->hCurrentFile) {
            Path_Reset(Paths.CurrentFile, Path_Get(i->hCurrentFile));
            Globals.fvCurFile = i->fvCurFile;
        }
    }
}
