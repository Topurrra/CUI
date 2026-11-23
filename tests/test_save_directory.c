/* Accept the proposed name in a real Save dialog. The chooser returns a path;
 * deciding whether the file can be created belongs to its caller. */
#include <cui/cui_app.h>
#include <windows.h>
#include <sddl.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

static HWND owner;
static bool accepted;

/* The modal loop runs timers on the calling thread. Click only our dialog. */
static void CALLBACK accept_name(HWND window, UINT msg, UINT_PTR timer, DWORD time) {
    (void)window; (void)msg; (void)time;
    HWND dialog = GetLastActivePopup(owner);
    if (dialog == owner || GetWindow(dialog, GW_OWNER) != owner || !IsWindowVisible(dialog)) return;
    HWND ok = GetDlgItem(dialog, IDOK);
    if (ok && IsWindowEnabled(ok)) {
        KillTimer(NULL, timer);
        accepted = PostMessageW(ok, BM_CLICK, 0, 0) != 0;
    }
}

int main(void) {
    WCHAR temp[MAX_PATH], folder[MAX_PATH], path[MAX_PATH];
    DWORD len = GetTempPathW(MAX_PATH, temp);
    if (!len || len + 75 >= MAX_PATH) return 1;
    swprintf(folder, MAX_PATH, L"%lscui-save-%lu-%llu", temp, GetCurrentProcessId(),
             (unsigned long long)GetTickCount64());
    swprintf(path, MAX_PATH, L"%ls\\notes-\u00e9.txt", folder);
    /* Deny FILE_ADD_FILE on this new directory only. The owner can remove it. */
    PSECURITY_DESCRIPTOR sd = NULL;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:P(D;;0x2;;;WD)(A;;FA;;;OW)(A;;FA;;;SY)", SDDL_REVISION_1, &sd, NULL)) return 1;
    SECURITY_ATTRIBUTES sa = { sizeof sa, sd, FALSE };
    BOOL made = CreateDirectoryW(folder, &sa);
    LocalFree(sd);
    if (!made) return 1;
    int result = 1;
    HANDLE probe = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, 0, NULL);
    if (probe != INVALID_HANDLE_VALUE) {
        CloseHandle(probe);
        DeleteFileW(path);
        fputs("Expected the directory to deny file creation\n", stderr);
    } else if (GetLastError() == ERROR_ACCESS_DENIED) {
        cui_app_desc desc = { .title = "Save directory test", .renderer = CUI_RENDERER_SOFTWARE };
        cui_app *app = cui_app_create(&desc);
        if (app) {
            owner = (HWND)cui_app_native_window(app);
            SetActiveWindow(owner);
            char expected[MAX_PATH * 4], actual[MAX_PATH * 4] = { 0 };
            cui_file_dialog_opts opts = { .kind = CUI_FILE_SAVE, .title = "Choose a destination",
                                          .default_name = expected };
            UINT_PTR timer = SetTimer(NULL, 0, 100, accept_name);
            if (timer && WideCharToMultiByte(CP_UTF8, 0, path, -1, expected, sizeof expected, NULL, NULL)) {
                bool chosen = cui_app_file_dialog(app, &opts, actual, sizeof actual);
                result = !(accepted && chosen && strcmp(expected, actual) == 0 &&
                           GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES);
                if (result) fprintf(stderr, "Expected '%s', Save returned '%s' (accepted=%d)\n",
                                    expected, actual, (int)accepted);
            }
            if (timer) KillTimer(NULL, timer);
            cui_app_destroy(app);
        }
    }
    if (!RemoveDirectoryW(folder)) result = 1;
    return result;
}
