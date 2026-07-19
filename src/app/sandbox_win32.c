/*
 * sandbox_win32.c — spawn vncworker inside a locked-down AppContainer.
 *
 * The worker parses untrusted RFB data, so it runs with the strongest process
 * confinement Windows offers:
 *   - AppContainer token (lowbox) with a SINGLE capability: internetClient
 *     (outbound TCP to the VNC server). No filesystem, registry, UI, or other
 *     network capability.
 *   - Process mitigation policies: ACG (no dynamically-generated executable
 *     memory), CIG (only Microsoft-signed images load), win32k syscall surface
 *     disabled, heap-terminate-on-corruption, forced ASLR / bottom-up ASLR,
 *     strict handle checks, extension-point disable, and strict Control Flow
 *     Guard.
 *   - Only the two IPC pipe ends and the framebuffer mapping are shared in; each
 *     is DACL'd to the AppContainer SID explicitly.
 *
 * Fails closed: if the AppContainer cannot be created the worker is not spawned
 * (unless built with VNC_ALLOW_UNSANDBOXED for local debugging).
 *
 * NOTE: this module targets the documented Win32 API surface and is pending
 * validation on a Windows host (it cannot be exercised in the Linux CI used for
 * the protocol core). See docs/TESTING.md, M2 checklist.
 */
#include "app/app.h"
#include "app/diag.h"

#include <userenv.h>
#include <aclapi.h>
#include <sddl.h>
#include <stdio.h>
#include <stdlib.h>

#pragma comment(lib, "userenv.lib")
#pragma comment(lib, "advapi32.lib")

#define APPCONTAINER_NAME    L"vnc-lightweight-x64.worker"
#define APPCONTAINER_DISPLAY L"VNC Lightweight Worker"

/* Add an allow-ACE for `sid` with `access` to a kernel object's DACL. */
static BOOL grant_sid_to_handle(HANDLE obj, PSID sid, DWORD access)
{
    PACL old_dacl = NULL, new_dacl = NULL;
    PSECURITY_DESCRIPTOR sd = NULL;
    BOOL ok = FALSE;

    if (GetSecurityInfo(obj, SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION,
                        NULL, NULL, &old_dacl, NULL, &sd) != ERROR_SUCCESS)
        return FALSE;

    EXPLICIT_ACCESSW ea;
    ZeroMemory(&ea, sizeof(ea));
    ea.grfAccessPermissions = access;
    ea.grfAccessMode = GRANT_ACCESS;
    ea.grfInheritance = NO_INHERITANCE;
    ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    ea.Trustee.TrusteeType = TRUSTEE_IS_GROUP;
    ea.Trustee.ptstrName = (LPWSTR)sid;

    if (SetEntriesInAclW(1, &ea, old_dacl, &new_dacl) != ERROR_SUCCESS)
        goto out;
    if (SetSecurityInfo(obj, SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION,
                        NULL, NULL, new_dacl, NULL) != ERROR_SUCCESS)
        goto out;
    ok = TRUE;
out:
    if (sd) LocalFree(sd);
    if (new_dacl) LocalFree(new_dacl);
    return ok;
}

/* Grant an AppContainer SID read access to a specific file by path, so the
 * sandboxed worker (which otherwise has no filesystem access) can open e.g. the
 * CA bundle. Narrow: grants only our worker's package SID, read-only. */
static void grant_sid_read_to_file(const char *utf8_path, PSID sid)
{
    if (!utf8_path || !sid)
        return;
    wchar_t wpath[1024];
    if (MultiByteToWideChar(CP_UTF8, 0, utf8_path, -1, wpath, 1024) <= 0)
        return;
    HANDLE h = CreateFileW(wpath, READ_CONTROL | WRITE_DAC, FILE_SHARE_READ,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return;
    grant_sid_to_handle(h, sid, FILE_GENERIC_READ);
    CloseHandle(h);
}

/* Create (or reuse) the AppContainer profile and return its SID (LocalFree it,
 * or FreeSid depending on source — see caller). */
static PSID create_appcontainer_sid(void)
{
    PSID sid = NULL;
    HRESULT hr = CreateAppContainerProfile(APPCONTAINER_NAME, APPCONTAINER_DISPLAY,
                                           APPCONTAINER_DISPLAY, NULL, 0, &sid);
    if (hr == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS)) {
        if (FAILED(DeriveAppContainerSidFromAppContainerName(APPCONTAINER_NAME, &sid)))
            return NULL;
    } else if (FAILED(hr)) {
        return NULL;
    }
    return sid; /* free with FreeSid() */
}

static BOOL utf8_to_wide(const char *s, wchar_t *out, int out_chars)
{
    return MultiByteToWideChar(CP_UTF8, 0, s, -1, out, out_chars) > 0;
}

BOOL sandbox_spawn_worker(ViewerApp *app, const WorkerSpawnParams *p)
{
    BOOL result = FALSE;
    PSID ac_sid = NULL;
    HANDLE cmd_rd = NULL, cmd_wr = NULL, evt_rd = NULL, evt_wr = NULL;
    HANDLE fbmap_inh = NULL;
    LPPROC_THREAD_ATTRIBUTE_LIST attrs = NULL;
    SID_AND_ATTRIBUTES cap = {0};
    PSID inet_sid = NULL;

    diag_logf(DIAG_INFO, "sandbox: creating AppContainer profile");
    ac_sid = create_appcontainer_sid();
    if (!ac_sid) {
        diag_win32("sandbox: CreateAppContainerProfile", GetLastError());
        fprintf(stderr, "sandbox: cannot create AppContainer profile (err %lu)\n",
                GetLastError());
#ifndef VNC_ALLOW_UNSANDBOXED
        return FALSE; /* fail closed */
#else
        diag_logf(DIAG_WARN, "sandbox: continuing UNSANDBOXED (debug build)");
#endif
    } else {
        diag_logf(DIAG_INFO, "sandbox: AppContainer SID acquired");
    }

    /* Two anonymous pipes => a duplex channel. Worker ends are inheritable. */
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    if (!CreatePipe(&cmd_rd, &cmd_wr, &sa, 0)) goto cleanup; /* UI writes cmd_wr */
    if (!CreatePipe(&evt_rd, &evt_wr, &sa, 0)) goto cleanup; /* UI reads evt_rd */
    /* UI-side ends must NOT be inherited by the worker. */
    SetHandleInformation(cmd_wr, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(evt_rd, HANDLE_FLAG_INHERIT, 0);

    /* The worker gets the framebuffer as an INHERITED handle (an AppContainer
     * cannot open the Local\ mapping by name). Duplicate an inheritable copy and
     * also grant the AppContainer SID on the underlying section object so the
     * lowbox token may use it. */
    HANDLE fbmap = (HANDLE)vnc_shm_native_handle(app->shm);
    if (!fbmap ||
        !DuplicateHandle(GetCurrentProcess(), fbmap, GetCurrentProcess(),
                         &fbmap_inh, 0, TRUE, DUPLICATE_SAME_ACCESS)) {
        fprintf(stderr, "sandbox: cannot duplicate framebuffer handle\n");
        goto cleanup;
    }
    if (ac_sid) {
        /* The framebuffer section is created once and persists across reconnects;
         * grant its DACL only once (the SID is profile-stable) so repeated
         * reconnects don't keep re-adding ACEs to a long-lived object. The pipe
         * ends are fresh each spawn, so they must be granted every time. */
        if (!app->fb_granted) {
            grant_sid_to_handle(fbmap, ac_sid, FILE_MAP_READ | FILE_MAP_WRITE);
            app->fb_granted = TRUE;
        }
        grant_sid_to_handle(cmd_rd, ac_sid, GENERIC_READ | SYNCHRONIZE);
        grant_sid_to_handle(evt_wr, ac_sid, GENERIC_WRITE | SYNCHRONIZE);
        /* The worker must read the CA bundle for VeNCrypt X509; grant its
         * package SID read access to that one file. */
        if (p->ca_file)
            grant_sid_read_to_file(p->ca_file, ac_sid);
    }

    /* Build the extended startup info: security capabilities, mitigations,
     * and an explicit inherit-only-these-handles list. */
    SIZE_T attr_size = 0;
    InitializeProcThreadAttributeList(NULL, 3, 0, &attr_size);
    attrs = (LPPROC_THREAD_ATTRIBUTE_LIST)HeapAlloc(GetProcessHeap(), 0, attr_size);
    if (!attrs) goto cleanup;
    if (!InitializeProcThreadAttributeList(attrs, 3, 0, &attr_size)) {
        /* Not initialized: free the raw memory but do NOT DeleteProcThread... it
         * (that would walk an uninitialized list header). */
        HeapFree(GetProcessHeap(), 0, attrs);
        attrs = NULL;
        goto cleanup;
    }

    /* (1) Security capabilities: AppContainer SID + internetClient capability. */
    SECURITY_CAPABILITIES sec_caps = {0};
    if (ac_sid) {
        SID_IDENTIFIER_AUTHORITY app_authority = SECURITY_APP_PACKAGE_AUTHORITY;
        if (!AllocateAndInitializeSid(&app_authority,
                SECURITY_BUILTIN_CAPABILITY_RID_COUNT,
                SECURITY_CAPABILITY_BASE_RID, SECURITY_CAPABILITY_INTERNET_CLIENT,
                0, 0, 0, 0, 0, 0, &inet_sid))
            goto cleanup;
        cap.Sid = inet_sid;
        cap.Attributes = SE_GROUP_ENABLED;
        sec_caps.AppContainerSid = ac_sid;
        sec_caps.Capabilities = &cap;
        sec_caps.CapabilityCount = 1;
        if (!UpdateProcThreadAttribute(attrs, 0,
                PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES,
                &sec_caps, sizeof(sec_caps), NULL, NULL))
            goto cleanup;
    }

    /* (2) Process mitigation policies (two DWORD64 words: policy + policy2). */
    DWORD64 mit[2];
    mit[0] =
        PROCESS_CREATION_MITIGATION_POLICY_PROHIBIT_DYNAMIC_CODE_ALWAYS_ON |
        PROCESS_CREATION_MITIGATION_POLICY_BLOCK_NON_MICROSOFT_BINARIES_ALWAYS_ON |
        PROCESS_CREATION_MITIGATION_POLICY_WIN32K_SYSTEM_CALL_DISABLE_ALWAYS_ON |
        PROCESS_CREATION_MITIGATION_POLICY_HEAP_TERMINATE_ALWAYS_ON |
        PROCESS_CREATION_MITIGATION_POLICY_BOTTOM_UP_ASLR_ALWAYS_ON |
        PROCESS_CREATION_MITIGATION_POLICY_FORCE_RELOCATE_IMAGES_ALWAYS_ON |
        PROCESS_CREATION_MITIGATION_POLICY_HIGH_ENTROPY_ASLR_ALWAYS_ON |
        PROCESS_CREATION_MITIGATION_POLICY_STRICT_HANDLE_CHECKS_ALWAYS_ON |
        PROCESS_CREATION_MITIGATION_POLICY_EXTENSION_POINT_DISABLE_ALWAYS_ON;
    mit[1] =
        PROCESS_CREATION_MITIGATION_POLICY2_STRICT_CONTROL_FLOW_GUARD_ALWAYS_ON |
        PROCESS_CREATION_MITIGATION_POLICY2_CET_USER_SHADOW_STACKS_ALWAYS_ON;
    if (!UpdateProcThreadAttribute(attrs, 0,
            PROC_THREAD_ATTRIBUTE_MITIGATION_POLICY,
            mit, sizeof(mit), NULL, NULL))
        goto cleanup;

    /* (3) Inherit exactly the two worker pipe ends and the framebuffer mapping. */
    HANDLE inherit[3] = { cmd_rd, evt_wr, fbmap_inh };
    if (!UpdateProcThreadAttribute(attrs, 0,
            PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
            inherit, sizeof(inherit), NULL, NULL))
        goto cleanup;

    /* Single-executable model: re-launch OURSELVES with a hidden --worker flag to
     * become the sandboxed decoder child (Chromium-style). exe_path is this exact
     * image; exe_dir is its folder (used as the child's working directory). The
     * GUI DLLs (user32/gdi32/...) are delay-loaded, so the worker never loads them
     * and the no-win32k mitigation holds. */
    wchar_t exe_path[MAX_PATH], exe_dir[MAX_PATH];
    GetModuleFileNameW(NULL, exe_path, MAX_PATH);
    wcsncpy_s(exe_dir, MAX_PATH, exe_path, _TRUNCATE);
    wchar_t *slash = wcsrchr(exe_dir, L'\\');
    if (slash) *slash = 0;

    /* All string values are DOUBLE-QUOTED on the command line: the worker's arg
     * parser takes the single next token as the value, so an unquoted multi-word
     * value (the default --encodings list is multi-word, and a host could contain
     * a space) would be split and mis-parsed. Quote host and encodings as --ca
     * already is. (Values here originate from our own UI/config, not the server.) */
    wchar_t whost[256], enc_arg[540] = L"", ca_arg[1060] = L"";
    utf8_to_wide(p->host, whost, 256);
    if (p->encodings) {
        wchar_t wenc[512];
        utf8_to_wide(p->encodings, wenc, 512);
        _snwprintf_s(enc_arg, 540, _TRUNCATE, L" --encodings \"%s\"", wenc);
    }
    if (p->ca_file) {
        wchar_t wca[1024];
        utf8_to_wide(p->ca_file, wca, 1024);
        _snwprintf_s(ca_arg, 1060, _TRUNCATE, L" --ca \"%s\"", wca); /* quote: may contain spaces */
    }

    /* Handle values are process-local numbers valid in the child because they
     * are inherited (pipes + framebuffer mapping). */
    wchar_t cmdline[2600];
    _snwprintf_s(cmdline, 2600, _TRUNCATE,
        L"\"%s\" --worker --shm-handle %llu --shm-bytes %zu --host \"%s\" --port %d "
        L"--rd %llu --wr %llu%s%s%s%s",
        exe_path,
        (unsigned long long)(uintptr_t)fbmap_inh,
        p->shm_bytes, whost, p->port,
        (unsigned long long)(uintptr_t)cmd_rd,
        (unsigned long long)(uintptr_t)evt_wr,
        enc_arg,
        p->view_only ? L" --view-only" : L"",
        p->audio ? L" --audio" : L"",
        ca_arg);

    STARTUPINFOEXW si = {0};
    si.StartupInfo.cb = sizeof(si);
    si.lpAttributeList = attrs;
    PROCESS_INFORMATION pi = {0};

    diag_logf(DIAG_INFO, "sandbox: launching worker (ACG/CIG/no-win32k/CFG/CET)");
    if (!CreateProcessW(exe_path, cmdline, NULL, NULL, TRUE,
                        EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW,
                        NULL, exe_dir, &si.StartupInfo, &pi)) {
        DWORD e = GetLastError();
        diag_win32("sandbox: CreateProcess (worker)", e);
        diag_logf(DIAG_ERROR, "sandbox: worker spawn failed \xE2\x80\x94 if this is "
                  "ERROR_ACCESS_DENIED/1058, check vncviewer.exe was built "
                  "/guard:cf /CETCOMPAT (the worker is this same image re-launched "
                  "with --worker)");
        fprintf(stderr, "sandbox: CreateProcess failed (err %lu)\n", e);
        goto cleanup;
    }
    CloseHandle(pi.hThread);
    app->worker_process = pi.hProcess;
    diag_logf(DIAG_INFO, "sandbox: worker started (pid=%lu)",
              (unsigned long)pi.dwProcessId);

    /* UI keeps its own ends; the worker's ends are now the child's. */
    app->ch.wr = (vnc_handle)cmd_wr; cmd_wr = NULL;
    app->ch.rd = (vnc_handle)evt_rd; evt_rd = NULL;
    result = TRUE;

cleanup:
    if (!result)
        diag_win32("sandbox: spawn failed at setup", GetLastError());
    if (cmd_rd) CloseHandle(cmd_rd);   /* child got its own inherited copy */
    if (evt_wr) CloseHandle(evt_wr);
    if (fbmap_inh) CloseHandle(fbmap_inh);
    if (!result) {
        if (cmd_wr) CloseHandle(cmd_wr);
        if (evt_rd) CloseHandle(evt_rd);
    }
    if (attrs) { DeleteProcThreadAttributeList(attrs); HeapFree(GetProcessHeap(), 0, attrs); }
    if (inet_sid) FreeSid(inet_sid);
    if (ac_sid) FreeSid(ac_sid);
    return result;
}

void sandbox_cleanup(ViewerApp *app)
{
    /* Close the COMMAND (write) end first and drive the worker to exit; the
     * reader thread is blocked in ReadFile on ch.rd, and closing a handle that
     * another thread is actively reading is undefined on Windows. The worker's
     * exit closes its event-write end, which delivers EOF and unblocks the reader
     * cleanly — so we only close ch.rd LAST, once the worker is gone and the
     * reader is no longer inside that ReadFile. */
    if (app->ch.wr) { CloseHandle((HANDLE)app->ch.wr); app->ch.wr = 0; }
    if (app->worker_process) {
        /* Ask nicely first (SHUTDOWN was sent by the caller), then ensure exit.
         * The worker's exit code is a useful triage signal — e.g. a nonzero code
         * with no prior "connected" status often means it died at startup. */
        DWORD code = 0;
        if (WaitForSingleObject(app->worker_process, 2000) == WAIT_TIMEOUT) {
            diag_logf(DIAG_WARN, "worker did not exit; terminating");
            TerminateProcess(app->worker_process, 1);
        } else if (GetExitCodeProcess(app->worker_process, &code)) {
            diag_logf(DIAG_INFO, "worker exited (code=%lu)", (unsigned long)code);
        }
        CloseHandle(app->worker_process);
        app->worker_process = NULL;
    }
    if (app->ch.rd) { CloseHandle((HANDLE)app->ch.rd); app->ch.rd = 0; }
}
