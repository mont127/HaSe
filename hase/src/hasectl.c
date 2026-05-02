#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

typedef struct hase_config {
    char name[128];
    char vm_name[160];
    char root[PATH_MAX];
    char bottle[PATH_MAX];
    char lima_yaml[PATH_MAX];
} hase_config_t;

static void die(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

static void usage(FILE *out) {
    fprintf(out,
        "Usage:\n"
        "  hasectl init <bottle> [--root DIR]\n"
        "  hasectl start <bottle> [--root DIR]\n"
        "  hasectl stop <bottle> [--root DIR]\n"
        "  hasectl status <bottle> [--root DIR]\n"
        "  hasectl shell <bottle> [--root DIR]\n"
        "  hasectl steam <bottle> [--root DIR]\n"
        "  hasectl demo-window <bottle> [--root DIR]\n"
        "  hasectl windows <bottle> [--root DIR]\n"
        "  hasectl install-icd <bottle> [--root DIR]\n"
        "  hasectl install-fex <bottle> [--root DIR]\n"
        "  hasectl prepare-dxvk-smoke <bottle> [--root DIR]\n"
        "  hasectl run-clear-demo <bottle> [--root DIR]\n"
        "  hasectl run-triangle-demo <bottle> [--root DIR]\n"
        "  hasectl run-dxvk-smoke <bottle> [--root DIR]\n"
        "  hasectl paths <bottle> [--root DIR]\n"
        "\n"
        "Environment:\n"
        "  HASE_ROOT overrides the default bottle root.\n"
        "\n"
        "This is a Lima-backed prototype. Install Lima separately and keep the\n"
        "Linux VM hidden from users; HaSe owns the bottle and launch lifecycle.\n");
}

static bool valid_name(const char *name) {
    if (!name || !*name || strlen(name) >= 120) return false;
    for (const char *p = name; *p; ++p) {
        if (!isalnum((unsigned char)*p) && *p != '-' && *p != '_')
            return false;
    }
    return true;
}

static void path_join(char *out, size_t out_size, const char *a, const char *b) {
    int n = snprintf(out, out_size, "%s/%s", a, b);
    if (n < 0 || (size_t)n >= out_size) die("path too long");
}

static bool exists_dir(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static void mkdir_one(const char *path) {
    if (mkdir(path, 0755) == 0) return;
    if (errno == EEXIST && exists_dir(path)) return;
    die("mkdir(%s): %s", path, strerror(errno));
}

static void mkdir_p(const char *path) {
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof tmp, "%s", path);
    size_t len = strlen(tmp);
    if (len == 0) return;
    if (len > 1 && tmp[len - 1] == '/') tmp[len - 1] = '\0';

    for (char *p = tmp + 1; *p; ++p) {
        if (*p == '/') {
            *p = '\0';
            mkdir_one(tmp);
            *p = '/';
        }
    }
    mkdir_one(tmp);
}

static void write_file(const char *path, const char *data, mode_t mode) {
    FILE *f = fopen(path, "wb");
    if (!f) die("open(%s): %s", path, strerror(errno));
    size_t len = strlen(data);
    if (fwrite(data, 1, len, f) != len) {
        fclose(f);
        die("write(%s): %s", path, strerror(errno));
    }
    if (fclose(f) != 0) die("close(%s): %s", path, strerror(errno));
    if (chmod(path, mode) != 0) die("chmod(%s): %s", path, strerror(errno));
}

static void appendf(char **buf, size_t *cap, size_t *pos, const char *fmt, ...) {
    for (;;) {
        if (*pos + 256 > *cap) {
            *cap *= 2;
            char *n = (char *)realloc(*buf, *cap);
            if (!n) die("out of memory");
            *buf = n;
        }
        va_list ap;
        va_start(ap, fmt);
        int n = vsnprintf(*buf + *pos, *cap - *pos, fmt, ap);
        va_end(ap);
        if (n < 0) die("format error");
        if (*pos + (size_t)n < *cap) {
            *pos += (size_t)n;
            return;
        }
        *cap *= 2;
        char *nb = (char *)realloc(*buf, *cap);
        if (!nb) die("out of memory");
        *buf = nb;
    }
}

static char *yaml_quote(const char *s) {
    size_t cap = strlen(s) * 2 + 3;
    char *out = (char *)malloc(cap);
    if (!out) die("out of memory");
    size_t pos = 0;
    out[pos++] = '"';
    for (const char *p = s; *p; ++p) {
        if (pos + 3 >= cap) {
            cap *= 2;
            char *n = (char *)realloc(out, cap);
            if (!n) die("out of memory");
            out = n;
        }
        if (*p == '\\' || *p == '"') out[pos++] = '\\';
        out[pos++] = *p;
    }
    out[pos++] = '"';
    out[pos] = '\0';
    return out;
}

static void default_root(char *out, size_t out_size) {
    const char *env = getenv("HASE_ROOT");
    if (env && *env) {
        snprintf(out, out_size, "%s", env);
        return;
    }
    const char *home = getenv("HOME");
    if (!home || !*home) die("HOME is not set; pass --root DIR");
    int n = snprintf(out, out_size,
                     "%s/Library/Application Support/HaSe/bottles", home);
    if (n < 0 || (size_t)n >= out_size) die("default root path too long");
}

static void parse_common(int argc, char **argv, hase_config_t *cfg) {
    if (argc < 3) {
        usage(stderr);
        exit(2);
    }
    memset(cfg, 0, sizeof *cfg);
    if (!valid_name(argv[2]))
        die("invalid bottle name '%s' (use letters, numbers, '-' or '_')", argv[2]);
    snprintf(cfg->name, sizeof cfg->name, "%s", argv[2]);
    snprintf(cfg->vm_name, sizeof cfg->vm_name, "hase-%s", cfg->name);
    default_root(cfg->root, sizeof cfg->root);

    for (int i = 3; i < argc; ++i) {
        if (!strcmp(argv[i], "--root")) {
            if (++i >= argc) die("--root requires a directory");
            snprintf(cfg->root, sizeof cfg->root, "%s", argv[i]);
        } else {
            die("unknown argument: %s", argv[i]);
        }
    }

    path_join(cfg->bottle, sizeof cfg->bottle, cfg->root, cfg->name);
    char vm_dir[PATH_MAX];
    path_join(vm_dir, sizeof vm_dir, cfg->bottle, "vm");
    path_join(cfg->lima_yaml, sizeof cfg->lima_yaml, vm_dir, "lima.yaml");
}

static int run_wait(char *const argv[]) {
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return 127;
    }
    if (pid == 0) {
        execvp(argv[0], argv);
        fprintf(stderr, "exec %s: %s\n", argv[0], strerror(errno));
        _exit(127);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR) continue;
        perror("waitpid");
        return 127;
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 127;
}

static void ensure_bottle_exists(const hase_config_t *cfg) {
    if (!exists_dir(cfg->bottle))
        die("bottle does not exist: %s\nRun: hasectl init %s", cfg->bottle, cfg->name);
}

static bool lima_instance_exists(const hase_config_t *cfg) {
    const char *home = getenv("HOME");
    if (!home || !*home) return false;
    char lima_root[PATH_MAX], instance_dir[PATH_MAX];
    path_join(lima_root, sizeof lima_root, home, ".lima");
    path_join(instance_dir, sizeof instance_dir, lima_root, cfg->vm_name);
    return exists_dir(instance_dir);
}

static void write_lima_yaml(const hase_config_t *cfg) {
    char *bottle = yaml_quote(cfg->bottle);
    const char *image =
        "https://cloud-images.ubuntu.com/releases/24.04/release/ubuntu-24.04-server-cloudimg-arm64.img";
    size_t cap = 8192;
    size_t pos = 0;
    char *y = (char *)calloc(1, cap);
    if (!y) die("out of memory");

    appendf(&y, &cap, &pos,
        "# Generated by hasectl. This is the first HaSe Linux VM prototype.\n"
        "vmType: \"vz\"\n"
        "arch: \"aarch64\"\n"
        "cpus: 4\n"
        "memory: \"4GiB\"\n"
        "disk: \"24GiB\"\n"
        "images:\n"
        "- location: \"%s\"\n"
        "  arch: \"aarch64\"\n",
        image);

    appendf(&y, &cap, &pos,
        "mounts:\n"
        "- location: %s\n"
        "  mountPoint: \"/mnt/hase\"\n"
        "  writable: true\n"
        "containerd:\n"
        "  system: false\n"
        "  user: false\n"
        "ssh:\n"
        "  localPort: 0\n"
        "provision:\n"
        "- mode: system\n"
        "  script: |\n"
        "    #!/bin/sh\n"
        "    set -eu\n"
        "    export DEBIAN_FRONTEND=noninteractive\n"
        "    apt-get update\n"
        "    apt-get install -y --no-install-recommends \\\n"
        "      ca-certificates curl dbus-x11 openbox pulseaudio-utils \\\n"
        "      netpbm vulkan-tools wmctrl x11-apps x11-utils \\\n"
        "      x11-xserver-utils xdotool xvfb\n"
        "message: |\n"
        "  HaSe bottle %s is ready.\n"
        "  Start the hidden X11 session with: /mnt/hase/runtime/start-session.sh\n",
        bottle, cfg->name);

    write_file(cfg->lima_yaml, y, 0644);
    free(bottle);
    free(y);
}

static void write_runtime_scripts(const hase_config_t *cfg) {
    char runtime[PATH_MAX], path[PATH_MAX];
    path_join(runtime, sizeof runtime, cfg->bottle, "runtime");

    path_join(path, sizeof path, runtime, "start-session.sh");
    write_file(path,
        "#!/bin/sh\n"
        "set -eu\n"
        "export DISPLAY=\"${HASE_DISPLAY:-:99}\"\n"
        "export CHEESEBRIDGE_HOST=\"${CHEESEBRIDGE_HOST:-tcp:host.lima.internal:43210}\"\n"
        "mkdir -p /tmp/hase\n"
        "if ! pgrep -f \"Xvfb ${DISPLAY}\" >/dev/null 2>&1; then\n"
        "  Xvfb \"${DISPLAY}\" -screen 0 \"${HASE_GEOMETRY:-1920x1080x24}\" -nolisten tcp >/tmp/hase/xvfb.log 2>&1 &\n"
        "fi\n"
        "sleep 1\n"
        "if command -v xsetroot >/dev/null 2>&1; then\n"
        "  xsetroot -display \"${DISPLAY}\" -solid black || true\n"
        "fi\n"
        "if ! DISPLAY=\"${DISPLAY}\" wmctrl -m >/dev/null 2>&1; then\n"
        "  DISPLAY=\"${DISPLAY}\" openbox >/tmp/hase/openbox.log 2>&1 &\n"
        "fi\n"
        "i=0\n"
        "while ! DISPLAY=\"${DISPLAY}\" wmctrl -m >/dev/null 2>&1; do\n"
        "  i=$((i + 1))\n"
        "  [ \"$i\" -ge 50 ] && break\n"
        "  sleep 0.1\n"
        "done\n"
        "printf '%s\\n' \"${DISPLAY}\" >/tmp/hase/display\n"
        "printf 'HaSe hidden X11 session running on %s\\n' \"${DISPLAY}\"\n",
        0755);

    path_join(path, sizeof path, runtime, "stop-session.sh");
    write_file(path,
        "#!/bin/sh\n"
        "set -eu\n"
        "export DISPLAY=\"${HASE_DISPLAY:-:99}\"\n"
        "pkill -f \"openbox\" >/dev/null 2>&1 || true\n"
        "pkill -f \"Xvfb ${DISPLAY}\" >/dev/null 2>&1 || true\n"
        "rm -rf /tmp/hase\n"
        "printf 'HaSe hidden X11 session stopped\\n'\n",
        0755);

    path_join(path, sizeof path, runtime, "window-snapshot.sh");
    write_file(path,
        "#!/bin/sh\n"
        "set -eu\n"
        "export DISPLAY=\"${HASE_DISPLAY:-:99}\"\n"
        "if ! command -v wmctrl >/dev/null 2>&1; then\n"
        "  echo 'wmctrl is not installed' >&2\n"
        "  exit 1\n"
        "fi\n"
        "DISPLAY=\"${DISPLAY}\" wmctrl -lG -p | awk '\n"
        "BEGIN { printf(\"linux_window_id\\tprocess_id\\tx\\ty\\twidth\\theight\\ttitle\\n\") }\n"
        "{ id=$1; pid=$3; x=$4; y=$5; w=$6; h=$7; title=\"\"; for (i=9; i<=NF; ++i) title=title (i==9 ? \"\" : \" \") $i; printf(\"%s\\t%s\\t%s\\t%s\\t%s\\t%s\\t%s\\n\", id, pid, x, y, w, h, title) }'\n",
        0755);

    path_join(path, sizeof path, runtime, "capture-window-png.sh");
    write_file(path,
        "#!/bin/sh\n"
        "set -eu\n"
        "if [ \"$#\" -ne 1 ]; then\n"
        "  echo 'usage: capture-window-png.sh WINDOW_ID' >&2\n"
        "  exit 2\n"
        "fi\n"
        "export DISPLAY=\"${HASE_DISPLAY:-:99}\"\n"
        "for tool in xwd xwdtopnm pnmtopng; do\n"
        "  if ! command -v \"$tool\" >/dev/null 2>&1; then\n"
        "    echo \"$tool is not installed; install x11-apps and netpbm in the HaSe VM\" >&2\n"
        "    exit 2\n"
        "  fi\n"
        "done\n"
        "DISPLAY=\"${DISPLAY}\" xwd -silent -id \"$1\" | xwdtopnm 2>/dev/null | pnmtopng -force\n",
        0755);

    path_join(path, sizeof path, runtime, "install-icd.sh");
    write_file(path,
        "#!/bin/sh\n"
        "# Reads a CheeseBridge source tarball on stdin, builds the guest ICD\n"
        "# inside the VM, and installs the .so + manifest into the bottle's\n"
        "# vulkan/icd.d/ so any guest process can find it via VK_ICD_FILENAMES.\n"
        "set -eu\n"
        "SRCDIR=/tmp/cb-icd-src\n"
        "BUILDDIR=/tmp/cb-icd-build\n"
        "rm -rf \"$SRCDIR\" \"$BUILDDIR\"\n"
        "mkdir -p \"$SRCDIR\"\n"
        "(cd \"$SRCDIR\" && tar -xf -)\n"
        "MISSING=\"\"\n"
        "for tool in cmake gcc make glslangValidator x86_64-w64-mingw32-gcc; do\n"
        "  command -v \"$tool\" >/dev/null 2>&1 || MISSING=\"$MISSING $tool\"\n"
        "done\n"
        "if [ -n \"$MISSING\" ]; then\n"
        "  echo \"installing build deps inside HaSe VM (missing:$MISSING)...\" >&2\n"
        "  sudo apt-get update -qq >/dev/null\n"
        "  sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \\\n"
        "    cmake build-essential libvulkan-dev vulkan-tools \\\n"
        "    glslang-tools mingw-w64 >/dev/null\n"
        "fi\n"
        "cmake -S \"$SRCDIR\" -B \"$BUILDDIR\" \\\n"
        "  -DCHEESEBRIDGE_BUILD_GUEST=ON -DCHEESEBRIDGE_BUILD_DEMO=ON \\\n"
        "  -DHASE_BUILD_MANAGER=OFF >/tmp/cb-icd-cmake.log 2>&1 || {\n"
        "    echo 'cmake configure failed; see /tmp/cb-icd-cmake.log' >&2\n"
        "    tail -n 30 /tmp/cb-icd-cmake.log >&2\n"
        "    exit 1\n"
        "  }\n"
        "cmake --build \"$BUILDDIR\" -j\"$(nproc)\" >/tmp/cb-icd-build.log 2>&1 || {\n"
        "    echo 'build failed; see /tmp/cb-icd-build.log' >&2\n"
        "    tail -n 30 /tmp/cb-icd-build.log >&2\n"
        "    exit 1\n"
        "  }\n"
        "DST=/mnt/hase/vulkan/icd.d\n"
        "mkdir -p \"$DST\"\n"
        "install -m 0755 \"$BUILDDIR/guest/libCheeseBridge_icd.so\" \\\n"
        "  \"$DST/libCheeseBridge_icd.so\"\n"
        "cat > \"$DST/cheesebridge_icd.json\" <<JSON\n"
        "{\n"
        "    \"file_format_version\": \"1.0.1\",\n"
        "    \"ICD\": {\n"
        "        \"library_path\": \"$DST/libCheeseBridge_icd.so\",\n"
        "        \"api_version\": \"1.0.0\"\n"
        "    }\n"
        "}\n"
        "JSON\n"
        "if [ -x \"$BUILDDIR/demo/cheesebridge_clear_demo\" ]; then\n"
        "  install -m 0755 \"$BUILDDIR/demo/cheesebridge_clear_demo\" \"$DST/clear-demo\"\n"
        "fi\n"
        "if [ -x \"$BUILDDIR/demo/cheesebridge_triangle_demo\" ]; then\n"
        "  install -m 0755 \"$BUILDDIR/demo/cheesebridge_triangle_demo\" \"$DST/triangle-demo\"\n"
        "fi\n"
        "if command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1 && \\\n"
        "   [ -f \"$SRCDIR/demo/d3d11_smoke.c\" ]; then\n"
        "  x86_64-w64-mingw32-gcc \"$SRCDIR/demo/d3d11_smoke.c\" \\\n"
        "    -O2 -Wall -Wextra -o \"$DST/d3d11-smoke.exe\" \\\n"
        "    -ld3d11 -ldxgi -lgdi32 -luser32 -lole32 -luuid \\\n"
        "    >/tmp/cb-d3d11-smoke-build.log 2>&1 || {\n"
        "      echo 'd3d11 smoke build failed; see /tmp/cb-d3d11-smoke-build.log' >&2\n"
        "      tail -n 30 /tmp/cb-d3d11-smoke-build.log >&2\n"
        "      exit 1\n"
        "    }\n"
        "fi\n"
        "echo \"installed: $DST/libCheeseBridge_icd.so\"\n"
        "echo \"manifest:  $DST/cheesebridge_icd.json\"\n",
        0755);

    path_join(path, sizeof path, runtime, "run-clear-demo.sh");
    write_file(path,
        "#!/bin/sh\n"
        "# Run the Phase 3 clear-color demo against the macOS CheeseBridge\n"
        "# host. Requires hasectl install-icd to have run already.\n"
        "set -eu\n"
        "DST=/mnt/hase/vulkan/icd.d\n"
        "if [ ! -x \"$DST/clear-demo\" ]; then\n"
        "  echo 'clear-demo binary missing - run hasectl install-icd <bottle> first' >&2\n"
        "  exit 1\n"
        "fi\n"
        "export CHEESEBRIDGE_ICD_PATH=\"$DST/libCheeseBridge_icd.so\"\n"
        "export CHEESEBRIDGE_STUB=0\n"
        "export CHEESEBRIDGE_HOST=\"${CHEESEBRIDGE_HOST:-tcp:host.lima.internal:43210}\"\n"
        "exec \"$DST/clear-demo\"\n",
        0755);

    path_join(path, sizeof path, runtime, "run-triangle-demo.sh");
    write_file(path,
        "#!/bin/sh\n"
        "# Run the Phase 4 triangle demo against the macOS CheeseBridge host.\n"
        "# Requires hasectl install-icd to have run already.\n"
        "set -eu\n"
        "DST=/mnt/hase/vulkan/icd.d\n"
        "if [ ! -x \"$DST/triangle-demo\" ]; then\n"
        "  echo 'triangle-demo binary missing - install glslang-tools and re-run' >&2\n"
        "  echo '  hasectl install-icd <bottle>' >&2\n"
        "  exit 1\n"
        "fi\n"
        "export CHEESEBRIDGE_ICD_PATH=\"$DST/libCheeseBridge_icd.so\"\n"
        "export CHEESEBRIDGE_STUB=0\n"
        "export CHEESEBRIDGE_HOST=\"${CHEESEBRIDGE_HOST:-tcp:host.lima.internal:43210}\"\n"
        "exec \"$DST/triangle-demo\"\n",
        0755);

    path_join(path, sizeof path, runtime, "install-fex.sh");
    write_file(path,
        "#!/bin/sh\n"
        "# Install and configure FEX for the ARM64 HaSe VM. This replaces the\n"
        "# temporary x86_64/Rosetta Lima path with the intended Linux guest stack.\n"
        "set -eu\n"
        "ROOT=/mnt/hase/fex\n"
        "ENV=\"$ROOT/env.sh\"\n"
        "LOGDIR=/tmp/hase-fex-install\n"
        "mkdir -p \"$LOGDIR\"\n"
        "banner() {\n"
        "  printf '\\n'\n"
        "  printf '========================================\\n'\n"
        "  printf ' HaSe FEX bootstrap\\n'\n"
        "  printf ' ARM64 Linux guest -> x86_64 runtime\\n'\n"
        "  printf '========================================\\n'\n"
        "}\n"
        "note() {\n"
        "  printf '>> %s\\n' \"$1\"\n"
        "}\n"
        "run_step() {\n"
        "  label=\"$1\"\n"
        "  log=\"$2\"\n"
        "  shift 2\n"
        "  status=\"$LOGDIR/step-$$.status\"\n"
        "  rm -f \"$status\"\n"
        "  printf '  %-54s ' \"$label\"\n"
        "  ( set +e; \"$@\" >\"$log\" 2>&1; rc=$?; printf '%s\\n' \"$rc\" >\"$status\" ) &\n"
        "  pid=$!\n"
        "  tick=0\n"
        "  while [ ! -s \"$status\" ]; do\n"
        "    tick=$((tick + 1))\n"
        "    case $((tick % 4)) in\n"
        "      0) mark='|' ;;\n"
        "      1) mark='/' ;;\n"
        "      2) mark='-' ;;\n"
        "      *) mark='+' ;;\n"
        "    esac\n"
        "    printf '\\r  %-54s [%s]' \"$label\" \"$mark\"\n"
        "    sleep 0.2\n"
        "  done\n"
        "  wait \"$pid\" 2>/dev/null || true\n"
        "  rc=\"$(cat \"$status\")\"\n"
        "  rm -f \"$status\"\n"
        "  if [ \"$rc\" = 0 ]; then\n"
        "    printf '\\r  %-54s [ok]\\n' \"$label\"\n"
        "    return 0\n"
        "  fi\n"
        "  printf '\\r  %-54s [failed]\\n' \"$label\" >&2\n"
        "  printf '  log: %s\\n' \"$log\" >&2\n"
        "  tail -n 40 \"$log\" >&2 || true\n"
        "  exit \"$rc\"\n"
        "}\n"
        "fetch_rootfs() {\n"
        "  FEXRootFSFetcher -y -a --distro-name=ubuntu --distro-version=24.04 || FEXRootFSFetcher -y -a\n"
        "}\n"
        "banner\n"
        "case \"$(uname -m)\" in\n"
        "  aarch64|arm64) ;;\n"
        "  *)\n"
        "    echo 'HaSe now expects an ARM64 Linux VM with FEX, not an x86_64/Rosetta VM.' >&2\n"
        "    echo 'Create a fresh bottle with hasectl init/start so Lima uses arch: aarch64.' >&2\n"
        "    exit 2\n"
        "    ;;\n"
        "esac\n"
        "mkdir -p \"$ROOT\" \"$ROOT/RootFS\"\n"
        "export FEX_APP_DATA_LOCATION=\"$ROOT\"\n"
        "rootfs_present() {\n"
        "  find \"$ROOT/RootFS\" -maxdepth 1 \\( -name 'Ubuntu_*.ero' -o -name 'Ubuntu_*.sqsh' \\) -print -quit 2>/dev/null | grep -q .\n"
        "}\n"
        "write_env() {\n"
        "  ROOTFS_FILE=\"$(find \"$ROOT/RootFS\" -maxdepth 1 \\( -name 'Ubuntu_*.ero' -o -name 'Ubuntu_*.sqsh' \\) -print -quit 2>/dev/null || true)\"\n"
        "  {\n"
        "    printf 'export FEX_APP_DATA_LOCATION=%s\\n' \"$ROOT\"\n"
        "    if [ -n \"$ROOTFS_FILE\" ]; then\n"
        "      base=\"$(basename \"$ROOTFS_FILE\")\"\n"
        "      printf 'export FEX_ROOTFS=%s\\n' \"${base%.*}\"\n"
        "    fi\n"
        "  } > \"$ENV\"\n"
        "}\n"
        "if ! command -v FEXBash >/dev/null 2>&1 || ! command -v FEXRootFSFetcher >/dev/null 2>&1; then\n"
        "  note 'FEX binaries missing; setting up packages inside the HaSe VM.'\n"
        "  run_step 'Refreshing Ubuntu package lists' \"$LOGDIR/apt-update.log\" \\\n"
        "    sudo apt-get update -qq\n"
        "  run_step 'Installing bootstrap tools' \"$LOGDIR/bootstrap-tools.log\" \\\n"
        "    sudo env DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \\\n"
        "    ca-certificates curl software-properties-common fuse3 \\\n"
        "    squashfs-tools squashfuse\n"
        "  if ! apt-cache policy | grep -q '/fex-emu/fex/ubuntu'; then\n"
        "    run_step 'Adding FEX-Emu Ubuntu PPA' \"$LOGDIR/fex-ppa.log\" \\\n"
        "      sudo add-apt-repository -y ppa:fex-emu/fex\n"
        "    run_step 'Refreshing FEX package index' \"$LOGDIR/fex-apt-update.log\" \\\n"
        "      sudo apt-get update -qq\n"
        "  else\n"
        "    note 'FEX PPA already configured.'\n"
        "  fi\n"
        "  FEX_PACKAGE=fex-emu-armv8.0\n"
        "  if ! apt-cache show \"$FEX_PACKAGE\" >/dev/null 2>&1; then\n"
        "    FEX_PACKAGE=fex-emu\n"
        "  fi\n"
        "  FEX_PACKAGES=\"$FEX_PACKAGE\"\n"
        "  for pkg in fex-emu-binfmt32 fex-emu-binfmt64; do\n"
        "    if apt-cache show \"$pkg\" >/dev/null 2>&1; then\n"
        "      FEX_PACKAGES=\"$FEX_PACKAGES $pkg\"\n"
        "    fi\n"
        "  done\n"
        "  note \"Selected FEX packages: $FEX_PACKAGES\"\n"
        "  run_step 'Installing FEX emulator packages' \"$LOGDIR/fex-packages.log\" \\\n"
        "    sudo env DEBIAN_FRONTEND=noninteractive apt-get install -y -qq $FEX_PACKAGES\n"
        "else\n"
        "  note 'FEX binaries already installed.'\n"
        "fi\n"
        "if ! command -v FEXBash >/dev/null 2>&1 || ! command -v FEXRootFSFetcher >/dev/null 2>&1; then\n"
        "  echo 'FEX install completed, but FEXBash/FEXRootFSFetcher is still missing.' >&2\n"
        "  exit 1\n"
        "fi\n"
        "if ! rootfs_present; then\n"
        "  note 'No FEX rootfs found; downloading Ubuntu 24.04 x86_64 rootfs.'\n"
        "  run_step 'Fetching FEX x86_64 rootfs' \"$LOGDIR/fex-rootfs.log\" fetch_rootfs\n"
        "else\n"
        "  note 'FEX rootfs already present.'\n"
        "fi\n"
        "run_step 'Writing HaSe FEX environment' \"$LOGDIR/fex-env.log\" write_env\n"
        ". \"$ENV\"\n"
        "run_step 'Testing FEXBash x86_64 shell' \"$LOGDIR/fex-test.log\" FEXBash -c 'uname -m'\n"
        "note \"FEX ready at $ROOT\"\n"
        "printf '>> Logs are in %s if anything needs inspection.\\n' \"$LOGDIR\"\n",
        0755);

    path_join(path, sizeof path, runtime, "prepare-dxvk-smoke.sh");
    write_file(path,
        "#!/bin/sh\n"
        "# Prepare the Phase 5 Wine + DXVK smoke prefix. The test executable is\n"
        "# d3d11-smoke.exe, built by hasectl install-icd from demo/d3d11_smoke.c.\n"
        "set -eu\n"
        "/mnt/hase/runtime/start-session.sh >/dev/null\n"
        "/mnt/hase/runtime/install-fex.sh\n"
        ". /mnt/hase/fex/env.sh\n"
        "DST=/mnt/hase/vulkan/icd.d\n"
        "PREFIX=\"${HASE_DXVK_SMOKE_PREFIX:-/mnt/hase/wine-prefixes/dxvk-smoke}\"\n"
        "DXVK_CACHE=\"/tmp/hase-dxvk\"\n"
        "EXE=\"$DST/d3d11-smoke.exe\"\n"
        "if [ ! -f \"$EXE\" ]; then\n"
        "  echo 'd3d11-smoke.exe missing - run hasectl install-icd <bottle> first' >&2\n"
        "  exit 1\n"
        "fi\n"
        "mkdir -p \"$PREFIX\" /mnt/hase/proton\n"
        "export DISPLAY=\"${HASE_DISPLAY:-:99}\"\n"
        "export WINEPREFIX=\"$PREFIX\"\n"
        "export WINEARCH=win64\n"
        "export VK_ICD_FILENAMES=\"${VK_ICD_FILENAMES:-$DST/cheesebridge_icd.json}\"\n"
        "export CHEESEBRIDGE_ICD_PATH=\"$DST/libCheeseBridge_icd.so\"\n"
        "export CHEESEBRIDGE_STUB=0\n"
        "export CHEESEBRIDGE_HOST=\"${CHEESEBRIDGE_HOST:-tcp:host.lima.internal:43210}\"\n"
        "run_x86() {\n"
        "  FEXBash -c \"$*\"\n"
        "}\n"
        "if ! run_x86 'command -v wine64 >/dev/null 2>&1 || command -v wine >/dev/null 2>&1'; then\n"
        "  echo 'x86_64 Wine is not available inside the FEX environment.' >&2\n"
        "  echo 'FEX is installed; add an x86_64 Wine/Proton runtime next for this smoke test.' >&2\n"
        "  exit 2\n"
        "fi\n"
        "run_x86 'wineboot -u'\n"
        "install_dxvk_dir() {\n"
        "  DXVK_DIR=\"$1\"\n"
        "  if [ ! -f \"$DXVK_DIR/x64/d3d11.dll\" ] || [ ! -f \"$DXVK_DIR/x64/dxgi.dll\" ]; then\n"
        "    return 1\n"
        "  fi\n"
        "  mkdir -p \"$PREFIX/drive_c/windows/system32\"\n"
        "  cp \"$DXVK_DIR/x64/d3d11.dll\" \"$DXVK_DIR/x64/dxgi.dll\" \\\n"
        "    \"$PREFIX/drive_c/windows/system32/\"\n"
        "  run_x86 'wine reg add \"HKCU\\\\Software\\\\Wine\\\\DllOverrides\" /v d3d11 /d native,builtin /f >/dev/null'\n"
        "  run_x86 'wine reg add \"HKCU\\\\Software\\\\Wine\\\\DllOverrides\" /v dxgi /d native,builtin /f >/dev/null'\n"
        "  return 0\n"
        "}\n"
        "if [ -n \"${HASE_DXVK_DIR:-}\" ] && install_dxvk_dir \"$HASE_DXVK_DIR\"; then\n"
        "  echo \"DXVK installed from $HASE_DXVK_DIR\"\n"
        "else\n"
        "  TARBALL=\"$(find /mnt/hase/proton -maxdepth 1 -name 'dxvk-*.tar.*' -print 2>/dev/null | head -n 1)\"\n"
        "  if [ -n \"$TARBALL\" ]; then\n"
        "    rm -rf \"$DXVK_CACHE\"\n"
        "    mkdir -p \"$DXVK_CACHE\"\n"
        "    tar -xf \"$TARBALL\" -C \"$DXVK_CACHE\"\n"
        "    DXVK_DIR=\"$(find \"$DXVK_CACHE\" -maxdepth 1 -type d -name 'dxvk-*' -print | head -n 1)\"\n"
        "    install_dxvk_dir \"$DXVK_DIR\"\n"
        "    echo \"DXVK installed from $TARBALL\"\n"
        "  elif run_x86 'command -v winetricks >/dev/null 2>&1'; then\n"
        "    echo 'installing DXVK with winetricks...' >&2\n"
        "    run_x86 'winetricks -q dxvk'\n"
        "  else\n"
        "    echo 'DXVK is not installed.' >&2\n"
        "    echo 'Place dxvk-*.tar.gz in /mnt/hase/proton, set HASE_DXVK_DIR, or install winetricks.' >&2\n"
        "    exit 3\n"
        "  fi\n"
        "fi\n"
        "echo \"Phase 5 Wine/DXVK smoke prefix ready: $PREFIX\"\n",
        0755);

    path_join(path, sizeof path, runtime, "run-dxvk-smoke.sh");
    write_file(path,
        "#!/bin/sh\n"
        "# Run the Phase 5 D3D11 -> Wine -> DXVK -> CheeseBridge smoke test.\n"
        "set -eu\n"
        "/mnt/hase/runtime/start-session.sh >/dev/null\n"
        "/mnt/hase/runtime/install-fex.sh\n"
        ". /mnt/hase/fex/env.sh\n"
        "DST=/mnt/hase/vulkan/icd.d\n"
        "PREFIX=\"${HASE_DXVK_SMOKE_PREFIX:-/mnt/hase/wine-prefixes/dxvk-smoke}\"\n"
        "EXE=\"${HASE_DXVK_SMOKE_EXE:-$DST/d3d11-smoke.exe}\"\n"
        "if [ ! -f \"$EXE\" ]; then\n"
        "  echo 'd3d11-smoke.exe missing - run hasectl install-icd <bottle> first' >&2\n"
        "  exit 1\n"
        "fi\n"
        "export DISPLAY=\"${HASE_DISPLAY:-:99}\"\n"
        "export WINEPREFIX=\"$PREFIX\"\n"
        "export WINEARCH=win64\n"
        "export WINEDEBUG=\"${WINEDEBUG:--all}\"\n"
        "export VK_ICD_FILENAMES=\"${VK_ICD_FILENAMES:-$DST/cheesebridge_icd.json}\"\n"
        "export CHEESEBRIDGE_ICD_PATH=\"$DST/libCheeseBridge_icd.so\"\n"
        "export CHEESEBRIDGE_STUB=0\n"
        "export CHEESEBRIDGE_HOST=\"${CHEESEBRIDGE_HOST:-tcp:host.lima.internal:43210}\"\n"
        "export HASE_DXVK_SMOKE_EXE=\"$EXE\"\n"
        "exec FEXBash -c 'if command -v wine64 >/dev/null 2>&1; then wine64 \"$HASE_DXVK_SMOKE_EXE\"; elif command -v wine >/dev/null 2>&1; then wine \"$HASE_DXVK_SMOKE_EXE\"; else echo \"x86_64 Wine is not available inside the FEX environment; run hasectl prepare-dxvk-smoke <bottle> after adding Wine/Proton\" >&2; exit 2; fi'\n"
        "exit 2\n",
        0755);

    path_join(path, sizeof path, runtime, "launch-test-window.sh");
    write_file(path,
        "#!/bin/sh\n"
        "set -eu\n"
        "/mnt/hase/runtime/start-session.sh >/dev/null\n"
        "export DISPLAY=\"${HASE_DISPLAY:-:99}\"\n"
        "if ! command -v xmessage >/dev/null 2>&1; then\n"
        "  echo 'xmessage is not installed; install x11-utils in the HaSe VM' >&2\n"
        "  exit 2\n"
        "fi\n"
        "nohup xmessage -name HaSeTestWindow -title 'HaSe Test Window' -center \\\n"
        "  'HaSe guest window captured by macOS host' >/tmp/hase/test-window.log 2>&1 &\n"
        "printf 'HaSe test window launched in hidden X11 session\\n'\n",
        0755);

    path_join(path, sizeof path, runtime, "launch-steam.sh");
    write_file(path,
        "#!/bin/sh\n"
        "set -eu\n"
        "/mnt/hase/runtime/start-session.sh\n"
        "export DISPLAY=\"${HASE_DISPLAY:-:99}\"\n"
        "export CHEESEBRIDGE_HOST=\"${CHEESEBRIDGE_HOST:-tcp:host.lima.internal:43210}\"\n"
        "export VK_ICD_FILENAMES=\"${VK_ICD_FILENAMES:-/mnt/hase/vulkan/icd.d/cheesebridge_icd.json}\"\n"
        "if command -v steam >/dev/null 2>&1; then\n"
        "  nohup steam \"$@\" >/tmp/hase/steam.log 2>&1 &\n"
        "  echo 'Steam launched in hidden HaSe session'\n"
        "else\n"
        "  echo 'Steam is not installed in this VM yet.' >&2\n"
        "  echo 'Install Steam/Proton provisioning in a later HaSe phase.' >&2\n"
        "  exit 2\n"
        "fi\n",
        0755);
}

static void write_metadata(const hase_config_t *cfg) {
    char path[PATH_MAX];
    char *bottle = yaml_quote(cfg->bottle);
    char *lima = yaml_quote(cfg->lima_yaml);

    size_t cap = 4096, pos = 0;
    char *json = (char *)calloc(1, cap);
    if (!json) die("out of memory");
    appendf(&json, &cap, &pos,
        "{\n"
        "  \"version\": 1,\n"
        "  \"name\": \"%s\",\n"
        "  \"kind\": \"hase_linux_bottle\",\n"
        "  \"backend\": \"lima\",\n"
        "  \"vm_name\": \"%s\",\n"
        "  \"arch\": \"aarch64\",\n"
        "  \"bottle_path\": %s,\n"
        "  \"lima_config\": %s,\n"
        "  \"display_model\": \"cropped_framebuffer_first\",\n"
        "  \"graphics_bridge\": \"cheesebridge\",\n"
        "  \"default_display\": \":99\"\n"
        "}\n",
        cfg->name, cfg->vm_name, bottle, lima);

    char metadata[PATH_MAX];
    path_join(metadata, sizeof metadata, cfg->bottle, "metadata");
    path_join(path, sizeof path, metadata, "bottle.json");
    write_file(path, json, 0644);
    path_join(path, sizeof path, metadata, "windows.json");
    write_file(path, "[]\n", 0644);
    path_join(path, sizeof path, metadata, "launch.json");
    write_file(path,
        "{\n"
        "  \"launch_sequence\": [\n"
        "    \"start_lima_vm\",\n"
        "    \"start_cheesebridge_host\",\n"
        "    \"start_hidden_x11_session\",\n"
        "    \"launch_steam_or_app\",\n"
        "    \"track_linux_windows\"\n"
        "  ]\n"
        "}\n",
        0644);
    free(json);
    free(bottle);
    free(lima);
}

static void create_layout(const hase_config_t *cfg) {
    static const char *dirs[] = {
        "home", "steam", "proton", "wine-prefixes", "wine-prefixes/default",
        "fex", "vulkan", "vulkan/icd.d", "shared", "metadata", "vm",
        "runtime", "windows"
    };
    mkdir_p(cfg->root);
    mkdir_p(cfg->bottle);
    for (size_t i = 0; i < sizeof dirs / sizeof dirs[0]; ++i) {
        char path[PATH_MAX];
        path_join(path, sizeof path, cfg->bottle, dirs[i]);
        mkdir_p(path);
    }
}

static int cmd_init(const hase_config_t *cfg) {
    create_layout(cfg);
    write_lima_yaml(cfg);
    write_runtime_scripts(cfg);
    write_metadata(cfg);
    printf("Created HaSe bottle: %s\n", cfg->bottle);
    printf("Lima VM name: %s\n", cfg->vm_name);
    printf("Next: hasectl start %s --root \"%s\"\n", cfg->name, cfg->root);
    return 0;
}

static int cmd_paths(const hase_config_t *cfg) {
    printf("name=%s\n", cfg->name);
    printf("vm_name=%s\n", cfg->vm_name);
    printf("arch=aarch64\n");
    printf("root=%s\n", cfg->root);
    printf("bottle=%s\n", cfg->bottle);
    printf("lima_config=%s\n", cfg->lima_yaml);
    return 0;
}

static int cmd_start(const hase_config_t *cfg) {
    ensure_bottle_exists(cfg);
    write_runtime_scripts(cfg);
    write_lima_yaml(cfg);
    int rc = 0;
    if (lima_instance_exists(cfg)) {
        char *start_existing_argv[] = {
            "limactl", "--tty=false", "start", (char *)cfg->vm_name, NULL
        };
        rc = run_wait(start_existing_argv);
    } else {
        char name_arg[192];
        snprintf(name_arg, sizeof name_arg, "--name=%s", cfg->vm_name);
        char *start_new_argv[] = {
            "limactl", "--tty=false", "start", "--containerd=none",
            name_arg, (char *)cfg->lima_yaml, NULL
        };
        rc = run_wait(start_new_argv);
    }
    if (rc != 0) return rc;

    char *session_argv[] = {
        "limactl", "shell", "--workdir=/mnt/hase", (char *)cfg->vm_name,
        "sh", "-lc", "/mnt/hase/runtime/start-session.sh",
        NULL
    };
    return run_wait(session_argv);
}

static int cmd_stop(const hase_config_t *cfg) {
    ensure_bottle_exists(cfg);
    char *session_argv[] = {
        "limactl", "shell", "--workdir=/", (char *)cfg->vm_name,
        "sh", "-lc", "/mnt/hase/runtime/stop-session.sh",
        NULL
    };
    (void)run_wait(session_argv);

    char *stop_argv[] = { "limactl", "stop", (char *)cfg->vm_name, NULL };
    return run_wait(stop_argv);
}

static int cmd_status(const hase_config_t *cfg) {
    char *argv[] = { "limactl", "list", (char *)cfg->vm_name, NULL };
    return run_wait(argv);
}

static int cmd_shell(const hase_config_t *cfg) {
    ensure_bottle_exists(cfg);
    char *argv[] = {
        "limactl", "shell", "--workdir=/mnt/hase", (char *)cfg->vm_name, NULL
    };
    execvp(argv[0], argv);
    fprintf(stderr, "exec limactl: %s\n", strerror(errno));
    return 127;
}

static int cmd_steam(const hase_config_t *cfg) {
    ensure_bottle_exists(cfg);
    write_runtime_scripts(cfg);
    char *argv[] = {
        "limactl", "shell", "--workdir=/mnt/hase", (char *)cfg->vm_name,
        "sh", "-lc", "/mnt/hase/runtime/launch-steam.sh",
        NULL
    };
    return run_wait(argv);
}

static int cmd_demo_window(const hase_config_t *cfg) {
    ensure_bottle_exists(cfg);
    write_runtime_scripts(cfg);
    char *argv[] = {
        "limactl", "shell", "--workdir=/mnt/hase", (char *)cfg->vm_name,
        "sh", "-lc", "/mnt/hase/runtime/launch-test-window.sh",
        NULL
    };
    return run_wait(argv);
}

/* Locate the CheeseBridge source tree.
 *   1. CHEESEBRIDGE_SRC env var if set
 *   2. dirname(realpath(argv0))/../..  (build/hase/hasectl -> repo root)
 *   3. fail
 * Returns 0 on success and writes the resolved path into out. */
static int resolve_src_tree(const char *argv0, char *out, size_t out_size) {
    const char *env = getenv("CHEESEBRIDGE_SRC");
    if (env && *env) {
        char tmp[PATH_MAX];
        snprintf(tmp, sizeof tmp, "%s/CMakeLists.txt", env);
        if (access(tmp, R_OK) == 0) {
            snprintf(out, out_size, "%s", env);
            return 0;
        }
    }
    char real[PATH_MAX];
    if (realpath(argv0, real)) {
        char *slash = strrchr(real, '/');
        if (slash) *slash = '\0';                       /* strip /hasectl     */
        slash = strrchr(real, '/'); if (slash) *slash = '\0';  /* strip /hase */
        slash = strrchr(real, '/'); if (slash) *slash = '\0';  /* strip /build*/
        char tmp[PATH_MAX];
        snprintf(tmp, sizeof tmp, "%s/CMakeLists.txt", real);
        if (access(tmp, R_OK) == 0) {
            snprintf(out, out_size, "%s", real);
            return 0;
        }
    }
    return -1;
}

/* Pipe `tar -cf - -C <src> <files...>` into `limactl shell ... <script>`.
 * Returns the exit status of the right-hand side (the install script). */
static int run_install_pipeline(const hase_config_t *cfg, const char *src) {
    int pipefd[2];
    if (pipe(pipefd) < 0) { perror("pipe"); return 127; }

    pid_t tar_pid = fork();
    if (tar_pid < 0) { perror("fork"); close(pipefd[0]); close(pipefd[1]); return 127; }
    if (tar_pid == 0) {
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[0]); close(pipefd[1]);
        execlp("tar", "tar", "-cf", "-", "-C", src,
               "CMakeLists.txt", "cmake", "guest", "host", "protocol", "hase",
               "demo", (char *)NULL);
        fprintf(stderr, "exec tar: %s\n", strerror(errno));
        _exit(127);
    }

    pid_t lima_pid = fork();
    if (lima_pid < 0) { perror("fork"); close(pipefd[0]); close(pipefd[1]); return 127; }
    if (lima_pid == 0) {
        dup2(pipefd[0], STDIN_FILENO);
        close(pipefd[0]); close(pipefd[1]);
        execlp("limactl", "limactl", "--tty=false", "shell",
               "--workdir=/mnt/hase", (char *)cfg->vm_name,
               "sh", "-lc", "/mnt/hase/runtime/install-icd.sh",
               (char *)NULL);
        fprintf(stderr, "exec limactl: %s\n", strerror(errno));
        _exit(127);
    }

    close(pipefd[0]); close(pipefd[1]);

    int tar_status = 0, lima_status = 0;
    waitpid(tar_pid, &tar_status, 0);
    waitpid(lima_pid, &lima_status, 0);
    if (WIFEXITED(tar_status) && WEXITSTATUS(tar_status) != 0) {
        fprintf(stderr, "tar exited with %d\n", WEXITSTATUS(tar_status));
    }
    if (WIFEXITED(lima_status)) return WEXITSTATUS(lima_status);
    return 127;
}

static int cmd_install_icd(const hase_config_t *cfg, const char *argv0) {
    ensure_bottle_exists(cfg);
    write_runtime_scripts(cfg);

    char src[PATH_MAX];
    if (resolve_src_tree(argv0, src, sizeof src) != 0) {
        die("could not locate CheeseBridge source tree.\n"
            "Set CHEESEBRIDGE_SRC=<path-to-repo-root> and retry.");
    }

    fprintf(stderr, "hasectl: installing ICD into bottle '%s' from %s\n",
            cfg->name, src);

    if (!lima_instance_exists(cfg)) {
        fprintf(stderr, "hasectl: VM not running; start it first with: "
                        "hasectl start %s\n", cfg->name);
        return 1;
    }

    return run_install_pipeline(cfg, src);
}

static int cmd_install_fex(const hase_config_t *cfg) {
    ensure_bottle_exists(cfg);
    write_runtime_scripts(cfg);

    if (!lima_instance_exists(cfg)) {
        fprintf(stderr, "hasectl: VM not running; start it first with: "
                        "hasectl start %s\n", cfg->name);
        return 1;
    }

    char *argv[] = {
        "limactl", "--tty=false", "shell", "--workdir=/mnt/hase", (char *)cfg->vm_name,
        "sh", "-lc", "/mnt/hase/runtime/install-fex.sh",
        NULL
    };
    return run_wait(argv);
}

static int cmd_run_clear_demo(const hase_config_t *cfg) {
    ensure_bottle_exists(cfg);
    write_runtime_scripts(cfg);
    char *argv[] = {
        "limactl", "--tty=false", "shell", "--workdir=/mnt/hase", (char *)cfg->vm_name,
        "sh", "-lc", "/mnt/hase/runtime/run-clear-demo.sh",
        NULL
    };
    return run_wait(argv);
}

static int cmd_run_triangle_demo(const hase_config_t *cfg) {
    ensure_bottle_exists(cfg);
    write_runtime_scripts(cfg);
    char *argv[] = {
        "limactl", "--tty=false", "shell", "--workdir=/mnt/hase", (char *)cfg->vm_name,
        "sh", "-lc", "/mnt/hase/runtime/run-triangle-demo.sh",
        NULL
    };
    return run_wait(argv);
}

static int cmd_prepare_dxvk_smoke(const hase_config_t *cfg) {
    ensure_bottle_exists(cfg);
    write_runtime_scripts(cfg);
    char *argv[] = {
        "limactl", "--tty=false", "shell", "--workdir=/mnt/hase", (char *)cfg->vm_name,
        "sh", "-lc", "/mnt/hase/runtime/prepare-dxvk-smoke.sh",
        NULL
    };
    return run_wait(argv);
}

static int cmd_run_dxvk_smoke(const hase_config_t *cfg) {
    ensure_bottle_exists(cfg);
    write_runtime_scripts(cfg);
    char *argv[] = {
        "limactl", "--tty=false", "shell", "--workdir=/mnt/hase", (char *)cfg->vm_name,
        "sh", "-lc", "/mnt/hase/runtime/run-dxvk-smoke.sh",
        NULL
    };
    return run_wait(argv);
}

static int cmd_windows(const hase_config_t *cfg) {
    ensure_bottle_exists(cfg);
    write_runtime_scripts(cfg);
    char *argv[] = {
        "limactl", "shell", "--workdir=/mnt/hase", (char *)cfg->vm_name,
        "sh", "-lc",
        "/mnt/hase/runtime/start-session.sh >/dev/null && /mnt/hase/runtime/window-snapshot.sh",
        NULL
    };
    return run_wait(argv);
}

int main(int argc, char **argv) {
    if (argc < 2 || !strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) {
        usage(argc < 2 ? stderr : stdout);
        return argc < 2 ? 2 : 0;
    }

    hase_config_t cfg;
    parse_common(argc, argv, &cfg);

    if (!strcmp(argv[1], "init")) return cmd_init(&cfg);
    if (!strcmp(argv[1], "start")) return cmd_start(&cfg);
    if (!strcmp(argv[1], "stop")) return cmd_stop(&cfg);
    if (!strcmp(argv[1], "status")) return cmd_status(&cfg);
    if (!strcmp(argv[1], "shell")) return cmd_shell(&cfg);
    if (!strcmp(argv[1], "steam")) return cmd_steam(&cfg);
    if (!strcmp(argv[1], "demo-window")) return cmd_demo_window(&cfg);
    if (!strcmp(argv[1], "windows")) return cmd_windows(&cfg);
    if (!strcmp(argv[1], "install-icd")) return cmd_install_icd(&cfg, argv[0]);
    if (!strcmp(argv[1], "install-fex")) return cmd_install_fex(&cfg);
    if (!strcmp(argv[1], "prepare-dxvk-smoke")) return cmd_prepare_dxvk_smoke(&cfg);
    if (!strcmp(argv[1], "run-clear-demo")) return cmd_run_clear_demo(&cfg);
    if (!strcmp(argv[1], "run-triangle-demo")) return cmd_run_triangle_demo(&cfg);
    if (!strcmp(argv[1], "run-dxvk-smoke")) return cmd_run_dxvk_smoke(&cfg);
    if (!strcmp(argv[1], "paths")) return cmd_paths(&cfg);

    usage(stderr);
    die("unknown command: %s", argv[1]);
}
