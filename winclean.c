#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <shlobj.h>

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
static int g_targetCount = sizeof(g_targets) / sizeof(g_targets[0]);
static unsigned long long g_totalBytes = 0;
static unsigned long g_totalFiles = 0;

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
                }
            } else {
                g_totalBytes += sz;
                g_totalFiles++;
                printf("  [dry] %s (%llu bytes)\n", child, sz);
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

static void printHelp(void) {
    printf("WinClean - Windows Cleaner (command line)\n\n");
    printf("Usage:\n");
    printf("  winclean [options]\n\n");
    printf("Options:\n");
    printf("  -h, --help     Show this help\n");
    printf("  -d, --dry      Dry run (list files, do not delete)\n");
    printf("  -a, --all      Enable all targets including optional ones\n");
    printf("  -l, --list     List available targets and exit\n\n");
    printf("Targets:\n");
    for (int i = 0; i < g_targetCount; i++) {
        printf("  [%d] %-18s %s\n", i, g_targets[i].name,
               g_targets[i].enabled ? "(default on)" : "(default off)");
    }
}

int main(int argc, char** argv) {
    int listOnly = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printHelp();
            return 0;
        } else if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--dry") == 0) {
            g_dryRun = 1;
        } else if (strcmp(argv[i], "-a") == 0 || strcmp(argv[i], "--all") == 0) {
            for (int j = 0; j < g_targetCount; j++) g_targets[j].enabled = 1;
        } else if (strcmp(argv[i], "-l") == 0 || strcmp(argv[i], "--list") == 0) {
            listOnly = 1;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            printHelp();
            return 1;
        }
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
        printf("Dry run total: %lu files, %llu bytes would be removed.\n",
               g_totalFiles, g_totalBytes);
    else
        printf("Total removed: %lu files, %llu bytes.\n", g_totalFiles, g_totalBytes);

    return 0;
}
