#ifndef DIRECT_UPDATES_H
#define DIRECT_UPDATES_H
#include <windows.h>
#define WM_DIRECT_UPDATE (WM_APP + 20)
void updates_init(HWND parent);
/* Returns 1 only for the private updater-helper invocation (before UI startup). */
int updates_apply_command(void);
/* Called on normal exit; installs a staged update only after the process exits. */
void updates_shutdown(void);
void updates_web_snapshot(WCHAR status[512],int *available,int *busy);
void updates_web_check(void);
void updates_web_download(void);
#endif
