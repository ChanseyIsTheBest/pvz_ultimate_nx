/* android_pm.h -- PackageManager / PackageInfo / ApplicationInfo.
 *
 * Options -> Advanced calls getPackageManager() for the version string. It
 * returned null and the managed null check on the result killed the process;
 * see android_pm.c for the derivation.
 */
#ifndef PVZU_ANDROID_PM_H
#define PVZU_ANDROID_PM_H

#include <jni.h>

void android_pm_init(void);

/* For Context.getPackageManager() and Context.getApplicationInfo(). */
jobject android_package_manager(void);
jobject android_application_info(void);

#endif /* PVZU_ANDROID_PM_H */
