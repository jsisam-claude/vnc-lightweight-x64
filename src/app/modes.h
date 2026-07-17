/*
 * modes.h — entry points for the single-executable multi-mode build.
 *
 * On Windows the product ships as ONE vncviewer.exe that runs in three modes,
 * dispatched in wWinMain by the command line:
 *   (default)    the trusted UI/broker
 *   --worker ..  the sandboxed decoder child (the UI re-launches itself with
 *                this flag; see sandbox_win32.c) — Chromium-style
 *   --headless   the console diagnostic client (equivalent to the standalone
 *                vnctest used on Linux/CI)
 *
 * These two functions are the worker and headless mains, renamed so wWinMain
 * can call them. On Linux they remain separate executables via thin main()
 * shims guarded by VNC_WORKER_STANDALONE / VNC_HEADLESS_STANDALONE.
 */
#ifndef VNC_APP_MODES_H
#define VNC_APP_MODES_H

int vnc_worker_main(int argc, char **argv);
int vnc_headless_main(int argc, char **argv);

#endif /* VNC_APP_MODES_H */
