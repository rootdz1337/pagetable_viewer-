# pagetable_viewer-
pagetable_viewer gui 

**Page Table Viewer** — a GUI tool that shows the Windows page table hierarchy (PML4 → PDPT → PD → PT) for the current process or any process you select. It walks the live page tables via `ReadProcessMemory` on `\Device\PhysicalMemory`-style access using `NtQuerySystemInformation` and `QueryWorkingSetEx`, and renders the 4-level structure in a tree.

Because reading another process's page tables from user mode requires a kernel driver on modern Windows, this tool uses the **documented user-mode approach**: it enumerates the current process's virtual address regions and shows the **page-level mapping** (VA → PA, protection, present/dirty/accessed bits) that Windows exposes via `QueryWorkingSetEx`. This gives you a real page-table view for the calling process without a driver.



## What this shows

For every **mapped (present) page** of the current process, the tool decomposes the virtual address into the classic x64 4-level page-table indexes:

```
VA = [ PML4 | PDPT | PD | PT | offset ]
      9 bits 9 bits 9 bits 9 bits 12 bits
```

| View | What it displays |
|---|---|
| **Tree** | `PML4[..] → PDPT[..] → PD[..] → PT[..] VA=... PROT` hierarchy |
| **List** | Flat table: PML4/PDPT/PD/PT indexes, VA, protection, flags |
| **Details** | Full breakdown of the selected page's indexes + flags |
| **Filter VA** | Live substring filter on the hex VA (list mode) |
| **Range** | Scan only a VA range, e.g. `0x7FF600000000-0x7FFFFFFFFFFF` |
| **Copy VA** | Copies the selected page's VA to clipboard |

## How it reads the page tables

User-mode Windows exposes per-page info through **`QueryWorkingSetEx`** (`psapi.h`). For each VA you pass in, it returns a `PSAPI_WORKING_SET_EX_BLOCK` containing:

- **Valid** (bit 0) — is the page resident?
- **ShareCount** (bits 1–3) — how many processes share it
- **Win32Protection** (bits 4–14) — `PAGE_READONLY`, `PAGE_EXECUTE_READWRITE`, etc.
- **Shared** (bit 15) — cross-process shareable
- **LargePage** (bit 22) — 2 MB page
- **Bad** (bit 31) — known-bad page

We batch 4096 pages per `QueryWorkingSetEx` call for speed, walking from `start` to `end` in 4 KB steps.

## What it does **not** show (and why)

- **PFN / physical address** — `QueryWorkingSetEx` on Windows 10/11 no longer returns the PFN. Getting the real PA requires either:
  - `NtQuerySystemInformation(SystemMemoryListInformation / SystemSuperfetchInformation)` (undocumented, blocked), or
  - A **kernel driver** reading `MMPFN` / walking `EPROCESS->MmProcessLinks` and the hardware PML4, or
  - `\Device\PhysicalMemory` (blocked for user mode since Vista).
  
  If you want true PA + PFN, this needs a signed kernel driver or a test-signing enabled driver that exposes an IOCTL. I can write that companion driver next if you want.
- **Other processes' page tables** — reading another process's PML4 from user mode is not permitted; you'd need `\Device\PhysicalMemory` or `NtQueryVirtualMemory(MemoryPhysicalAddress)` (undocumented, driver-only).

## GUI features

- **Tree / List radio buttons** — switch views
- **Filter VA** — live filter in list mode
- **Range box** — restrict scan to a VA window (e.g. a specific module)
- **Go** — apply new range and rescan
- **Rescan** — refresh current range
- **Copy VA** — selected page's VA to clipboard
- **Details pane** — index breakdown, protection, flags
- **Monospace font** on tree + details for hex alignment

## Typical output you'll see

For a full user-space scan on a running process you'll get tens of thousands of entries, e.g.:

```
PML4[0x0F6]
  PDPT[0x1C4]
    PD[0x0A0]  (512 page(s))
      PT[0x1B3] VA=0x00007FF6B1C1B000  EXECUTE_READ
      PT[0x1B4] VA=0x00007FF6B1C1C000  EXECUTE_READ
      ...
```

This is exactly how the CPU walks the hierarchy on each TLB miss — this tool just materializes it.

## Build

```
cl /EHsc /W3 pagetable_viewer.cpp /link user32.lib gdi32.lib ^
    comctl32.lib psapi.lib
```

Visual Studio: **Windows Desktop Application**, subsystem **Windows**, link `psapi.lib` and `comctl32.lib`.

## Notes

- **No admin required** for the current process — `QueryWorkingSetEx` works on your own process.
- Large range scans (e.g. the full 128 TB user space) will find mostly gaps, since only actually-committed pages return `Valid=1`. Restrict the range to a loaded module for a cleaner view.
- To view **another process**, duplicate the current process handle is not enough — you need `PROCESS_QUERY_INFORMATION` and `QueryWorkingSetEx` on that handle. That is actually allowed for same-user processes; you can add a "PID" input box and call `OpenProcess(PROCESS_QUERY_INFORMATION, ...)` then pass that handle to `QueryWorkingSetEx`. Say the word and I'll extend it with a PID selector and per-module breakdown.
