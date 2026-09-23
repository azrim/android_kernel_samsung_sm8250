#ifndef __KSU_H_FEATURE_SUSFS
#define __KSU_H_FEATURE_SUSFS

// entry point called from sys_prctl() in kernel/sys.c, mirroring how
// ksu_handle_sys_reboot() is called from sys_reboot()
int ksu_handle_prctl(int option, unsigned long arg2, unsigned long arg3,
		     unsigned long arg4, unsigned long arg5);

#endif
