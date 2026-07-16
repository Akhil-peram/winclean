#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <shlobj.h>
#include <tlhelp32.h>
#include <psapi.h>

#define MAX_PATH_LEN 4096

typedef struct {
    const char* name;
    int csidl;
    int enabled;
} CleanTarget;

static CleanTarget g_targets[] = {
    { "Temporary Files",        CSIDL_INTERNET_CACHE, 1 },
    { "Windows Temp",           CSIDL_WINDOWS,        1 },
    { "User Temp",              -2,                   1 },
    { "Recent Items",           CSIDL_RECENT,         0 },
    { "Recycle Bin",            -1,                   0 },
};

static int g_dryRun = 0;
static int g_verbose = 0;
static int g_targetCount = sizeof(g_targets) / sizeof(g_targets[0]);
static unsigned long long g_totalBytes = 0;
static unsigned long g_totalFiles = 0;

static const char* formatBytes(unsigned long long bytes) {
    static char buf[32];
    if (bytes >= (1ULL << 30))
        snprintf(buf, sizeof(buf), "%.2f GB", (double)bytes / (1ULL << 30));
    else if (bytes >= (1ULL << 20))
        snprintf(buf, sizeof(buf), "%.2f MB", (double)bytes / (1ULL << 20));
    else if (bytes >= (1ULL << 10))
        snprintf(buf, sizeof(buf), "%.2f KB", (double)bytes / (1ULL << 10));
    else
        snprintf(buf, sizeof(buf), "%llu B", bytes);
    return buf;
}

static int dirExists(const char* path) {
    DWORD attr = GetFileAttributesA(path);
    return (attr != INVALID_FILE_ATTRIBUTES) &&
           (attr & FILE_ATTRIBUTE_DIRECTORY);
}

static unsigned long long fileSize(const char* path) {
    WIN32_FIND_DATAA ffd;
    HANDLE h = FindFirstFileA(path, &ffd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    LARGE_INTEGER s;
    s.LowPart = ffd.nFileSizeLow;
    s.HighPart = (LONG)ffd.nFileSizeHigh;
    FindClose(h);
    return (unsigned long long)s.QuadPart;
}

static const char* skipDot(const char* name) {
    if (strcmp(name, ".") == 0) return NULL;
    if (strcmp(name, "..") == 0) return NULL;
    return name;
}

static int deleteFile(const char* path) {
    DWORD attr = GetFileAttributesA(path);
    if (attr == INVALID_FILE_ATTRIBUTES) return 0;
    if (attr & FILE_ATTRIBUTE_DIRECTORY) {
        if (attr & FILE_ATTRIBUTE_READONLY)
            SetFileAttributesA(path, attr & ~FILE_ATTRIBUTE_READONLY);
        return RemoveDirectoryA(path) ? 1 : 0;
    }
    if (attr & FILE_ATTRIBUTE_READONLY)
        SetFileAttributesA(path, attr & ~FILE_ATTRIBUTE_READONLY);
    return DeleteFileA(path) ? 1 : 0;
}

static int deletePath(const char* path, unsigned long long* bytes, unsigned long* files) {
    WIN32_FIND_DATAA ffd;
    char search[MAX_PATH_LEN];
    char child[MAX_PATH_LEN];
    HANDLE h;

    snprintf(search, sizeof(search), "%s\\*", path);
    h = FindFirstFileA(search, &ffd);
    if (h == INVALID_HANDLE_VALUE) return 0;

    do {
        const char* nm = skipDot(ffd.cFileName);
        if (!nm) continue;
        snprintf(child, sizeof(child), "%s\\%s", path, nm);

        if (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            deletePath(child, bytes, files);
            if (!g_dryRun) deleteFile(child);
        } else {
            unsigned long long sz = fileSize(child);
            *bytes += sz;
            (*files)++;
            if (!g_dryRun) {
                if (deleteFile(child)) {
                    g_totalBytes += sz;
                    g_totalFiles++;
                    if (g_verbose) printf("  [del] %s (%s)\n", child, formatBytes(sz));
                } else if (g_verbose) {
                    printf("  [skip] %s (in use or locked)\n", child);
                }
            } else {
                g_totalBytes += sz;
                g_totalFiles++;
                printf("  [dry] %s (%s)\n", child, formatBytes(sz));
            }
        }
    } while (FindNextFileA(h, &ffd));
    FindClose(h);
    return 1;
}

static int emptyRecycleBin(void) {
    DWORD flags = g_dryRun ? 0 : (SHERB_NOCONFIRMATION | SHERB_NOPROGRESSUI | SHERB_NOSOUND);
    HRESULT hr = SHEmptyRecycleBinA(NULL, NULL, flags);
    if (hr == S_OK) { printf("  Recycle Bin emptied.\n"); return 1; }
    printf("  Recycle Bin: already empty or unavailable (0x%08X).\n", (unsigned)hr);
    return 0;
}

static int getProcessName(DWORD pid, char* buf, size_t size) {
    HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (h == NULL) {
        if (GetLastError() == ERROR_ACCESS_DENIED) {
            HANDLE h2 = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            if (h2) {
                DWORD sz = (DWORD)size;
                if (QueryFullProcessImageNameA(h2, 0, buf, &sz)) {
                    char* name = strrchr(buf, '\\');
                    strncpy(buf, name ? name + 1 : buf, size);
                    CloseHandle(h2);
                    return 1;
                }
                CloseHandle(h2);
            }
        }
        return 0;
    }
    HMODULE mod;
    DWORD needed;
    if (EnumProcessModules(h, &mod, sizeof(mod), &needed)) {
        GetModuleBaseNameA(h, mod, buf, (DWORD)size);
        CloseHandle(h);
        return 1;
    }
    CloseHandle(h);
    return 0;
}

static int isUserProcess(DWORD pid) {
    BOOL isService = FALSE;
    SC_HANDLE scm = OpenSCManagerA(NULL, NULL, SC_MANAGER_ENUMERATE_SERVICE);
    if (!scm) return 1;
    ENUM_SERVICE_STATUSA services[1024];
    DWORD bytesNeeded = 0, servicesReturned = 0, resume = 0;
    if (EnumServicesStatusA(scm, SERVICE_WIN32, SERVICE_ACTIVE, services,
                            sizeof(services), &bytesNeeded, &servicesReturned, &resume)) {
        for (DWORD i = 0; i < servicesReturned; i++) {
            SC_HANDLE svc = OpenServiceA(scm, services[i].lpServiceName, SERVICE_QUERY_STATUS);
            if (svc) {
                SERVICE_STATUS_PROCESS ssp;
                DWORD dummy = 0;
                if (QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO, (LPBYTE)&ssp,
                                         sizeof(ssp), &dummy)) {
                    if (ssp.dwProcessId == pid) isService = TRUE;
                }
                CloseServiceHandle(svc);
            }
        }
    }
    CloseServiceHandle(scm);
    return !isService;
}

typedef struct {
    DWORD pid;
    char name[MAX_PATH_LEN];
    unsigned long long memBytes;
    double cpuPercent;
} ProcInfo;

static int g_numProcs = 0;
static ProcInfo* g_procs = NULL;

static unsigned long long getProcessMem(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!h) return 0;
    PROCESS_MEMORY_COUNTERS pmc;
    unsigned long long mem = 0;
    if (GetProcessMemoryInfo(h, &pmc, sizeof(pmc)))
        mem = (unsigned long long)pmc.WorkingSetSize;
    CloseHandle(h);
    return mem;
}

static void collectProcesses(int userOnly) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    g_procs = NULL;
    g_numProcs = 0;
    int cap = 0;
    PROCESSENTRY32 pe;
    pe.dwSize = sizeof(pe);
    if (Process32First(snap, &pe)) {
        do {
            if (pe.th32ProcessID == 0) continue;
            if (userOnly && !isUserProcess(pe.th32ProcessID)) continue;
            if (g_numProcs >= cap) {
                cap = cap ? cap * 2 : 64;
                g_procs = realloc(g_procs, cap * sizeof(ProcInfo));
            }
            ProcInfo* p = &g_procs[g_numProcs];
            p->pid = pe.th32ProcessID;
            p->memBytes = getProcessMem(pe.th32ProcessID);
            p->cpuPercent = 0.0;
            if (getProcessName(pe.th32ProcessID, p->name, sizeof(p->name)))
                ;
            else
                strncpy(p->name, pe.szExeFile, sizeof(p->name) - 1);
            g_numProcs++;
        } while (Process32Next(snap, &pe));
    }
    CloseHandle(snap);
}

static void computeCpu(void) {
    double cpuSum = 0.0;
    for (int i = 0; i < g_numProcs; i++) {
        HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, g_procs[i].pid);
        if (!h) continue;
        FILETIME create, exit, kernel, user;
        if (GetProcessTimes(h, &create, &exit, &kernel, &user)) {
            ULARGE_INTEGER k, u;
            k.LowPart = kernel.dwLowDateTime; k.HighPart = kernel.dwHighDateTime;
            u.LowPart = user.dwLowDateTime; u.HighPart = user.dwHighDateTime;
            g_procs[i].cpuPercent = (double)(k.QuadPart + u.QuadPart) / 1e7;
            cpuSum += g_procs[i].cpuPercent;
        }
        CloseHandle(h);
    }
    if (cpuSum > 0.0)
        for (int i = 0; i < g_numProcs; i++)
            g_procs[i].cpuPercent = g_procs[i].cpuPercent / cpuSum * 100.0;
}

static int cmpMem(const void* a, const void* b) {
    const ProcInfo* x = a, * y = b;
    return (y->memBytes > x->memBytes) - (y->memBytes < x->memBytes);
}

static int cmpCpu(const void* a, const void* b) {
    const ProcInfo* x = a, * y = b;
    return (y->cpuPercent > x->cpuPercent) - (y->cpuPercent < x->cpuPercent);
}

static void showProcesses(int userOnly, int sortBy) {
    collectProcesses(userOnly);
    if (g_numProcs == 0) {
        fprintf(stderr, "Could not enumerate processes.\n");
        return;
    }
    computeCpu();
    if (sortBy == 1) qsort(g_procs, g_numProcs, sizeof(ProcInfo), cmpCpu);
    else if (sortBy == 2) qsort(g_procs, g_numProcs, sizeof(ProcInfo), cmpMem);

    printf("%-8s %-28s %10s %8s\n", "PID", "NAME", "MEM", "CPU%");
    for (int i = 0; i < g_numProcs; i++) {
        printf("%-8lu %-28s %10s %7.1f\n",
               g_procs[i].pid, g_procs[i].name,
               formatBytes(g_procs[i].memBytes), g_procs[i].cpuPercent);
    }
    free(g_procs);
    g_procs = NULL;
    g_numProcs = 0;
}

static void killProcess(const char* arg) {
    DWORD pid = (DWORD)strtoul(arg, NULL, 10);
    if (pid == 0) {
        fprintf(stderr, "Invalid PID: %s\n", arg);
        return;
    }
    HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
    if (!h) {
        fprintf(stderr, "Could not open PID %lu (access denied or not found).\n", pid);
        return;
    }
    if (TerminateProcess(h, 0)) {
        printf("Killed PID %lu.\n", pid);
    } else {
        fprintf(stderr, "Failed to kill PID %lu (0x%08X).\n", pid, (unsigned)GetLastError());
    }
    CloseHandle(h);
}

static void printHelp(void) {
    printf("WinClean - Windows Cleaner (command line)\n\n");
    printf("Usage:\n");
    printf("  winclean [options]\n\n");
    printf("Options:\n");
    printf("  -h, --help        Show this help\n");
    printf("  -d, --dry         Dry run (list files, do not delete)\n");
    printf("  -a, --all         Enable all targets including optional ones\n");
    printf("  -l, --list        List available targets and exit\n");
    printf("  -v, --verbose     Show each file as it is processed\n");
    printf("  -p, --procs       Show running processes with CPU/RAM usage\n");
    printf("  -u, --userprocs   Show user/application processes only (no services)\n");
    printf("  -s, --sort KEY    Sort processes by 'cpu' or 'mem' (with -p/-u)\n");
    printf("  -k, --kill PID    Terminate the process with the given PID\n\n");
    printf("Targets:\n");
    for (int i = 0; i < g_targetCount; i++) {
        printf("  [%d] %-18s %s\n", i, g_targets[i].name,
               g_targets[i].enabled ? "(default on)" : "(default off)");
    }
}

int main(int argc, char** argv) {
    int listOnly = 0;
    int showProcs = 0;
    int userProcs = 0;
    int sortBy = 0;
    int sortNext = 0;
    int killNext = 0;
    for (int i = 1; i < argc; i++) {
        if (killNext) {
            killProcess(argv[i]);
            killNext = 0;
            return 0;
        }
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printHelp();
            return 0;
        } else if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--dry") == 0) {
            g_dryRun = 1;
        } else if (strcmp(argv[i], "-a") == 0 || strcmp(argv[i], "--all") == 0) {
            for (int j = 0; j < g_targetCount; j++) g_targets[j].enabled = 1;
        } else if (strcmp(argv[i], "-l") == 0 || strcmp(argv[i], "--list") == 0) {
            listOnly = 1;
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            g_verbose = 1;
        } else if (strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--procs") == 0) {
            showProcs = 1;
        } else if (strcmp(argv[i], "-u") == 0 || strcmp(argv[i], "--userprocs") == 0) {
            userProcs = 1;
        } else if (strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "--sort") == 0) {
            sortNext = 1;
        } else if (strcmp(argv[i], "-k") == 0 || strcmp(argv[i], "--kill") == 0) {
            killNext = 1;
        } else if (sortNext) {
            if (strcmp(argv[i], "cpu") == 0) sortBy = 1;
            else if (strcmp(argv[i], "mem") == 0) sortBy = 2;
            else { fprintf(stderr, "Unknown sort key: %s\n", argv[i]); return 1; }
            sortNext = 0;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            printHelp();
            return 1;
        }
    }

    if (killNext) {
        fprintf(stderr, "Missing PID for --kill.\n");
        return 1;
    }

    if (sortNext) {
        fprintf(stderr, "Missing sort key (cpu|mem) for --sort.\n");
        return 1;
    }

    if (showProcs || userProcs) {
        showProcesses(userProcs, sortBy);
        return 0;
    }

    if (listOnly) {
        for (int i = 0; i < g_targetCount; i++)
            printf("[%d] %s\n", i, g_targets[i].name);
        return 0;
    }

    printf("WinClean %s\n", g_dryRun ? "(dry run)" : "");
    printf("========================================\n");

    char base[MAX_PATH_LEN];
    for (int i = 0; i < g_targetCount; i++) {
        if (!g_targets[i].enabled) continue;

        printf("\n[%s]\n", g_targets[i].name);

        if (g_targets[i].csidl == -1) {
            emptyRecycleBin();
            continue;
        }

        char cleanPath[MAX_PATH_LEN];
        if (g_targets[i].csidl == -2) {
            DWORD len = GetTempPathA(sizeof(cleanPath), cleanPath);
            if (len == 0 || len > sizeof(cleanPath)) {
                printf("  Could not resolve temp path.\n");
                continue;
            }
        } else {
            if (SHGetFolderPathA(NULL, g_targets[i].csidl, NULL, 0, base) != S_OK) {
                printf("  Could not resolve path.\n");
                continue;
            }
            if (g_targets[i].csidl == CSIDL_WINDOWS)
                snprintf(cleanPath, sizeof(cleanPath), "%s\\Temp", base);
            else
                snprintf(cleanPath, sizeof(cleanPath), "%s", base);
        }

        if (!dirExists(cleanPath)) {
            printf("  Path does not exist: %s\n", cleanPath);
            continue;
        }

        unsigned long long bytes = 0;
        unsigned long files = 0;
        deletePath(cleanPath, &bytes, &files);
        if (!g_dryRun) printf("  Cleaned %lu files, %llu bytes.\n", files, bytes);
    }

    printf("\n========================================\n");
    if (g_dryRun)
        printf("Dry run total: %lu files, %s would be removed.\n",
               g_totalFiles, formatBytes(g_totalBytes));
    else
        printf("Total removed: %lu files, %s.\n", g_totalFiles, formatBytes(g_totalBytes));

    return 0;
}
