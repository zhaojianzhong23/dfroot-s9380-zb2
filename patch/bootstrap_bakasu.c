#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define BLKROSET   0x125d
#define KSUD_STAGE "/data/user_de/0/df.root.bakasu/ksud"
#define KSUD       "/data/adb/ksud"
#define PREFS_PATH "/data/user_de/0/df.root.bakasu/shared_prefs/dfroot.xml"
#define MODULES_DIR "/data/adb/modules"
/* BakaSU build: A-stage Samsung-patched kernelsu.ko (android15-6.6) staged by
 * ExploitRunner from our own assets. Loaded via `ksud insmod` before late-load,
 * so late-load sees has_kernelsu()=true and skips the daemon's embedded 6.12 ko. */
#define KO_PATH "/data/user_de/0/df.root.bakasu/kernelsu.ko"

static int pref_true(const char *buf, const char *key)
{
    char needle[64];
    snprintf(needle, sizeof(needle), "name=\"%s\"", key);
    char *p = strstr(buf, needle);
    if (!p) return 0;
    char *tag_end = strchr(p, '>');
    char *v = strstr(p, "value=\"true\"");
    return v && tag_end && v < tag_end;
}

static int read_prefs(char *su_manager, size_t su_manager_size, int *soft_reboot,
                      int *disable_modules)
{
    int fd = open(PREFS_PATH, O_RDONLY);
    if (fd < 0) return -1;

    char buf[4096];
    int n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = '\0';

    char *p = strstr(buf, "name=\"su_manager\">");
    if (!p) return -1;
    p += strlen("name=\"su_manager\">");
    char *end = strchr(p, '<');
    if (!end) return -1;
    size_t len = end - p;
    if (len == 0 || len >= su_manager_size) return -1;
    memcpy(su_manager, p, len);
    su_manager[len] = '\0';

    *soft_reboot = pref_true(buf, "soft_reboot");
    *disable_modules = pref_true(buf, "disable_modules");

    return 0;
}

static int adopt_zygote_env(void)
{
    FILE *f = popen("pidof zygote64 zygote", "r");
    if (!f) return -1;
    int pid = 0;
    fscanf(f, "%d", &pid);
    pclose(f);
    if (!pid) return -1;

    char path[32];
    snprintf(path, sizeof(path), "/proc/%d/environ", pid);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    static char buf[16384];
    int n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = '\0';
    for (char *p = buf, *end = buf + n; p < end; p += strlen(p) + 1)
        putenv(p);
    return 0;
}

static int should_ro(const char *name)
{
    size_t len = strlen(name);
    if (!strcmp(name, "super"))  return 1;
    if (!strcmp(name, "misc"))   return 1;
    if (!strcmp(name, "steady")) return 1;
    if (len >= 2 && name[len - 2] == '_' &&
        (name[len - 1] == 'a' || name[len - 1] == 'b'))
        return 1;
    return 0;
}

static int set_partitions_ro(void)
{
    DIR *dir = opendir("/dev/block/by-name");
    if (!dir)
        return -1;

    struct dirent *ent;
    while ((ent = readdir(dir))) {
        if (!should_ro(ent->d_name))
            continue;

        char path[128];
        snprintf(path, sizeof(path), "/dev/block/by-name/%s", ent->d_name);

        int fd = open(path, O_RDONLY);
        if (fd < 0)
            continue;

        struct stat st;
        if (fstat(fd, &st) == 0 && S_ISBLK(st.st_mode)) {
            int on = 1;
            ioctl(fd, BLKROSET, &on);
        }
        close(fd);
    }

    closedir(dir);
    return 0;
}

static int late_load_running(void)
{
    DIR *d = opendir("/proc");
    if (!d)
        return 0;
    struct dirent *e;
    char path[64], buf[256];
    int found = 0;
    while ((e = readdir(d)) != NULL) {
        char *end;
        strtol(e->d_name, &end, 10);
        if (*end) continue;
        snprintf(path, sizeof(path), "/proc/%s/cmdline", e->d_name);
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) continue;
        int n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n <= 0) continue;
        buf[n] = '\0';
        for (int i = 0; i < n; i++)
            if (!buf[i]) buf[i] = ' ';
        if (strstr(buf, "ksud") && strstr(buf, "late-load")) {
            found = 1;
            break;
        }
    }
    closedir(d);
    return found;
}

static int manager_installed(const char *pkg)
{
    DIR *d = opendir("/data/app");
    if (!d) return 0;
    struct dirent *e;
    int found = 0;
    while ((e = readdir(d))) {
        if (strstr(e->d_name, pkg)) { found = 1; break; }
    }
    closedir(d);
    return found;
}

static int run_ctx(const char *ctx, char *const argv[])
{
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        if (ctx) {
            int fd = open("/proc/self/attr/exec", O_WRONLY);
            if (fd >= 0) { write(fd, ctx, strlen(ctx)); close(fd); }
        }
        execv(argv[0], argv);
        _exit(127);
    }
    int status;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static void touch(const char *path)
{
    int fd = open(path, O_CREAT | O_WRONLY, 0666);
    if (fd >= 0)
        close(fd);
}

/* Mark every installed module disabled before ksud runs. A broken module
 * otherwise loads on the next boot and bootloops the device. */
static int disable_modules(void)
{
    DIR *dir = opendir(MODULES_DIR);
    if (!dir)
        return -1;

    struct dirent *ent;
    while ((ent = readdir(dir))) {
        if (ent->d_name[0] == '.')
            continue;
        char path[256];
        snprintf(path, sizeof(path), MODULES_DIR "/%s/disable", ent->d_name);
        touch(path);
    }
    closedir(dir);
    return 0;
}

int main(void)
{
    touch("/dev/dfm1");
    char su_manager[256];
    int soft_reboot, disable_mods;
    if (read_prefs(su_manager, sizeof(su_manager), &soft_reboot, &disable_mods) != 0) {
        touch("/dev/dfme0");
        return 1;
    }

    touch("/dev/dfm2");
    if (adopt_zygote_env())
        touch("/dev/dfmw0");

    touch("/dev/dfm3");
    if (set_partitions_ro())
        touch("/dev/dfmw1");

    if (disable_mods) {
        touch("/dev/dfm4");
        if (disable_modules()) {
            touch("/dev/dfme1");
            return 1;
        }
    }

    touch("/dev/dfm5");
    /* BakaSU build: the bundled APK is the real BakaSU official build (display
     * name BakaSU, signature d3469712.. matches our ko static whitelist) from the
     * Najip/KernelSU_SUSFS_Samsung_SM-A325X release. That fork keeps the legacy
     * Android package id com.resukisu.resukisu even though it is BakaSU, so the
     * install check and late-load --package-name must use the REAL package id,
     * not org.bakasu.bakasu (which does not exist on device). */
    static char manager_pkg[] = "com.resukisu.resukisu";
    /* auto-reinstall the BakaSU manager if the user uninstalled it.
     * APK staged by ExploitRunner to a predictable private path. */
    static char mgr_apk[] = "/data/user_de/0/df.root.bakasu/ksumgr.apk";
    if (access(mgr_apk, F_OK) == 0 && !manager_installed(manager_pkg)) {
        touch("/dev/dfm7");
        if (run_ctx("u:r:shell:s0", (char *[]){"/system/bin/pm", "install", "-r", mgr_apk, NULL}) == 0)
            touch("/dev/dfm8");   /* v9: manager reinstalled via shell context */
        else if (run_ctx(NULL, (char *[]){"/system/bin/pm", "install", "-r", mgr_apk, NULL}) == 0)
            touch("/dev/dfm8");   /* v9: manager reinstalled via current context */
        else
            touch("/dev/dfmw2");  /* v9: pm install failed; continue anyway */
    }
    /* BakaSU: load the Samsung-patched 6.6 ko staged from our assets. This
     * daemon's embedded ko is the 6.12 build (S26U) and must NOT be used here.
     * insmod resolves symbols from /proc/kallsyms (proven working on this
     * device with the v7 Samsung build). Once loaded, late-load's has_kernelsu()
     * check returns true and it skips the embedded ko entirely. */
    if (access(KO_PATH, F_OK) == 0) {
        touch("/dev/dfka1");
        if (run_ctx(NULL, (char *[]){ KSUD_STAGE, "insmod", KO_PATH, NULL }) == 0)
            touch("/dev/dfka2");   /* ko loaded via insmod */
        else
            touch("/dev/dfkew");   /* insmod failed; late-load will try embedded ko */
    }
    int ko_loaded = (access("/dev/dfka2", F_OK) == 0);
    int late_rc = run_ctx(NULL, (char *[]){ KSUD_STAGE, "late-load", "--kmi", "android15-6.6", "--package-name", manager_pkg, NULL });
    /* late-load on this Samsung 6.6 build can return nonzero from unsupported
     * optional commands (e.g. SuSFS) even though the ko is loaded and root works.
     * Trust the ko-load marker over the exit code so the UI does not show a false
     * "ksud start: ERROR" while root is in fact fully functional. */
    if (late_rc == 0 || ko_loaded) {
        touch("/dev/dfm6");
        touch("/dev/dfm9");  /* BakaSU: late-load invoked with pinned manager package */
        /* BakaSU: explicitly launch the manager so the "auto-open after root"
         * behaviour matches v9. late-load's implicit start is unreliable here.
         * Use monkey -p (by package, LAUNCHER intent) so we don't hardcode the
         * activity class; fall back to am start -n if monkey is absent. */
        if (access("/system/bin/monkey", X_OK) == 0)
            run_ctx("u:r:shell:s0", (char *[]){ "/system/bin/monkey", "-p",
                "com.resukisu.resukisu", "-c", "android.intent.category.LAUNCHER", "1", NULL });
        else
            run_ctx("u:r:shell:s0", (char *[]){ "/system/bin/am", "start", "-n",
                "com.resukisu.resukisu/.ui.MainActivity", NULL });
        if (soft_reboot) {
            while (late_load_running()) sleep(1);
            run_ctx("u:r:ksu:s0", (char *[]){ KSUD, "soft-reboot", NULL });
        }
    } else {
        touch("/dev/dfme2");
    }
        
    return 0;
}
