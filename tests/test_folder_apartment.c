/* Select and cancel an owned Windows folder dialog from either COM apartment. */
#include <cui/cui_app.h>
#include <windows.h>
#include <shlobj.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

typedef struct {
    HWND owner;
    const WCHAR *folder;   /* NULL cancels */
} dialog_input;

/* Drive only the folder dialog belonging to this test's owner window. */
static DWORD WINAPI answer(void *data) {
    dialog_input *input = data;
    ULONGLONG until = GetTickCount64() + 8000;
    while (GetTickCount64() < until) {
        HWND dialog = GetLastActivePopup(input->owner);
        if (GetWindow(dialog, GW_OWNER) == input->owner && IsWindowVisible(dialog)) {
            HWND button = GetDlgItem(dialog, input->folder ? IDOK : IDCANCEL);
            if (button) {
                if (input->folder && !SendMessageTimeoutW(dialog, BFFM_SETSELECTIONW, TRUE,
                        (LPARAM)input->folder, SMTO_ABORTIFHUNG, 2000, NULL)) break;
                if (IsWindowEnabled(button) && PostMessageW(button, BM_CLICK, 0, 0)) return 0;
            }
        }
        Sleep(25);
    }
    fputs("The owned folder dialog did not respond\n", stderr);
    fflush(stderr);
    return 1;
}

static bool choose(cui_app *app, const WCHAR *folder, const char *expected) {
    dialog_input input = { (HWND)cui_app_native_window(app), folder };
    SetActiveWindow(input.owner);
    HANDLE driver = CreateThread(NULL, 0, answer, &input, 0, NULL);
    if (!driver) return false;
    cui_file_dialog_opts opts = { .kind = CUI_FILE_FOLDER, .title = "Choose a test folder" };
    char actual[MAX_PATH * 4];
    bool chosen = cui_app_file_dialog(app, &opts, actual, sizeof actual);
    DWORD status = 1;
    WaitForSingleObject(driver, INFINITE);
    GetExitCodeThread(driver, &status);
    CloseHandle(driver);
    bool ok = !status && (expected ? chosen && strcmp(actual, expected) == 0 : !chosen);
    if (!ok) fprintf(stderr, "Folder dialog returned %d, path '%s', driver %lu\n",
                     (int)chosen, actual, status);
    return ok;
}

int main(int argc, char **argv) {
    WCHAR folder[MAX_PATH];
    DWORD n = GetTempPathW(MAX_PATH, folder);
    if (!n || n >= MAX_PATH) return 1;
    if (n > 3 && folder[n - 1] == L'\\') folder[n - 1] = 0;
    char expected[MAX_PATH * 4];
    if (!WideCharToMultiByte(CP_UTF8, 0, folder, -1, expected, sizeof expected, NULL, NULL)) return 1;
    DWORD apartment = argc > 1 && strcmp(argv[1], "sta") == 0 ? COINIT_APARTMENTTHREADED : COINIT_MULTITHREADED;
    if (FAILED(CoInitializeEx(NULL, apartment))) return 1;
    cui_app *app = cui_app_create(&(cui_app_desc){ .title = "Folder dialog test",
                                  .renderer = CUI_RENDERER_SOFTWARE, .continuous = true });
    bool ok = app && choose(app, folder, expected) && choose(app, NULL, NULL) && cui_app_frame(app);
    if (app) cui_app_destroy(app);
    CoUninitialize();
    return ok ? 0 : 1;
}
