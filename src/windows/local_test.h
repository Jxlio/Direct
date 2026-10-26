#ifndef DIRECT_LOCAL_TEST_H
#define DIRECT_LOCAL_TEST_H
#include <windows.h>
int local_test_start(HWND owner,int responder);
int local_test_launch(HWND owner);
void local_test_record_mode(int enabled);
void local_test_tick(void);
void local_test_close(void);
#endif
