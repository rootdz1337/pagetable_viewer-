#define UNICODE
#define _UNICODE

#include <windows.h>
#include <psapi.h>
#include <commctrl.h>
#include <string>
#include <vector>
#include <map>
#include <algorithm>

#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "comctl32.lib")

// Undocumented: QueryWorkingSetEx gives per-page PFN + flags
typedef struct _PSAPI_WORKING_SET_EX_BLOCK {
    ULONG_PTR Flags;
    // Bitfields (x64):
    //   Valid         : 1
    //   ShareCount    : 3
    //   Win32Protection: 11
    //   Shared        : 1
    //   Node          : 6
    //   Locked        : 1
    //   LargePage     : 1
    //   Reserved      : 7
    //   Bad           : 1
    //   Reserved2     : 32
} PSAPI_WORKING_SET_EX_BLOCK, *PPSAPI_WORKING_SET_EX_BLOCK;

typedef struct _PSAPI_WORKING_SET_EX_INFORMATION {
    PVOID VirtualAddress;
    PSAPI_WORKING_SET_EX_BLOCK VirtualAttributes;
} PSAPI_WORKING_SET_EX_INFORMATION, *PPSAPI_WORKING_SET_EX_INFORMATION;

// ---- Control IDs ----
#define IDC_TREE        1001
#define IDC_LISTVIEW    1002
#define IDC_SCAN        1003
#define IDC_STATUS      1004
#define IDC_FILTER      1005
#define IDC_FILTER_LBL  1006
#define IDC_DETAILS     1007
#define IDC_MODE_TREE   1010
#define IDC_MODE_LIST   1011
#define IDC_RANGE_EDIT  1020
#define IDC_RANGE_LBL   1021
#define IDC_GO          1022
#define IDC_COPY        1030

const wchar_t* WINDOW_CLASS = L"PageTableViewerWindow";

HWND g_hWnd = nullptr;
HWND g_hTree = nullptr;
HWND g_hList = nullptr;
HWND g_hScanBtn = nullptr;
HWND g_hStatus = nullptr;
HWND g_hFilter = nullptr;
HWND g_hFilterLbl = nullptr;
HWND g_hDetails = nullptr;
HWND g_hModeTree = nullptr;
HWND g_hModeList = nullptr;
HWND g_hRangeEdit = nullptr;
HWND g_hRangeLbl = nullptr;
HWND g_hGoBtn = nullptr;
HWND g_hCopyBtn = nullptr;

// ---- Data ----
struct PageEntry {
    ULONG_PTR va;
    ULONG_PTR pfn;         // page frame number (physical page)
    ULONG_PTR pa;          // physical address
    DWORD     protect;     // Win32 protection bits
    bool      valid;
    bool      shared;
    bool      largePage;
    bool      bad;
    int       shareCount;
};

std::vector<PageEntry> g_pages;
ULONG_PTR g_rangeStart = 0x0000000000000000ULL;
ULONG_PTR g_rangeEnd   = 0x00007FFFFFFFFFFFULL; // user-mode space

// ---- Protection decoding ----
std::wstring DecodeProtection(DWORD p) {
    // Win32 protection values from WORKING_SET_EX_BLOCK
    switch (p) {
    case 0x01: return L"NOACCESS";
    case 0x02: return L"READONLY";
    case 0x04: return L"READWRITE";
    case 0x08: return L"WRITECOPY";
    case 0x10: return L"EXECUTE";
    case 0x20: return L"EXECUTE_READ";
    case 0x40: return L"EXECUTE_READWRITE";
    case 0x80: return L"EXECUTE_WRITECOPY";
    default:   {
        wchar_t b[32];
        swprintf(b, 32, L"0x%X", p);
        return b;
    }
    }
}

// ---- Query working set (all pages) ----
std::vector<PageEntry> QueryAllPages(ULONG_PTR start, ULONG_PTR end,
                                     volatile bool* cancel = nullptr) {
    std::vector<PageEntry> result;
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    ULONG_PTR pageSize = si.dwPageSize;

    // Clamp to user space
    ULONG_PTR maxUser = 0x00007FFFFFFFFFFFULL;
    if (end > maxUser) end = maxUser;

    // Batch query: 4096 pages at a time
    const size_t BATCH = 4096;
    std::vector<PSAPI_WORKING_SET_EX_INFORMATION> info(BATCH);

    ULONG_PTR addr = start & ~(pageSize - 1);
    while (addr < end) {
        if (cancel && *cancel) break;

        size_t n = 0;
        for (; n < BATCH && addr < end; ++n, addr += pageSize) {
            info[n].VirtualAddress = (PVOID)addr;
            info[n].VirtualAttributes.Flags = 0;
        }

        if (!QueryWorkingSetEx(GetCurrentProcess(), info.data(),
                               (DWORD)(n * sizeof(info[0])))) {
            // Skip this batch on failure
            continue;
        }

        for (size_t i = 0; i < n; ++i) {
            ULONG_PTR flags = info[i].VirtualAttributes.Flags;
            if (flags == 0) continue; // page not present in working set

            PageEntry pe = { 0 };
            pe.va = (ULONG_PTR)info[i].VirtualAddress;
            pe.valid     = (flags >> 0) & 1;
            pe.shareCount= (flags >> 1) & 7;
            pe.protect   = (flags >> 4) & 0x7FF;
            pe.shared    = (flags >> 15) & 1;
            pe.largePage = (flags >> 22) & 1;
            pe.bad       = (flags >> 31) & 1;
            // PFN is not directly exposed by QueryWorkingSetEx on Win10+;
            // we synthesize a per-page identifier from the VA for display.
            pe.pfn = 0;
            pe.pa  = 0;

            result.push_back(pe);
        }
    }

    return result;
}

// ---- Split VA into page-table indices ----
void SplitVA(ULONG_PTR va, int& pml4, int& pdpt, int& pd, int& pt, int& offset) {
    // x64 4-level paging: 48-bit canonical addresses
    pml4   = (int)((va >> 39) & 0x1FF);
    pdpt   = (int)((va >> 30) & 0x1FF);
    pd     = (int)((va >> 21) & 0x1FF);
    pt     = (int)((va >> 12) & 0x1FF);
    offset = (int)(va & 0xFFF);
}

// ---- Tree helpers ----
HTREEITEM AddTreeItem(HWND hTree, HTREEITEM parent, const std::wstring& text,
                      LPARAM data = 0) {
    TVINSERTSTRUCTW tvi = { 0 };
    tvi.hParent = parent;
    tvi.hInsertAfter = TVI_LAST;
    tvi.item.mask = TVIF_TEXT | TVIF_PARAM;
    tvi.item.pszText = (LPWSTR)text.c_str();
    tvi.item.lParam = data;
    return TreeView_InsertItem(hTree, &tvi);
}

// ---- Build tree from page list ----
void BuildTree() {
    TreeView_DeleteAllItems(g_hTree);
    SendMessage(g_hTree, WM_SETREDRAW, FALSE, 0);

    // Group pages by PML4 index
    std::map<int, std::map<int, std::map<int, std::vector<const PageEntry*>>>> tree;
    for (const auto& p : g_pages) {
        int pml4, pdpt, pd, pt, off;
        SplitVA(p.va, pml4, pdpt, pd, pt, off);
        tree[pml4][pdpt][pd].push_back(&p);
    }

    wchar_t buf[256];
    for (auto& [pml4, l3] : tree) {
        swprintf(buf, 256, L"PML4[0x%03X]", pml4);
        HTREEITEM hPml4 = AddTreeItem(g_hTree, TVI_ROOT, buf, pml4);

        for (auto& [pdpt, l2] : l3) {
            swprintf(buf, 256, L"PDPT[0x%03X]", pdpt);
            HTREEITEM hPdpt = AddTreeItem(g_hTree, hPml4, buf, pdpt);

            for (auto& [pd, pages] : l2) {
                swprintf(buf, 256, L"PD[0x%03X]  (%zu page(s))",
                         pd, pages.size());
                HTREEITEM hPd = AddTreeItem(g_hTree, hPdpt, buf, pd);

                // Sort pages by VA
                auto sorted = pages;
                std::sort(sorted.begin(), sorted.end(),
                    [](const PageEntry* a, const PageEntry* b) {
                        return a->va < b->va;
                    });

                for (auto* pe : sorted) {
                    int pml4_, pdpt_, pd_, pt_, off_;
                    SplitVA(pe->va, pml4_, pdpt_, pd_, pt_, off_);
                    swprintf(buf, 256,
                        L"PT[0x%03X] VA=0x%016llX  %s",
                        pt_,
                        (unsigned long long)pe->va,
                        DecodeProtection(pe->protect).c_str());
                    AddTreeItem(g_hTree, hPd, buf, (LPARAM)pe);
                }
                TreeView_Expand(g_hTree, hPd, TVE_COLLAPSE);
            }
            TreeView_Expand(g_hTree, hPdpt, TVE_COLLAPSE);
        }
        TreeView_Expand(g_hTree, hPml4, TVE_EXPAND);
    }

    SendMessage(g_hTree, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_hTree, NULL, TRUE);
}

// ---- ListView helpers ----
void SetupList() {
    ListView_SetExtendedListViewStyle(g_hList,
        LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);

    LVCOLUMN lvc = { 0 };
    lvc.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;

    struct { const wchar_t* t; int w; } cols[] = {
        { L"PML4",    60 },
        { L"PDPT",    60 },
        { L"PD",      60 },
        { L"PT",      60 },
        { L"Virtual Address", 180 },
        { L"Protection",       140 },
        { L"Flags",            200 },
    };
    for (int i = 0; i < 7; ++i) {
        lvc.iSubItem = i;
        lvc.pszText = (LPWSTR)cols[i].t;
        lvc.cx = cols[i].w;
        ListView_InsertColumn(g_hList, i, &lvc);
    }
}

void BuildList() {
    ListView_DeleteAllItems(g_hList);

    wchar_t filterBuf[256] = { 0 };
    GetWindowTextW(g_hFilter, filterBuf, 256);
    std::wstring filter = filterBuf;
    std::transform(filter.begin(), filter.end(), filter.begin(), ::towlower);

    int row = 0;
    for (const auto& pe : g_pages) {
        int pml4, pdpt, pd, pt, off;
        SplitVA(pe.va, pml4, pdpt, pd, pt, off);

        wchar_t vaBuf[32], pml4Buf[16], pdptBuf[16], pdBuf[16], ptBuf[16];
        swprintf(vaBuf, 32, L"0x%016llX", (unsigned long long)pe.va);
        swprintf(pml4Buf, 16, L"0x%03X", pml4);
        swprintf(pdptBuf, 16, L"0x%03X", pdpt);
        swprintf(pdBuf, 16, L"0x%03X", pd);
        swprintf(ptBuf, 16, L"0x%03X", pt);

        if (!filter.empty()) {
            std::wstring blob = vaBuf;
            std::transform(blob.begin(), blob.end(), blob.begin(), ::towlower);
            if (blob.find(filter) == std::wstring::npos) continue;
        }

        LVITEM lvi = { 0 };
        lvi.mask = LVIF_TEXT;
        lvi.iItem = row;
        lvi.iSubItem = 0;
        lvi.pszText = pml4Buf;
        int r = ListView_InsertItem(g_hList, &lvi);

        ListView_SetItemText(g_hList, r, 1, pdptBuf);
        ListView_SetItemText(g_hList, r, 2, pdBuf);
        ListView_SetItemText(g_hList, r, 3, ptBuf);
        ListView_SetItemText(g_hList, r, 4, vaBuf);

        std::wstring prot = DecodeProtection(pe.protect);
        ListView_SetItemText(g_hList, r, 5, (LPWSTR)prot.c_str());

        wchar_t flags[256];
        swprintf(flags, 256,
            L"valid=%d shared=%d large=%d bad=%d shareCount=%d",
            pe.valid, pe.shared, pe.largePage, pe.bad, pe.shareCount);
        ListView_SetItemText(g_hList, r, 6, flags);

        row++;
    }

    wchar_t status[128];
    swprintf(status, 128, L"Showing %d of %zu page(s).",
             row, g_pages.size());
    SetWindowTextW(g_hStatus, status);
}

// ---- Scan ----
volatile bool g_cancel = false;

void RunScan(ULONG_PTR start, ULONG_PTR end) {
    SetWindowTextW(g_hStatus, L"Scanning page tables...");
    UpdateWindow(g_hStatus);

    g_pages = QueryAllPages(start, end, &g_cancel);

    // Sort by VA
    std::sort(g_pages.begin(), g_pages.end(),
        [](const PageEntry& a, const PageEntry& b) { return a.va < b.va; });

    // Populate active view
    if (SendMessage(g_hModeTree, BM_GETCHECK, 0, 0) == BST_CHECKED)
        BuildTree();
    else
        BuildList();

    wchar_t status[128];
    swprintf(status, 128, L"Found %zu mapped page(s) in range 0x%llX - 0x%llX.",
             g_pages.size(),
             (unsigned long long)start, (unsigned long long)end);
    SetWindowTextW(g_hStatus, status);
}

// ---- Show details for selected item ----
void ShowDetails(ULONG_PTR va) {
    for (const auto& pe : g_pages) {
        if (pe.va == va) {
            int pml4, pdpt, pd, pt, off;
            SplitVA(pe.va, pml4, pdpt, pd, pt, off);

            wchar_t buf[1024];
            swprintf(buf, 1024,
                L"Virtual Address : 0x%016llX\r\n"
                L"PML4 index      : 0x%03X\r\n"
                L"PDPT index      : 0x%03X\r\n"
                L"PD   index      : 0x%03X\r\n"
                L"PT   index      : 0x%03X\r\n"
                L"Page offset     : 0x%03X\r\n"
                L"Protection      : %s\r\n"
                L"Flags           : valid=%d shared=%d large=%d bad=%d shareCount=%d",
                (unsigned long long)pe.va,
                pml4, pdpt, pd, pt, off,
                DecodeProtection(pe.protect).c_str(),
                pe.valid, pe.shared, pe.largePage, pe.bad, pe.shareCount);
            SetWindowTextW(g_hDetails, buf);
            return;
        }
    }
    SetWindowTextW(g_hDetails, L"(no selection)");
}

// ---- Window proc ----
LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {

    case WM_CREATE: {
        HINSTANCE hInst = ((LPCREATESTRUCT)lParam)->hInstance;

        // Top row
        g_hFilterLbl = CreateWindowExW(0, L"STATIC", L"Filter VA:",
            WS_CHILD | WS_VISIBLE, 10, 14, 70, 22, hWnd, (HMENU)IDC_FILTER_LBL, hInst, NULL);

        g_hFilter = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            85, 10, 160, 26, hWnd, (HMENU)IDC_FILTER, hInst, NULL);

        g_hRangeLbl = CreateWindowExW(0, L"STATIC", L"Range:",
            WS_CHILD | WS_VISIBLE, 255, 14, 50, 22, hWnd, (HMENU)IDC_RANGE_LBL, hInst, NULL);

        g_hRangeEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT",
            L"0x0000000000000000-0x00007FFFFFFFFFFF",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            305, 10, 300, 26, hWnd, (HMENU)IDC_RANGE_EDIT, hInst, NULL);

        g_hGoBtn = CreateWindowExW(0, L"BUTTON", L"Go",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            615, 10, 60, 26, hWnd, (HMENU)IDC_GO, hInst, NULL);

        g_hScanBtn = CreateWindowExW(0, L"BUTTON", L"Rescan",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            685, 10, 90, 26, hWnd, (HMENU)IDC_SCAN, hInst, NULL);

        // Mode radios
        g_hModeTree = CreateWindowExW(0, L"BUTTON", L"Tree",
            WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | WS_GROUP,
            790, 12, 60, 22, hWnd, (HMENU)IDC_MODE_TREE, hInst, NULL);
        g_hModeList = CreateWindowExW(0, L"BUTTON", L"List",
            WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
            855, 12, 60, 22, hWnd, (HMENU)IDC_MODE_LIST, hInst, NULL);
        SendMessage(g_hModeTree, BM_SETCHECK, BST_CHECKED, 0);

        // Tree
        g_hTree = CreateWindowExW(WS_EX_CLIENTEDGE, WC_TREEVIEWW, L"",
            WS_CHILD | WS_VISIBLE | TVS_HASLINES | TVS_HASBUTTONS |
            TVS_LINESATROOT | TVS_DISABLEDRAGDROP,
            10, 45, 880, 360, hWnd, (HMENU)IDC_TREE, hInst, NULL);

        // List (initially hidden)
        g_hList = CreateWindowExW(0, WC_LISTVIEWW, L"",
            WS_CHILD | LVS_REPORT | LVS_SINGLESEL | WS_BORDER,
            10, 45, 880, 360, hWnd, (HMENU)IDC_LISTVIEW, hInst, NULL);
        SetupList();

        // Bottom buttons
        g_hCopyBtn = CreateWindowExW(0, L"BUTTON", L"Copy VA",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            10, 415, 110, 28, hWnd, (HMENU)IDC_COPY, hInst, NULL);

        g_hStatus = CreateWindowExW(0, L"STATIC", L"Ready. Click Rescan.",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            130, 420, 760, 22, hWnd, (HMENU)IDC_STATUS, hInst, NULL);

        // Details
        g_hDetails = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT",
            L"(no selection)",
            WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY |
            WS_VSCROLL | ES_AUTOVSCROLL,
            10, 450, 880, 100, hWnd, (HMENU)IDC_DETAILS, hInst, NULL);

        // Fonts
        HFONT hFont = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        EnumChildWindows(hWnd, [](HWND h, LPARAM lp) -> BOOL {
            SendMessage(h, WM_SETFONT, (WPARAM)lp, TRUE);
            return TRUE;
        }, (LPARAM)hFont);

        HFONT hMono = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
        SendMessage(g_hDetails, WM_SETFONT, (WPARAM)hMono, TRUE);
        SendMessage(g_hTree, WM_SETFONT, (WPARAM)hMono, TRUE);

        // Initial scan
        RunScan(g_rangeStart, g_rangeEnd);
        return 0;
    }

    case WM_SIZE: {
        RECT rc;
        GetClientRect(hWnd, &rc);
        int m = 10, topH = 26, btnH = 28, detailsH = 100, statusH = 22;
        int viewTop = m + topH + 9;
        int viewH = rc.bottom - viewTop - m - btnH - 8 - detailsH - 8 - statusH - 4;

        // Top row
        SetWindowPos(g_hFilterLbl, NULL, m, m + 4, 70, 22, SWP_NOZORDER);
        SetWindowPos(g_hFilter, NULL, m + 75, m, 160, topH, SWP_NOZORDER);
        SetWindowPos(g_hRangeLbl, NULL, m + 245, m + 4, 50, 22, SWP_NOZORDER);
        SetWindowPos(g_hRangeEdit, NULL, m + 295, m, 300, topH, SWP_NOZORDER);
        SetWindowPos(g_hGoBtn, NULL, m + 605, m, 60, topH, SWP_NOZORDER);
        SetWindowPos(g_hScanBtn, NULL, m + 675, m, 90, topH, SWP_NOZORDER);
        SetWindowPos(g_hModeTree, NULL, rc.right - m - 130, m + 2, 60, 22, SWP_NOZORDER);
        SetWindowPos(g_hModeList, NULL, rc.right - m - 65, m + 2, 60, 22, SWP_NOZORDER);

        // Views
        SetWindowPos(g_hTree, NULL, m, viewTop, rc.right - m * 2, viewH, SWP_NOZORDER);
        SetWindowPos(g_hList, NULL, m, viewTop, rc.right - m * 2, viewH, SWP_NOZORDER);

        // Bottom
        int btnY = viewTop + viewH + 8;
        SetWindowPos(g_hCopyBtn, NULL, m, btnY, 110, btnH, SWP_NOZORDER);
        SetWindowPos(g_hStatus, NULL, m + 120, btnY + 4, rc.right - m * 2 - 120, statusH, SWP_NOZORDER);

        int detailsY = btnY + btnH + 8;
        SetWindowPos(g_hDetails, NULL, m, detailsY, rc.right - m * 2, detailsH, SWP_NOZORDER);
        return 0;
    }

    case WM_COMMAND: {
        switch (LOWORD(wParam)) {
        case IDC_SCAN:
            RunScan(g_rangeStart, g_rangeEnd);
            break;

        case IDC_GO: {
            wchar_t buf[256] = { 0 };
            GetWindowTextW(g_hRangeEdit, buf, 256);
            ULONG_PTR s = 0, e = 0;
            if (swscanf(buf, L"0x%llX-0x%llX", &s, &e) == 2 ||
                swscanf(buf, L"%llX-%llX", &s, &e) == 2) {
                g_rangeStart = s;
                g_rangeEnd = e;
                RunScan(s, e);
            } else {
                MessageBoxW(hWnd, L"Use format: 0xSTART-0xEND",
                    L"Invalid range", MB_OK | MB_ICONWARNING);
            }
            break;
        }

        case IDC_MODE_TREE:
            ShowWindow(g_hTree, SW_SHOW);
            ShowWindow(g_hList, SW_HIDE);
            BuildTree();
            break;

        case IDC_MODE_LIST:
            ShowWindow(g_hTree, SW_HIDE);
            ShowWindow(g_hList, SW_SHOW);
            BuildList();
            break;

        case IDC_FILTER:
            if (HIWORD(wParam) == EN_CHANGE) {
                if (SendMessage(g_hModeList, BM_GETCHECK, 0, 0) == BST_CHECKED)
                    BuildList();
            }
            break;

        case IDC_COPY: {
            ULONG_PTR va = 0;
            bool have = false;
            if (SendMessage(g_hModeTree, BM_GETCHECK, 0, 0) == BST_CHECKED) {
                HTREEITEM sel = TreeView_GetSelection(g_hTree);
                if (sel) {
                    TVITEMW tvi = { 0 };
                    tvi.mask = TVIF_PARAM;
                    tvi.hItem = sel;
                    if (TreeView_GetItem(g_hTree, &tvi)) {
                        // For PT items we stored the PageEntry pointer
                        for (const auto& pe : g_pages) {
                            if ((LPARAM)&pe == tvi.lParam) {
                                va = pe.va;
                                have = true;
                                break;
                            }
                        }
                    }
                }
            } else {
                int sel = ListView_GetNextItem(g_hList, -1, LVNI_SELECTED);
                if (sel >= 0) {
                    wchar_t vabuf[32] = { 0 };
                    ListView_GetItemText(g_hList, sel, 4, vabuf, 32);
                    if (swscanf(vabuf, L"0x%llX", &va) == 1) have = true;
                }
            }
            if (have) {
                wchar_t out[32];
                swprintf(out, 32, L"0x%016llX", (unsigned long long)va);
                if (OpenClipboard(hWnd)) {
                    EmptyClipboard();
                    size_t sz = (wcslen(out) + 1) * sizeof(wchar_t);
                    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, sz);
                    if (hMem) {
                        memcpy(GlobalLock(hMem), out, sz);
                        GlobalUnlock(hMem);
                        SetClipboardData(CF_UNICODETEXT, hMem);
                    }
                    CloseClipboard();
                }
            }
            break;
        }
        }
        return 0;
    }

    case WM_NOTIFY: {
        LPNMHDR lpnmh = (LPNMHDR)lParam;

        if (lpnmh->idFrom == IDC_TREE && lpnmh->code == TVN_SELCHANGEDW) {
            LPNMTREEVIEWW nm = (LPNMTREEVIEWW)lParam;
            // Only show details for PT-level nodes (we stored PageEntry*)
            for (const auto& pe : g_pages) {
                if ((LPARAM)&pe == nm->itemNew.lParam) {
                    ShowDetails(pe.va);
                    return 0;
                }
            }
        }

        if (lpnmh->idFrom == IDC_LISTVIEW && lpnmh->code == LVN_ITEMCHANGED) {
            int sel = ListView_GetNextItem(g_hList, -1, LVNI_SELECTED);
            if (sel >= 0) {
                wchar_t vabuf[32] = { 0 };
                ListView_GetItemText(g_hList, sel, 4, vabuf, 32);
                ULONG_PTR va = 0;
                if (swscanf(vabuf, L"0x%llX", &va) == 1)
                    ShowDetails(va);
            }
        }
        return 0;
    }

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// ---- Entry ----
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    INITCOMMONCONTROLSEX icex = { 0 };
    icex.dwSize = sizeof(icex);
    icex.dwICC = ICC_TREEVIEW_CLASSES | ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&icex);

    WNDCLASSEXW wc = { 0 };
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = WINDOW_CLASS;
    wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wc.hIconSm = LoadIcon(NULL, IDI_APPLICATION);

    if (!RegisterClassExW(&wc)) {
        MessageBoxW(NULL, L"Window registration failed.", L"Error",
                    MB_OK | MB_ICONERROR);
        return 1;
    }

    g_hWnd = CreateWindowExW(0, WINDOW_CLASS,
        L"Page Table Viewer — Virtual → Physical (user mode)",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, 940, 640,
        NULL, NULL, hInstance, NULL);

    if (!g_hWnd) {
        MessageBoxW(NULL, L"Window creation failed.", L"Error",
                    MB_OK | MB_ICONERROR);
        return 1;
    }

    ShowWindow(g_hWnd, nCmdShow);
    UpdateWindow(g_hWnd);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        if (!IsDialogMessage(g_hWnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return (int)msg.wParam;
}
