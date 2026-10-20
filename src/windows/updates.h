#ifndef DIRECT_UPDATES_H
#define DIRECT_UPDATES_H
#include <windows.h>
#define WM_DIRECT_UPDATE (WM_APP + 20)
void updates_init(HWND parent);
void updates_show(HWND parent);
void updates_refresh(void);
int updates_translate(MSG *message);
/* Returns 1 only for the private updater-helper invocation (before UI startup). */
int updates_apply_command(void);
/* Called on normal exit; installs a staged update only after the process exits. */
void updates_shutdown(void);
#endif
