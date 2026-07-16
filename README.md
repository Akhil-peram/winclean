# WinClean

A small, dependency-free Windows system cleaner for the command line. It
recursively removes junk from common temp/cache locations and can empty the
Recycle Bin.

## Features

- Cleans Temporary Internet Files, Windows Temp, and the user Temp folder
- Optionally clears Recent Items and the Recycle Bin
- Dry-run mode to preview what would be deleted
- Verbose mode to log every file as it is processed
- Human-readable size totals (B / KB / MB / GB)
- Show processes based on Memory and CPU usage
- Can kill processes with elevated privileges

## Build

Requires MinGW-w64 (`gcc`). From the project directory:

```powershell
gcc -O2 -o winclean.exe winclean.c -lshfolder -lshell32
```

## Usage

```
winclean [options]
```

| Option           | Description                                          |
| ---------------- | ---------------------------------------------------- |
| `-h, --help`     | Show help                                            |
| `-d, --dry`      | Dry run — list files, do not delete                  |
| `-a, --all`      | Enable all targets, including optional ones          |
| `-l, --list`     | List available targets and exit                      |
| `-v, --verbose`  | Show each file as it is processed                    |
| `-p, --processes` | Show running processes                               |

### Examples

```powershell
# Preview what would be cleaned
winclean --dry

# Clean everything, including Recent Items and Recycle Bin
winclean --all

# Clean defaults and log every deleted file
winclean --verbose
```

## Targets

| # | Name             | Default | Notes                                  |
| - | ---------------- | ------- | -------------------------------------- |
| 0 | Temporary Files  | on      | Internet cache (`CSIDL_INTERNET_CACHE`)|
| 1 | Windows Temp     | on      | `%WINDIR%\Temp`                        |
| 2 | User Temp        | on      | `GetTempPath` location                 |
| 3 | Recent Items     | off     | `CSIDL_RECENT`                         |
| 4 | Recycle Bin      | off     | Empties the bin via the shell          |

## Notes

- Run from an elevated (Administrator) prompt for best results, since some
  temp files are owned by system services.
- Files that are locked or in use are skipped automatically.
- This tool deletes files permanently (except the Recycle Bin target, which
  only empties the bin). Use `--dry` first if you are unsure.
