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

static void cui__w(const char *s, WCHAR *out, int cap) { MultiByteToWideChar(CP_UTF8, 0, s ? s : "", -1, out, cap); }

bool cui__plat_file_dialog(cui_app *app, const cui_file_dialog_opts *o, char *out, size_t cap) {
    (void)app;
    HWND owner = GetActiveWindow();
    WCHAR title[256];
    cui__w(o->title ? o->title : (o->kind == CUI_FILE_SAVE ? "Save" : "Open"), title, 256);
    WCHAR path[MAX_PATH * 4] = { 0 };
    if (o->kind == CUI_FILE_FOLDER) {
        BROWSEINFOW bi;
        memset(&bi, 0, sizeof bi);
        bi.hwndOwner = owner;
        bi.lpszTitle = title;
        bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
        LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
        if (!pidl) return false;
        BOOL ok = SHGetPathFromIDListW(pidl, path);
        CoTaskMemFree(pidl);
        if (!ok) return false;
    } else {
        /* filter: "Images\0*.png;*.jpg\0All files\0*.*\0\0" */
        WCHAR filter[512];
        int n = 0;
        if (o->filter && *o->filter) {
            char spec[400];
            size_t k = 0;
            for (const char *p = o->filter; *p && k + 8 < sizeof spec;) {
                const char *e = strchr(p, ';');
                size_t l = e ? (size_t)(e - p) : strlen(p);
                k += (size_t)snprintf(spec + k, sizeof spec - k, "%s*.%.*s", k ? ";" : "", (int)l, p);
                p = e ? e + 1 : p + l;
            }
            n += MultiByteToWideChar(CP_UTF8, 0, o->filter_name ? o->filter_name : "Files", -1, filter + n, 512 - n);
            n += MultiByteToWideChar(CP_UTF8, 0, spec, -1, filter + n, 512 - n);
        }
        n += MultiByteToWideChar(CP_UTF8, 0, "All files", -1, filter + n, 512 - n);
        n += MultiByteToWideChar(CP_UTF8, 0, "*.*", -1, filter + n, 512 - n);
        filter[n] = 0;
        if (o->default_name) cui__w(o->default_name, path, MAX_PATH * 4);
        OPENFILENAMEW ofn;
        memset(&ofn, 0, sizeof ofn);
        ofn.lStructSize = sizeof ofn;
        ofn.hwndOwner = owner;
        ofn.lpstrFilter = filter;
        ofn.lpstrFile = path;
        ofn.nMaxFile = MAX_PATH * 4;
        ofn.lpstrTitle = title;
        ofn.Flags = OFN_EXPLORER | OFN_NOCHANGEDIR | (o->kind == CUI_FILE_SAVE ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
        BOOL ok = o->kind == CUI_FILE_SAVE ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn);
        if (!ok) return false;
    }
    return WideCharToMultiByte(CP_UTF8, 0, path, -1, out, (int)cap, NULL, NULL) > 0;
}

bool cui__plat_open_url(const char *url) {
    WCHAR w[2048];
    cui__w(url, w, 2048);
    return (INT_PTR)ShellExecuteW(NULL, L"open", w, NULL, NULL, SW_SHOWNORMAL) > 32;
}

#else
#include <sys/wait.h>
#include <unistd.h>
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

#if !defined(__APPLE__)
static bool cui__have(const char *tool) {
    char cmd[256];
    snprintf(cmd, sizeof cmd, "command -v %s >/dev/null 2>&1", tool);
    return system(cmd) == 0;
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
    else snprintf(script, sizeof script, "POSIX path of (choose file with prompt \"%s\")", title);
    cui__shell_quote(quoted, sizeof quoted, script);
    snprintf(cmd, sizeof cmd, "osascript -e %s 2>/dev/null", quoted);
    FILE *f = popen(cmd, "r");
    if (!f) return false;
    if (!fgets(out, (int)cap, f)) out[0] = 0;
    int status = pclose(f);
    size_t n = strlen(out);
    while (n && (out[n - 1] == '\n' || out[n - 1] == '\r')) out[--n] = 0;
    return status == 0 && n > 0;
}

bool cui__plat_open_url(const char *url) {
    pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) { execlp("open", "open", url, (char *)NULL); _exit(127); }
    return true;
}
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
        snprintf(cmd, sizeof cmd, "kdialog %s %s --title %s 2>/dev/null", mode, o->default_name ? name : "''", title);
    } else {
        fprintf(stderr, "cui: install zenity or kdialog for file dialogs\n");
        return false;
    }
    FILE *f = popen(cmd, "r");
    if (!f) return false;
    if (!fgets(out, (int)cap, f)) out[0] = 0;
    int status = pclose(f);
    size_t n = strlen(out);
    while (n && (out[n - 1] == '\n' || out[n - 1] == '\r')) out[--n] = 0;
    return status == 0 && n > 0;
}

bool cui__plat_open_url(const char *url) {
    pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        execlp("xdg-open", "xdg-open", url, (char *)NULL);
        _exit(127);
    }
    return true;
}

#endif

bool cui__plat_file_dialog(cui_app *app, const cui_file_dialog_opts *o, char *out, size_t cap) {
    (void)app;
    return cui__posix_file_dialog(o, out, cap);
}
#endif
