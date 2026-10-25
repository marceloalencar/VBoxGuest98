/** @file
 * VBoxTray - Guest statistics.
 *
 * Follows upstream VBoxServiceStats.cpp (vgsvcVMStatsWorker): the host sets
 * the reporting interval (0, the default, means no statistics) and announces
 * changes with VMMDEV_EVENT_STATISTICS_INTERVAL_CHANGE_REQUEST; the host turns
 * it on when someone asks for guest metrics (e.g. VBoxManage metrics setup).
 *
 * Windows 98 has none of the APIs upstream uses (NtQuerySystemInformation,
 * GlobalMemoryStatusEx, GetPerformanceInfo), so:
 *  - Memory: GlobalMemoryStatus.  On Windows 9x the paging file fields
 *    describe the swap file alone, so dwTotalPageFile is used as is.
 *  - Processes and threads: ToolHelp32 snapshots.
 *  - CPU: the KERNEL\CPUUsage counter of HKEY_DYN_DATA\PerfStats (the one
 *    System Monitor shows).  It only gives the total usage, which is
 *    reported as user load with a kernel load of 0.
 *  - Disk cache: the VCACHE\cCurPages counter (4KB pages), 0 if missing.
 *  - Handles and kernel memory: not available, not reported.
 *
 * The host's metrics collector (Main, pm::CollectorGuest::isValid) only
 * records a group when every value of it arrived: user, kernel and idle CPU
 * load, and total, free, ballooned, cache and page file memory.  So these are
 * always sent together, even when some of them are 0.
 */

#include "VBoxTray.h"
#include <tlhelp32.h>
#include <string.h>


/*********************************************************************************************************************************
*   Defined Constants And Macros                                                                                                 *
*********************************************************************************************************************************/

/** How often to check for shutdown and interval changes (ms). */
#define VBOXSTATS_POLL_MS           1000
/** The Windows 9x performance counter for the CPU usage (percent). */
#define VBOXSTATS_CPU_COUNTER       "KERNEL\\CPUUsage"
/** The Windows 9x performance counter for the disk cache size (4KB pages). */
#define VBOXSTATS_CACHE_COUNTER     "VCACHE\\cCurPages"


/**
 * Reads a Windows 9x performance counter from HKEY_DYN_DATA\PerfStats.
 *
 * Reading it under StartStat starts the counter, under StatData gets the
 * current value and under StopStat stops it.
 *
 * @returns TRUE on success.
 * @param   pszSubKey   "PerfStats\\StartStat", "PerfStats\\StatData" or "PerfStats\\StopStat".
 * @param   pszCounter  The counter, VBOXSTATS_XXX_COUNTER.
 * @param   pdwValue    Where to return the value (optional).
 */
static BOOL vboxStatsReadCounter(const char *pszSubKey, const char *pszCounter, DWORD *pdwValue)
{
    DWORD dwValue = 0;
    DWORD cbValue = sizeof(dwValue);
    DWORD dwType;
    HKEY hKey;
    BOOL fOk;

    if (RegOpenKeyEx(HKEY_DYN_DATA, pszSubKey, 0, KEY_READ, &hKey) != ERROR_SUCCESS)
        return FALSE;
    fOk =    RegQueryValueEx(hKey, pszCounter, NULL, &dwType, (LPBYTE)&dwValue, &cbValue) == ERROR_SUCCESS
          && cbValue == sizeof(dwValue);
    RegCloseKey(hKey);
    if (fOk && pdwValue)
        *pdwValue = dwValue;
    return fOk;
}

/**
 * Counts the processes and their threads.
 *
 * @returns TRUE on success.
 */
static BOOL vboxStatsCountProcesses(UINT32 *pcProcesses, UINT32 *pcThreads)
{
    PROCESSENTRY32 Entry;
    HANDLE hSnapshot;
    BOOL fMore;

    *pcProcesses = 0;
    *pcThreads   = 0;
    hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE)
        return FALSE;

    Entry.dwSize = sizeof(Entry);
    for (fMore = Process32First(hSnapshot, &Entry); fMore; fMore = Process32Next(hSnapshot, &Entry))
    {
        (*pcProcesses)++;
        *pcThreads += Entry.cntThreads;
    }
    CloseHandle(hSnapshot);
    return *pcProcesses != 0;
}

/**
 * Gathers the statistics and reports them to the host.
 *
 * @param   hDevice     The device handle.
 * @param   fCounters   Whether the performance counters are running.
 */
static void vboxStatsReport(HANDLE hDevice, BOOL fCounters)
{
    VBoxGuestStatistics Stats;
    MEMORYSTATUS MemStatus;
    SYSTEM_INFO SysInfo;
    DWORD dwCpuUsage;
    DWORD cCachePages = 0;

    memset(&Stats, 0, sizeof(Stats));

    GetSystemInfo(&SysInfo);
    memset(&MemStatus, 0, sizeof(MemStatus));
    MemStatus.dwLength = sizeof(MemStatus);
    GlobalMemoryStatus(&MemStatus);

    Stats.u32PageSize       = SysInfo.dwPageSize;
    Stats.u32PhysMemTotal   = MemStatus.dwTotalPhys / 4096;
    Stats.u32PhysMemAvail   = MemStatus.dwAvailPhys / 4096;
    Stats.u32PageFileSize   = MemStatus.dwTotalPageFile / 4096;
    Stats.u32MemoryLoad     = MemStatus.dwMemoryLoad;
    Stats.u32PhysMemBalloon = 0; /* No memory ballooning on Windows 98. */
    if (fCounters)
        vboxStatsReadCounter("PerfStats\\StatData", VBOXSTATS_CACHE_COUNTER, &cCachePages);
    Stats.u32MemSystemCache = cCachePages;
    Stats.u32StatCaps       = VBOX_GUEST_STAT_PHYS_MEM_TOTAL
                            | VBOX_GUEST_STAT_PHYS_MEM_AVAIL
                            | VBOX_GUEST_STAT_PAGE_FILE_SIZE
                            | VBOX_GUEST_STAT_MEMORY_LOAD
                            | VBOX_GUEST_STAT_PHYS_MEM_BALLOON
                            | VBOX_GUEST_STAT_MEM_SYSTEM_CACHE;

    if (vboxStatsCountProcesses(&Stats.u32Processes, &Stats.u32Threads))
        Stats.u32StatCaps |= VBOX_GUEST_STAT_PROCESSES | VBOX_GUEST_STAT_THREADS;

    if (fCounters && vboxStatsReadCounter("PerfStats\\StatData", VBOXSTATS_CPU_COUNTER, &dwCpuUsage))
    {
        if (dwCpuUsage > 100)
            dwCpuUsage = 100;
        Stats.u32CpuLoad_Idle   = 100 - dwCpuUsage;
        Stats.u32CpuLoad_User   = dwCpuUsage; /* No kernel/user split on Windows 9x. */
        Stats.u32CpuLoad_Kernel = 0;
        Stats.u32StatCaps      |= VBOX_GUEST_STAT_CPU_LOAD_IDLE
                               |  VBOX_GUEST_STAT_CPU_LOAD_USER
                               |  VBOX_GUEST_STAT_CPU_LOAD_KERNEL;
    }

    Stats.u32CpuId = 0; /* Windows 98 uses a single CPU. */
    VbglR3StatReport(hDevice, &Stats);
}

/**
 * Guest statistics thread.
 *
 * @returns 0 on normal exit, 1 if the driver could not be used.
 * @param   pvUser      Unused.
 */
unsigned __stdcall VBoxStatsThread(void *pvUser)
{
    HANDLE hDevice;
    DWORD cMsInterval = 0;
    DWORD cMsSinceReport = 0;
    BOOL fCounters = FALSE;
    UINT32 fEvents;
    int rc;

    hDevice = VbglR3Open();
    if (hDevice == INVALID_HANDLE_VALUE)
        return 1;

    rc = VbglR3ChangeFilterMask(hDevice, VMMDEV_EVENT_STATISTICS_INTERVAL_CHANGE_REQUEST, 0);
    if (RT_FAILURE(rc))
    {
        VbglR3Close(hDevice);
        return 1;
    }
    VbglR3StatQueryInterval(hDevice, &cMsInterval);
    cMsSinceReport = cMsInterval; /* First report right away, like upstream. */

    while (!g_fVBoxTrayShutdown)
    {
        /* Start the performance counters on demand; they cost the system a little. */
        if (cMsInterval && !fCounters)
        {
            vboxStatsReadCounter("PerfStats\\StartStat", VBOXSTATS_CACHE_COUNTER, NULL);
            fCounters = vboxStatsReadCounter("PerfStats\\StartStat", VBOXSTATS_CPU_COUNTER, NULL);
        }

        if (cMsInterval && cMsSinceReport >= cMsInterval)
        {
            vboxStatsReport(hDevice, fCounters);
            cMsSinceReport = 0;
        }

        rc = VbglR3WaitForEvents(hDevice, VMMDEV_EVENT_STATISTICS_INTERVAL_CHANGE_REQUEST, VBOXSTATS_POLL_MS, &fEvents);
        if (RT_SUCCESS(rc) && (fEvents & VMMDEV_EVENT_STATISTICS_INTERVAL_CHANGE_REQUEST))
        {
            VbglR3StatQueryInterval(hDevice, &cMsInterval);
            cMsSinceReport = cMsInterval; /* Report right away with the new interval. */
        }
        else
        {
            if (rc != VERR_TIMEOUT)
                Sleep(VBOXSTATS_POLL_MS); /* The wait didn't block, don't spin. */
            if (cMsSinceReport < 0xffffffffU - VBOXSTATS_POLL_MS)
                cMsSinceReport += VBOXSTATS_POLL_MS;
        }
    }

    if (fCounters)
    {
        vboxStatsReadCounter("PerfStats\\StopStat", VBOXSTATS_CPU_COUNTER, NULL);
        vboxStatsReadCounter("PerfStats\\StopStat", VBOXSTATS_CACHE_COUNTER, NULL);
    }
    VbglR3ChangeFilterMask(hDevice, 0, VMMDEV_EVENT_STATISTICS_INTERVAL_CHANGE_REQUEST);
    VbglR3Close(hDevice);
    return 0;
}
