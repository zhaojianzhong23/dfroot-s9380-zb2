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
#define ROOTDIR    "/data/user_de/0/df.root.unified"
#define KSUD_STAGE ROOTDIR "/ksud"
#define KSUD       "/data/adb/ksud"
#define PREFS_PATH ROOTDIR "/shared_prefs/dfroot.xml"
#define MODULES_DIR "/data/adb/modules"
#define KO_PATH    ROOTDIR "/kernelsu.ko"
#define MGR_APK    ROOTDIR "/ksumgr.apk"

/* Unified bootstrap for df.root.unified.
 * Flavor selected in the UI is stored in prefs as name="flavor">v9< or >bakasu<.
 *  - bakasu : stage the Samsung-patched BakaSU kernelsu.ko, insmod it, then
 *             late-load pinned to com.resukisu.resukisu (UAPI 6).
 *  - finasu : FinaSU ko rebuilt with Samsung KDP/DEFEX compat and the FinaSU
 *             manager certificate compiled in; FinaSU ksud, late-load without --kmi.
 *  - v9     : no standalone ko (Samsung ko is embedded in the v9 ksud),
 *             late-load pinned to me.weishu.kernelsu (UAPI 5).
 * Everything else (partition RO, zygote env, optional module-disable, soft reboot)
 * is shared. */

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

/* read a string pref: name="<key>">VALUE< */
static int pref_string(const char *buf, const char *key, char *out, size_t out_size)
{
    char needle[64];
    snprintf(needle, sizeof(needle), "name=\"%s\">", key);
    char *p = strstr(buf, needle);
    if (!p) return -1;
    p += strlen(needle);
    char *end = strchr(p, '<');
    if (!end) return -1;
    size_t len = end - p;
    if (len == 0 || len >= out_size) return -1;
    memcpy(out, p, len);
    out[len] = '\0';
    return 0;
}

static int read_prefs(char *su_manager, size_t su_manager_size, char *flavor,
                      size_t flavor_size, int *soft_reboot, int *disable_modules)
{
    int fd = open(PREFS_PATH, O_RDONLY);
    if (fd < 0) return -1;

    char buf[4096];
    int n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = '\0';

    if (pref_string(buf, "su_manager", su_manager, su_manager_size) != 0) return -1;
    /* flavor is optional: default to bakasu if absent */
    if (pref_string(buf, "flavor", flavor, flavor_size) != 0)
        snprintf(flavor, flavor_size, "bakasu");

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

static void launch_manager(const char *mgr_pkg)
{
    /* use monkey -p (LAUNCHER intent, no hardcoded activity); fall back to am */
    if (access("/system/bin/monkey", X_OK) == 0) {
        char *argv[] = { "/system/bin/monkey", "-p", (char *)mgr_pkg,
                         "-c", "android.intent.category.LAUNCHER", "1", NULL };
        run_ctx("u:r:shell:s0", argv);
    } else {
        run_ctx("u:r:shell:s0", (char *[]){ "/system/bin/am", "start", "-a",
            "android.intent.action.MAIN", "-c", "android.intent.category.LAUNCHER",
            "-p", (char *)mgr_pkg, NULL });
    }
}

int main(void)
{
    touch("/dev/dfm1");
    char su_manager[256];
    char flavor[32];
    int soft_reboot, disable_mods;
    if (read_prefs(su_manager, sizeof(su_manager), flavor, sizeof(flavor),
                   &soft_reboot, &disable_mods) != 0) {
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
    int is_bakasu  = (strcmp(flavor, "bakasu") == 0);
    int is_finasu  = (strcmp(flavor, "finasu") == 0);
    int use_samsung_ko = is_bakasu || is_finasu;

    /* auto-reinstall the selected manager APK if the user uninstalled it.
     * ExploitRunner stages the correct flavor's manager to MGR_APK. */
    if (access(MGR_APK, F_OK) == 0 && !manager_installed(su_manager)) {
        touch("/dev/dfm7");
        if (run_ctx("u:r:shell:s0", (char *[]){"/system/bin/pm", "install", "-r", MGR_APK, NULL}) == 0)
            touch("/dev/dfm8");
        else if (run_ctx(NULL, (char *[]){"/system/bin/pm", "install", "-r", MGR_APK, NULL}) == 0)
            touch("/dev/dfm8");
        else
            touch("/dev/dfmw2");
    }

    int ko_loaded = 0;
    int late_rc;
    if (use_samsung_ko) {
        /* BakaSU / FinaSU: insmod the Samsung-patched 6.6 ko staged from assets,
         * then late-load with --kmi so the daemon sees has_kernelsu()=true and
         * skips its embedded (wrong) ko. */
        if (access(KO_PATH, F_OK) == 0) {
            touch("/dev/dfka1");
            if (run_ctx(NULL, (char *[]){ KSUD_STAGE, "insmod", KO_PATH, NULL }) == 0)
                touch("/dev/dfka2");
            else
                touch("/dev/dfkew");
        }
        ko_loaded = (access("/dev/dfka2", F_OK) == 0);

        /* FinaSU ko has the FinaSU manager certificate compiled in statically,
         * so no dynamic-manager registration is needed. */
        if (is_bakasu) {
            late_rc = run_ctx(NULL, (char *[]){ KSUD_STAGE, "late-load", "--kmi",
                "android15-6.6", "--package-name", su_manager, NULL });
        } else {
            /* FinaSU ksud detects KMI itself and does not accept --kmi. */
            late_rc = run_ctx(NULL, (char *[]){ KSUD_STAGE, "late-load",
                "--package-name", su_manager, NULL });
        }
    } else {
        /* v9: Samsung ksud has the compatible ko embedded; late-load alone is
         * enough (same as the proven v9 build). */
        late_rc = run_ctx(NULL, (char *[]){ KSUD_STAGE, "late-load",
            "--package-name", su_manager, NULL });
    }

    /* late-load can return nonzero from unsupported optional commands (e.g.
     * SuSFS) even though root works. Trust the ko-load marker for BakaSU. */
    if (late_rc == 0 || ko_loaded) {
        touch("/dev/dfm6");
        touch("/dev/dfm9");
        launch_manager(su_manager);
        if (soft_reboot) {
            while (late_load_running()) sleep(1);
            run_ctx("u:r:ksu:s0", (char *[]){ KSUD, "soft-reboot", NULL });
        }
    } else {
        touch("/dev/dfme2");
    }

    return 0;
}
