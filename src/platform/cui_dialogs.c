/* Native file dialogs and URL opening, shared by every backend on a given
 * operating system: Win32 common dialogs, AppleScript on macOS, and zenity or
 * kdialog on Linux and BSD. */
#include "cui_platform.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>
#include <shellapi.h>

/* UTF-8 to UTF-16; always terminated, empty on failure. */
static bool cui__to_wide(const char *s, WCHAR *out, int cap) {
    if (cap <= 0) return false;
    int n = MultiByteToWideChar(CP_UTF8, 0, s ? s : "", -1, out, cap);
    if (n <= 0) { out[0] = 0; return false; }
    out[cap - 1] = 0;
    return true;
}

/* Appends a NUL terminated UTF-16 string at *n; false when it does not fit. */
static bool cui__w_append(WCHAR *buf, int cap, int *n, const char *s) {
    if (*n >= cap) return false;
    int k = MultiByteToWideChar(CP_UTF8, 0, s, -1, buf + *n, cap - *n);
    if (k <= 0) return false;
    *n += k;
    return true;
}

bool cui__plat_file_dialog(cui_app *app, const cui_file_dialog_opts *o, char *out, size_t cap) {
    (void)app;
    HWND owner = GetActiveWindow();
    WCHAR title[256];
    cui__to_wide(o->title ? o->title : (o->kind == CUI_FILE_SAVE ? "Save" : "Open"), title, 256);
    bool multi = o->multi && o->kind == CUI_FILE_OPEN;
    int path_cap = multi ? 32768 : MAX_PATH * 4;   /* a multi selection returns the folder and every name in one buffer */
    WCHAR *path = (WCHAR *)calloc((size_t)path_cap, sizeof(WCHAR));
    if (!path) return false;
    bool result = false;
    if (o->kind == CUI_FILE_FOLDER) {
        /* the new style folder dialog needs COM on this thread */
        HRESULT com = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        BROWSEINFOW bi;
        memset(&bi, 0, sizeof bi);
        bi.hwndOwner = owner;
        bi.lpszTitle = title;
        bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
        LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
        BOOL ok = FALSE;
        if (pidl) {
            ok = SHGetPathFromIDListW(pidl, path);
            CoTaskMemFree(pidl);
        }
        if (SUCCEEDED(com)) CoUninitialize();
        if (!ok) goto done;
    } else {
        /* filter: "Images\0*.png;*.jpg\0All files\0*.*\0\0" */
        WCHAR filter[512];
        int n = 0;
        bool fit = true;
        if (o->filter && *o->filter) {
            char spec[400];
            size_t k = 0;
            for (const char *p = o->filter; *p && k + 8 < sizeof spec;) {
                const char *e = strchr(p, ';');
                size_t l = e ? (size_t)(e - p) : strlen(p);
                k += (size_t)snprintf(spec + k, sizeof spec - k, "%s*.%.*s", k ? ";" : "", (int)l, p);
                p = e ? e + 1 : p + l;
            }
            fit = cui__w_append(filter, 511, &n, o->filter_name ? o->filter_name : "Files") && cui__w_append(filter, 511, &n, spec);
        }
        if (!fit) n = 0;
        if (!cui__w_append(filter, 511, &n, "All files") || !cui__w_append(filter, 511, &n, "*.*")) n = 0;
        filter[n] = 0;   /* list ends with an empty string */
        if (o->default_name) cui__to_wide(o->default_name, path, path_cap);
        OPENFILENAMEW ofn;
        memset(&ofn, 0, sizeof ofn);
        ofn.lStructSize = sizeof ofn;
        ofn.hwndOwner = owner;
        ofn.lpstrFilter = filter;
        ofn.lpstrFile = path;
        ofn.nMaxFile = (DWORD)path_cap;
        ofn.lpstrTitle = title;
        ofn.Flags = OFN_EXPLORER | OFN_NOCHANGEDIR | (o->kind == CUI_FILE_SAVE ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
        if (multi) ofn.Flags |= OFN_ALLOWMULTISELECT;
        BOOL ok = o->kind == CUI_FILE_SAVE ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn);
        if (!ok) goto done;
    }
    if (!multi) {
        result = WideCharToMultiByte(CP_UTF8, 0, path, -1, out, (int)cap, NULL, NULL) > 0;
        if (result && strlen(out) + 2 <= cap) out[strlen(out) + 1] = 0;   /* packed list of one */
        goto done;
    }
    {
        /* one file: the full path. Several: the folder, then each name, every string ending
         * in a NUL and the list in a second one. Packed as full paths, each ending in a NUL. */
        const WCHAR *first = path, *next = first + wcslen(first) + 1;
        size_t used = 0;
        if (*next == 0) {   /* a single file */
            int k = WideCharToMultiByte(CP_UTF8, 0, first, -1, NULL, 0, NULL, NULL);
            if (k <= 0 || used + (size_t)k + 1 > cap) goto done;
            WideCharToMultiByte(CP_UTF8, 0, first, -1, out, (int)cap, NULL, NULL);
            used = (size_t)k;
        } else {
            size_t dirlen = wcslen(first);
            for (const WCHAR *name = next; *name; name += wcslen(name) + 1) {
                WCHAR full[MAX_PATH * 4];
                size_t nl = wcslen(name);
                if (dirlen + nl + 2 > MAX_PATH * 4) continue;
                memcpy(full, first, dirlen * sizeof(WCHAR));
                size_t at = dirlen;
                if (at && full[at - 1] != L'\\' && full[at - 1] != L'/') full[at++] = L'\\';
                memcpy(full + at, name, (nl + 1) * sizeof(WCHAR));
                int k = WideCharToMultiByte(CP_UTF8, 0, full, -1, NULL, 0, NULL, NULL);
                if (k <= 0 || used + (size_t)k + 1 > cap) { out[0] = 0; goto done; }
                WideCharToMultiByte(CP_UTF8, 0, full, -1, out + used, (int)(cap - used), NULL, NULL);
                used += (size_t)k;
            }
        }
        out[used] = 0;
        result = used > 0;
    }
done:
    free(path);
    return result;
}

bool cui__plat_open_url(const char *url) {
    WCHAR w[4096];
    if (!cui__to_wide(url, w, 4096)) return false;
    return (INT_PTR)ShellExecuteW(NULL, L"open", w, NULL, NULL, SW_SHOWNORMAL) > 32;
}

#else
#include <errno.h>
#include <sys/wait.h>
#include <unistd.h>

static void cui__shell_quote(char *dst, size_t cap, const char *src) {
    size_t o = 0;
    if (o + 1 < cap) dst[o++] = '\'';
    for (const char *p = src ? src : ""; *p && o + 5 < cap; p++) {
        if (*p == '\'') { dst[o++] = '\''; dst[o++] = '\\'; dst[o++] = '\''; dst[o++] = '\''; }
        else dst[o++] = *p;
    }
    if (o + 1 < cap) dst[o++] = '\'';
    dst[o] = 0;
}

/* Runs a command and reads the first line of its output (newline stripped). */
static bool cui__run_line(const char *cmd, char *out, size_t cap) {
    FILE *f = popen(cmd, "r");
    if (!f) return false;
    if (!fgets(out, (int)cap, f)) out[0] = 0;
    int status = pclose(f);
    size_t n = strlen(out);
    while (n && (out[n - 1] == '\n' || out[n - 1] == '\r')) out[--n] = 0;
    return status == 0 && n > 0;
}

/* Runs a command and packs its output as a list of strings: the text is split at every
 * `sep`, empty pieces are dropped, each string ends in a NUL and the list in a second one.
 * True when at least one string was read and the command succeeded. */
static bool cui__run_list(const char *cmd, char sep, char *out, size_t cap) {
    if (cap < 3) return false;
    FILE *f = popen(cmd, "r");
    if (!f) return false;
    size_t n = fread(out, 1, cap - 2, f);
    int status = pclose(f);
    size_t o = 0, i = 0;
    while (i < n) {
        size_t s = i;
        while (i < n && out[i] != sep && out[i] != '\n' && out[i] != '\r') i++;
        if (i > s) {
            memmove(out + o, out + s, i - s);
            o += i - s;
            out[o++] = 0;
        }
        i++;
    }
    out[o] = 0;
    if (o == 0) out[0] = 0;
    return status == 0 && o > 0;
}

/* Starts a program without waiting for it and without leaving a zombie:
 * the intermediate child exits at once and the grandchild is reparented. */
static bool cui__spawn_detached(const char *prog, const char *arg) {
    pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        pid_t g = fork();
        if (g == 0) { execlp(prog, prog, arg, (char *)NULL); _exit(127); }
        _exit(g < 0 ? 1 : 0);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) { if (errno != EINTR) return false; }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

#if !defined(__APPLE__)
/* 1 present, 0 missing, -1 unknown; looked up once per tool. */
static bool cui__have(const char *tool) {
    static int zenity = -1, kdialog = -1, xdg = -1;
    int *slot = strcmp(tool, "zenity") == 0 ? &zenity : strcmp(tool, "kdialog") == 0 ? &kdialog : &xdg;
    if (*slot < 0) {
        char cmd[256];
        snprintf(cmd, sizeof cmd, "command -v %s >/dev/null 2>&1", tool);
        *slot = system(cmd) == 0;
    }
    return *slot == 1;
}
#endif


#if defined(__APPLE__)
static void cui__applescript_quote(char *dst, size_t cap, const char *src) {
    size_t o = 0;
    for (const char *p = src ? src : ""; *p && o + 3 < cap; p++) {
        if (*p == '"' || *p == '\\') dst[o++] = '\\';
        dst[o++] = *p;
    }
    dst[o] = 0;
}

static bool cui__posix_file_dialog(const cui_file_dialog_opts *o, char *out, size_t cap) {
    char title[300], name[300], script[1400], quoted[1600], cmd[1800];
    cui__applescript_quote(title, sizeof title, o->title ? o->title : (o->kind == CUI_FILE_SAVE ? "Save" : "Open"));
    cui__applescript_quote(name, sizeof name, o->default_name ? o->default_name : "Untitled");
    if (o->kind == CUI_FILE_FOLDER) snprintf(script, sizeof script, "POSIX path of (choose folder with prompt \"%s\")", title);
    else if (o->kind == CUI_FILE_SAVE) snprintf(script, sizeof script, "POSIX path of (choose file name with prompt \"%s\" default name \"%s\")", title, name);
    else {
        char types[300] = "";
        if (o->filter && *o->filter) {
            size_t k = (size_t)snprintf(types, sizeof types, " of type {");
            for (const char *p = o->filter; *p && k + 8 < sizeof types;) {
                const char *e = strchr(p, ';');
                size_t l = e ? (size_t)(e - p) : strlen(p);
                k += (size_t)snprintf(types + k, sizeof types - k, "%s\"%.*s\"", p == o->filter ? "" : ", ", (int)l, p);
                p = e ? e + 1 : p + l;
            }
            snprintf(types + k, sizeof types - k, "}");
        }
        if (o->multi)
            snprintf(script, sizeof script, "set o to \"\"\nrepeat with f in (choose file with prompt \"%s\"%s with multiple selections allowed)\nset o to o & POSIX path of f & linefeed\nend repeat\no", title, types);
        else
            snprintf(script, sizeof script, "POSIX path of (choose file with prompt \"%s\"%s)", title, types);
    }
    cui__shell_quote(quoted, sizeof quoted, script);
    snprintf(cmd, sizeof cmd, "osascript -e %s 2>/dev/null", quoted);
    if (o->multi && o->kind == CUI_FILE_OPEN) return cui__run_list(cmd, '\n', out, cap);
    return cui__run_line(cmd, out, cap);
}

bool cui__plat_open_url(const char *url) { return cui__spawn_detached("open", url); }
#else
static bool cui__posix_file_dialog(const cui_file_dialog_opts *o, char *out, size_t cap) {
    char cmd[2048], title[300], name[600], filter[400];
    cui__shell_quote(title, sizeof title, o->title ? o->title : (o->kind == CUI_FILE_SAVE ? "Save" : "Open"));
    cui__shell_quote(name, sizeof name, o->default_name ? o->default_name : "");
    filter[0] = 0;
    if (cui__have("zenity")) {
        size_t n = (size_t)snprintf(cmd, sizeof cmd, "zenity --file-selection --title=%s", title);
        if (o->kind == CUI_FILE_SAVE) n += (size_t)snprintf(cmd + n, sizeof cmd - n, " --save --confirm-overwrite");
        if (o->kind == CUI_FILE_FOLDER) n += (size_t)snprintf(cmd + n, sizeof cmd - n, " --directory");
        if (o->multi && o->kind == CUI_FILE_OPEN) n += (size_t)snprintf(cmd + n, sizeof cmd - n, " --multiple --separator=\"$(printf '\\037')\"");
        if (o->kind == CUI_FILE_SAVE && o->default_name) n += (size_t)snprintf(cmd + n, sizeof cmd - n, " --filename=%s", name);
        if (o->filter && *o->filter && o->kind != CUI_FILE_FOLDER) {
            char spec[360];
            size_t k = (size_t)snprintf(spec, sizeof spec, "%s |", o->filter_name ? o->filter_name : "Files");
            for (const char *p = o->filter; *p && k + 8 < sizeof spec;) {
                const char *e = strchr(p, ';');
                size_t l = e ? (size_t)(e - p) : strlen(p);
                k += (size_t)snprintf(spec + k, sizeof spec - k, " *.%.*s", (int)l, p);
                p = e ? e + 1 : p + l;
            }
            cui__shell_quote(filter, sizeof filter, spec);
            n += (size_t)snprintf(cmd + n, sizeof cmd - n, " --file-filter=%s", filter);
        }
        snprintf(cmd + n, sizeof cmd - n, " 2>/dev/null");
    } else if (cui__have("kdialog")) {
        const char *mode = o->kind == CUI_FILE_SAVE ? "--getsavefilename" : o->kind == CUI_FILE_FOLDER ? "--getexistingdirectory" : "--getopenfilename";
        char spec[360] = "";
        if (o->filter && *o->filter && o->kind != CUI_FILE_FOLDER) {
            size_t k = 0;
            for (const char *p = o->filter; *p && k + 8 < sizeof spec;) {
                const char *e = strchr(p, ';');
                size_t l = e ? (size_t)(e - p) : strlen(p);
                k += (size_t)snprintf(spec + k, sizeof spec - k, "%s*.%.*s", k ? " " : "", (int)l, p);
                p = e ? e + 1 : p + l;
            }
            snprintf(spec + k, sizeof spec - k, "|%s", o->filter_name ? o->filter_name : "Files");
            cui__shell_quote(filter, sizeof filter, spec);
        }
        snprintf(cmd, sizeof cmd, "kdialog %s %s %s %s --title %s 2>/dev/null", mode, o->default_name ? name : "'.'", filter,
                 o->multi && o->kind == CUI_FILE_OPEN ? "--multiple --separate-output" : "", title);
    } else {
        fprintf(stderr, "cui: install zenity or kdialog for file dialogs\n");
        return false;
    }
    if (o->multi && o->kind == CUI_FILE_OPEN) return cui__run_list(cmd, '\037', out, cap);
    return cui__run_line(cmd, out, cap);
}

bool cui__plat_open_url(const char *url) {
    if (!cui__have("xdg-open")) return false;
    return cui__spawn_detached("xdg-open", url);
}

#endif

bool cui__plat_file_dialog(cui_app *app, const cui_file_dialog_opts *o, char *out, size_t cap) {
    (void)app;
    bool ok = cui__posix_file_dialog(o, out, cap);
    /* a single path is a list of one: the second NUL ends it */
    if (ok && !(o->multi && o->kind == CUI_FILE_OPEN) && strlen(out) + 2 <= cap) out[strlen(out) + 1] = 0;
    return ok;
}
#endif
