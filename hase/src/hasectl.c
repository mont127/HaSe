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
        "  hasectl refresh-runtime <bottle> [--root DIR]\n"
        "  hasectl install-icd <bottle> [--root DIR]\n"
        "  hasectl install-fex <bottle> [--root DIR]\n"
        "  hasectl install-steam <bottle> [--root DIR]\n"
        "  hasectl run-clear-demo <bottle> [--root DIR]\n"
        "  hasectl run-triangle-demo <bottle> [--root DIR]\n"
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

static char *read_text_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) die("open(%s): %s", path, strerror(errno));
    if (fseek(f, 0, SEEK_END) != 0) die("seek(%s): %s", path, strerror(errno));
    long len = ftell(f);
    if (len < 0) die("tell(%s): %s", path, strerror(errno));
    if (fseek(f, 0, SEEK_SET) != 0) die("seek(%s): %s", path, strerror(errno));
    char *data = (char *)calloc((size_t)len + 1, 1);
    if (!data) die("out of memory");
    if (len > 0 && fread(data, 1, (size_t)len, f) != (size_t)len) {
        fclose(f);
        free(data);
        die("read(%s): %s", path, strerror(errno));
    }
    if (fclose(f) != 0) die("close(%s): %s", path, strerror(errno));
    data[len] = '\0';
    return data;
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

static bool lima_instance_config_path(const hase_config_t *cfg,
                                      char *out,
                                      size_t out_size) {
    const char *home = getenv("HOME");
    if (!home || !*home) return false;
    char lima_root[PATH_MAX], instance_dir[PATH_MAX];
    path_join(lima_root, sizeof lima_root, home, ".lima");
    path_join(instance_dir, sizeof instance_dir, lima_root, cfg->vm_name);
    if (!exists_dir(instance_dir)) return false;
    path_join(out, out_size, instance_dir, "lima.yaml");
    return true;
}

static void sync_existing_lima_yaml(const hase_config_t *cfg, bool verbose) {
    char live_yaml[PATH_MAX];
    if (!lima_instance_config_path(cfg, live_yaml, sizeof live_yaml)) return;
    char *data = read_text_file(cfg->lima_yaml);
    write_file(live_yaml, data, 0644);
    free(data);
    if (verbose) printf("Updated Lima instance config: %s\n", live_yaml);
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
        "cpus: 6\n"
        "memory: \"8GiB\"\n"
        "disk: \"48GiB\"\n"
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
        "GEOMETRY=\"${HASE_GEOMETRY:-960x540x24}\"\n"
        "mkdir -p /tmp/hase\n"
        "if [ -f /tmp/hase/geometry ] && [ \"$(cat /tmp/hase/geometry)\" != \"$GEOMETRY\" ]; then\n"
        "  pkill -f \"Xvfb ${DISPLAY}\" >/dev/null 2>&1 || true\n"
        "fi\n"
        "if [ ! -f /tmp/hase/geometry ] && pgrep -f \"Xvfb ${DISPLAY}\" >/dev/null; then\n"
        "  pkill -f \"Xvfb ${DISPLAY}\" >/dev/null 2>&1 || true\n"
        "fi\n"
        "if ! pgrep -f \"Xvfb ${DISPLAY}\" >/dev/null; then\n"
        "  Xvfb \"${DISPLAY}\" -screen 0 \"$GEOMETRY\" -nolisten tcp >/tmp/hase/xvfb.log 2>&1 &\n"
        "  printf '%s\\n' \"$GEOMETRY\" >/tmp/hase/geometry\n"
        "  sleep 1\n"
        "fi\n"
        "if command -v xsetroot >/dev/null 2>&1; then\n"
        "  xsetroot -display \"${DISPLAY}\" -solid black || true\n"
        "fi\n"
        "if ! pgrep -f \"matchbox-window-manager\" >/dev/null; then\n"
        "  pkill -f \"matchbox-window-manager\" >/dev/null 2>&1 || true\n"
        "  DISPLAY=\"${DISPLAY}\" matchbox-window-manager -use_titlebar no >/tmp/hase/wm.log 2>&1 &\n"
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
        "pkill -f \"matchbox-window-manager\" >/dev/null 2>&1 || true\n"
        "pkill -f \"steamwebhelper\" >/dev/null 2>&1 || true\n"
        "pkill -f \"pressure-vessel\" >/dev/null 2>&1 || true\n"
        "pkill -f \"steam-runtime\" >/dev/null 2>&1 || true\n"
        "pkill -f \"steam-env/usr/bin\" >/dev/null 2>&1 || true\n"
        "pkill -f \"Steam/ubuntu12\" >/dev/null 2>&1 || true\n"
        "pkill -f \"FEXBash.*steam\" >/dev/null 2>&1 || true\n"
        "pkill -x \"steam\" >/dev/null 2>&1 || true\n"
        "if [ -f /tmp/hase/dbus.env ]; then\n"
        "  . /tmp/hase/dbus.env >/dev/null 2>&1 || true\n"
        "  [ -n \"${DBUS_SESSION_BUS_PID:-}\" ] && kill \"$DBUS_SESSION_BUS_PID\" >/dev/null 2>&1 || true\n"
        "fi\n"
        "pkill -f \"Xvfb ${DISPLAY}\" >/dev/null 2>&1 || true\n"
        "pkill -f \"capture-daemon.sh\" >/dev/null 2>&1 || true\n"
        "pkill -f \"input-daemon.sh\" >/dev/null 2>&1 || true\n"
        "rm -rf /tmp/hase /mnt/hase/runtime/frame.bmp /mnt/hase/runtime/frame.tmp.bmp \\\n"
        "  /mnt/hase/runtime/frame.png /mnt/hase/runtime/frame.tmp.png \\\n"
        "  /mnt/hase/runtime/frame.xwd /mnt/hase/runtime/frame.tmp.xwd \\\n"
        "  /mnt/hase/runtime/input.queue /mnt/hase/runtime/input.processing\n"
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
        "WINDOW_ID=\"$1\"\n"
        "export DISPLAY=\"${HASE_DISPLAY:-:99}\"\n"
        "xwd -silent -id \"$WINDOW_ID\" | xwdtopnm 2>/dev/null | pnmtopng -force 2>/dev/null\n",
        0755);

    path_join(path, sizeof path, runtime, "capture-daemon.sh");
    write_file(path,
        "#!/bin/sh\n"
        "set -eu\n"
        "export DISPLAY=\"${HASE_DISPLAY:-:99}\"\n"
        "FORMAT=\"${HASE_CAPTURE_FORMAT:-xwd}\"\n"
        "DELAY=\"${HASE_CAPTURE_DELAY:-0.016}\"\n"
        "BASE=\"/mnt/hase/runtime/frame\"\n"
        "mkdir -p /mnt/hase/runtime\n"
        "rm -f \"$BASE.xwd\" \"$BASE.bmp\" \"$BASE.tmp.xwd\" \"$BASE.tmp.bmp\"\n"
        "while true; do\n"
        "  if [ \"$FORMAT\" = bmp ]; then\n"
        "    DISPLAY=\"$DISPLAY\" xwd -silent -root | xwdtopnm 2>/dev/null | ppmtobmp > \"$BASE.tmp.bmp\" 2>/dev/null || true\n"
        "    if [ -s \"$BASE.tmp.bmp\" ]; then mv \"$BASE.tmp.bmp\" \"$BASE.bmp\"; else rm -f \"$BASE.tmp.bmp\"; fi\n"
        "  else\n"
        "    DISPLAY=\"$DISPLAY\" xwd -silent -root -out \"$BASE.tmp.xwd\" >/dev/null 2>&1 || true\n"
        "    if [ -s \"$BASE.tmp.xwd\" ]; then mv \"$BASE.tmp.xwd\" \"$BASE.xwd\"; else rm -f \"$BASE.tmp.xwd\"; fi\n"
        "  fi\n"
        "  sleep \"$DELAY\"\n"
        "done\n",
        0755);

    path_join(path, sizeof path, runtime, "input-daemon.sh");
    write_file(path,
        "#!/bin/sh\n"
        "set -eu\n"
        "export DISPLAY=\"${HASE_DISPLAY:-:99}\"\n"
        "QUEUE=/mnt/hase/runtime/input.queue\n"
        "WORK=/mnt/hase/runtime/input.processing\n"
        "DELAY=\"${HASE_INPUT_DELAY:-0.004}\"\n"
        "mkdir -p /mnt/hase/runtime\n"
        "touch \"$QUEUE\"\n"
        "while true; do\n"
        "  if [ -s \"$QUEUE\" ]; then\n"
        "    cp \"$QUEUE\" \"$WORK\" 2>/dev/null || true\n"
        "    : > \"$QUEUE\"\n"
        "    if [ -s \"$WORK\" ]; then\n"
        "      while IFS= read -r cmd; do\n"
        "        [ -n \"$cmd\" ] || continue\n"
        "        DISPLAY=\"$DISPLAY\" sh -lc \"$cmd\" >/tmp/hase/input-daemon.log 2>&1 || true\n"
        "      done < \"$WORK\"\n"
        "    fi\n"
        "    rm -f \"$WORK\"\n"
        "  fi\n"
        "  sleep \"$DELAY\"\n"
        "done\n",
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
        "  return \"$rc\"\n"
        "}\n"
        "must_step() {\n"
        "  run_step \"$@\" || exit \"$?\"\n"
        "}\n"
        "retry_step() {\n"
        "  RETRY_LABEL=\"$1\"\n"
        "  RETRY_LOG=\"$2\"\n"
        "  RETRY_TRIES=\"$3\"\n"
        "  shift 3\n"
        "  RETRY_ATTEMPT=1\n"
        "  while [ \"$RETRY_ATTEMPT\" -le \"$RETRY_TRIES\" ]; do\n"
        "    if run_step \"$RETRY_LABEL (try $RETRY_ATTEMPT/$RETRY_TRIES)\" \"$RETRY_LOG\" \"$@\"; then\n"
        "      return 0\n"
        "    fi\n"
        "    if [ \"$RETRY_ATTEMPT\" -lt \"$RETRY_TRIES\" ]; then\n"
        "      RETRY_DELAY=$((RETRY_ATTEMPT * 5))\n"
        "      note \"Retrying $RETRY_LABEL in ${RETRY_DELAY}s.\"\n"
        "      sleep \"$RETRY_DELAY\"\n"
        "    fi\n"
        "    RETRY_ATTEMPT=$((RETRY_ATTEMPT + 1))\n"
        "  done\n"
        "  return 1\n"
        "}\n"
        "fetch_rootfs() {\n"
        "  FEXRootFSFetcher --force-ui=tty -y -a --distro-name=ubuntu --distro-version=24.04 || FEXRootFSFetcher --force-ui=tty -y -a\n"
        "  for d in \"$HOME/.fex-emu/RootFS\" \"$HOME/.local/share/fex-emu/RootFS\"; do\n"
        "    if [ -d \"$d\" ]; then\n"
        "      mv \"$d\"/* \"$ROOT/RootFS/\" 2>/dev/null || true\n"
        "    fi\n"
        "  done\n"
        "}\n"
        "fex_packages_visible() {\n"
        "  apt-cache search --names-only '^fex-emu-armv8\\.' | grep -q .\n"
        "}\n"
        "refresh_fex_index() {\n"
        "  sudo apt-get -o Acquire::Retries=3 update -qq\n"
        "  if fex_packages_visible; then\n"
        "    return 0\n"
        "  fi\n"
        "  echo 'FEX PPA package index is still not visible to apt.' >&2\n"
        "  echo 'This usually means ppa.launchpadcontent.net failed during apt update.' >&2\n"
        "  apt-cache policy | grep -A4 -B2 'fex-emu\\|launchpadcontent' >&2 || true\n"
        "  return 1\n"
        "}\n"
        "download_fex_debs_from_launchpad_api() {\n"
        "  DEB_DIR=\"$LOGDIR/debs\"\n"
        "  URLS=\"$LOGDIR/fex-deb-urls.txt\"\n"
        "  rm -rf \"$DEB_DIR\"\n"
        "  mkdir -p \"$DEB_DIR\"\n"
        "  python3 - \"$URLS\" <<'PY'\n"
        "import json\n"
        "import sys\n"
        "import urllib.parse\n"
        "import urllib.request\n"
        "\n"
        "out_path = sys.argv[1]\n"
        "base = 'https://api.launchpad.net/devel/~fex-emu/+archive/ubuntu/fex'\n"
        "arch_series = 'https://api.launchpad.net/devel/ubuntu/noble/arm64'\n"
        "wanted = ['fex-emu-armv8.0', 'fex-emu-binfmt32', 'fex-emu-binfmt64', 'fex-emu-wine']\n"
        "required = {'fex-emu-armv8.0', 'fex-emu-binfmt32', 'fex-emu-binfmt64'}\n"
        "urls = []\n"
        "missing = []\n"
        "\n"
        "for name in wanted:\n"
        "    query = urllib.parse.urlencode({\n"
        "        'ws.op': 'getPublishedBinaries',\n"
        "        'binary_name': name,\n"
        "        'status': 'Published',\n"
        "        'exact_match': 'true',\n"
        "        'order_by_date': 'true',\n"
        "        'distro_arch_series': arch_series,\n"
        "    })\n"
        "    with urllib.request.urlopen(base + '?' + query, timeout=30) as response:\n"
        "        listing = json.load(response)\n"
        "    entries = listing.get('entries') or []\n"
        "    if not entries:\n"
        "        if name in required:\n"
        "            missing.append(name)\n"
        "        continue\n"
        "    pub = entries[0]\n"
        "    with urllib.request.urlopen(pub['self_link'] + '?ws.op=binaryFileUrls', timeout=30) as response:\n"
        "        urls.extend(json.load(response))\n"
        "\n"
        "if missing:\n"
        "    raise SystemExit('Missing required Launchpad binaries: ' + ', '.join(missing))\n"
        "with open(out_path, 'w', encoding='utf-8') as out:\n"
        "    for url in urls:\n"
        "        out.write(url + '\\n')\n"
        "PY\n"
        "  [ -s \"$URLS\" ] || return 1\n"
        "  while IFS= read -r url; do\n"
        "    file=\"$DEB_DIR/${url##*/}\"\n"
        "    curl -fL --retry 3 --connect-timeout 30 -o \"$file\" \"$url\" || return 1\n"
        "  done < \"$URLS\"\n"
        "  ls \"$DEB_DIR\"/*.deb >/dev/null 2>&1\n"
        "}\n"
        "install_fex_debs_from_launchpad_api() {\n"
        "  download_fex_debs_from_launchpad_api\n"
        "  sudo env DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \"$LOGDIR\"/debs/*.deb\n"
        "}\n"
        "build_fex_from_source() {\n"
        "  SRC=/tmp/hase-fex-src\n"
        "  BUILD=\"$SRC/Build\"\n"
        "  sudo env DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \\\n"
        "    git cmake ninja-build pkgconf clang llvm lld binfmt-support \\\n"
        "    libssl-dev libfmt-dev libxxhash-dev python3-setuptools \\\n"
        "    g++-x86-64-linux-gnu squashfs-tools squashfuse erofs-utils erofs-fuse\n"
        "  rm -rf \"$SRC\"\n"
        "  git clone --depth 1 --branch FEX-2604 --recurse-submodules --shallow-submodules \\\n"
        "    https://github.com/FEX-Emu/FEX.git \"$SRC\"\n"
        "  mkdir -p \"$BUILD\"\n"
        "  cd \"$BUILD\"\n"
        "  CC=clang CXX=clang++ cmake -DCMAKE_INSTALL_PREFIX=/usr \\\n"
        "    -DCMAKE_BUILD_TYPE=Release -DUSE_LINKER=lld -DENABLE_LTO=True \\\n"
        "    -DBUILD_TESTING=False -DENABLE_ASSERTIONS=False -G Ninja ..\n"
        "  ninja\n"
        "  sudo ninja install\n"
        "  sudo ninja binfmt_misc || true\n"
        "}\n"
        "latest_package() {\n"
        "  prefix=\"$1\"\n"
        "  apt-cache search --names-only \"^${prefix}\" | awk '{print $1}' | sort -V | tail -n 1\n"
        "}\n"
        "pick_package() {\n"
        "  plain=\"$1\"\n"
        "  if apt-cache show \"$plain\" >/dev/null 2>&1; then\n"
        "    printf '%s\\n' \"$plain\"\n"
        "    return 0\n"
        "  fi\n"
        "  found=\"$(latest_package \"${plain}-\")\"\n"
        "  if [ -n \"$found\" ] && apt-cache show \"$found\" >/dev/null 2>&1; then\n"
        "    printf '%s\\n' \"$found\"\n"
        "    return 0\n"
        "  fi\n"
        "  return 1\n"
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
        "export FEX_APP_DATA_LOCATION=\"$ROOT/\"\n"
        "rootfs_present() {\n"
        "  find \"$ROOT/RootFS\" -maxdepth 1 \\( -name 'Ubuntu_*.ero' -o -name 'Ubuntu_*.sqsh' \\) -print -quit 2>/dev/null | grep -q .\n"
        "}\n"
        "write_env() {\n"
        "  ROOTFS_FILE=\"$(find \"$ROOT/RootFS\" -maxdepth 1 \\( -name 'Ubuntu_*.ero' -o -name 'Ubuntu_*.sqsh' \\) -print -quit 2>/dev/null || true)\"\n"
        "  {\n"
        "    printf 'export FEX_APP_DATA_LOCATION=%s/\\n' \"$ROOT\"\n"
        "    if [ -n \"$ROOTFS_FILE\" ]; then\n"
        "      printf 'export FEX_ROOTFS=%s\\n' \"$ROOTFS_FILE\"\n"
        "    fi\n"
        "  } > \"$ENV\"\n"
        "}\n"
        "if ! command -v FEXBash >/dev/null 2>&1 || ! command -v FEXRootFSFetcher >/dev/null 2>&1; then\n"
        "  note 'FEX binaries missing; setting up packages inside the HaSe VM.'\n"
        "  must_step 'Refreshing Ubuntu package lists' \"$LOGDIR/apt-update.log\" \\\n"
        "    sudo apt-get update -qq\n"
        "  must_step 'Installing bootstrap tools' \"$LOGDIR/bootstrap-tools.log\" \\\n"
        "    sudo env DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \\\n"
        "    ca-certificates curl software-properties-common fuse3 \\\n"
        "    squashfs-tools squashfuse\n"
        "  if ! apt-cache policy | grep -q '/fex-emu/fex/ubuntu'; then\n"
        "    retry_step 'Adding FEX-Emu Ubuntu PPA' \"$LOGDIR/fex-ppa.log\" 4 \\\n"
        "      sudo add-apt-repository -y ppa:fex-emu/fex\n"
        "    [ \"$?\" -eq 0 ] || exit 1\n"
        "  else\n"
        "    note 'FEX PPA already configured.'\n"
        "  fi\n"
        "  FEX_PACKAGE_SOURCE=apt\n"
        "  if ! retry_step 'Refreshing FEX package index' \"$LOGDIR/fex-apt-update.log\" 6 refresh_fex_index; then\n"
        "    note 'PPA apt host is unreachable; falling back to Launchpad API .deb downloads.'\n"
        "    if ! run_step 'Installing FEX packages from Launchpad API' \"$LOGDIR/fex-launchpad-debs.log\" install_fex_debs_from_launchpad_api; then\n"
        "      if [ \"${HASE_FEX_BUILD_FROM_SOURCE:-0}\" = 1 ]; then\n"
        "        note 'Binary install failed; building FEX from source because HASE_FEX_BUILD_FROM_SOURCE=1.'\n"
        "        must_step 'Building and installing FEX from source' \"$LOGDIR/fex-source-build.log\" build_fex_from_source\n"
        "      else\n"
        "        echo 'Binary FEX install failed.' >&2\n"
        "        echo 'To try the long source build fallback, rerun with HASE_FEX_BUILD_FROM_SOURCE=1.' >&2\n"
        "        exit 1\n"
        "      fi\n"
        "    fi\n"
        "    FEX_PACKAGE_SOURCE=launchpad-api\n"
        "  fi\n"
        "  if [ \"$FEX_PACKAGE_SOURCE\" = apt ]; then\n"
        "    FEX_PACKAGE=\"$(pick_package fex-emu-armv8.0 || true)\"\n"
        "    if [ -z \"$FEX_PACKAGE\" ]; then\n"
        "      FEX_PACKAGE=\"$(pick_package fex-emu-armv8.2 || true)\"\n"
        "    fi\n"
        "    if [ -z \"$FEX_PACKAGE\" ]; then\n"
        "      FEX_PACKAGE=\"$(pick_package fex-emu-armv8.4 || true)\"\n"
        "    fi\n"
        "    if [ -z \"$FEX_PACKAGE\" ]; then\n"
        "      echo 'Could not find an installable FEX emulator package in apt.' >&2\n"
        "      echo 'Try: apt-cache search --names-only \"^fex-emu\"' >&2\n"
        "      exit 1\n"
        "    fi\n"
        "    FEX_PACKAGES=\"$FEX_PACKAGE\"\n"
        "    case \"$FEX_PACKAGE\" in\n"
        "      fex-emu-armv8.*-*) suffix=\"${FEX_PACKAGE#fex-emu-armv8.0-}\"; suffix=\"${suffix#fex-emu-armv8.2-}\"; suffix=\"${suffix#fex-emu-armv8.4-}\" ;;\n"
        "      *) suffix='' ;;\n"
        "    esac\n"
        "    for base in fex-emu-binfmt32 fex-emu-binfmt64 fex-emu-wine; do\n"
        "      pkg=\"\"\n"
        "      if [ -n \"$suffix\" ] && apt-cache show \"${base}-${suffix}\" >/dev/null 2>&1; then\n"
        "        pkg=\"${base}-${suffix}\"\n"
        "      else\n"
        "        pkg=\"$(pick_package \"$base\" || true)\"\n"
        "      fi\n"
        "      if [ -n \"$pkg\" ]; then\n"
        "        FEX_PACKAGES=\"$FEX_PACKAGES $pkg\"\n"
        "      fi\n"
        "    done\n"
        "    note \"Selected FEX packages: $FEX_PACKAGES\"\n"
        "    must_step 'Installing FEX emulator packages' \"$LOGDIR/fex-packages.log\" \\\n"
        "      sudo env DEBIAN_FRONTEND=noninteractive apt-get install -y -qq $FEX_PACKAGES\n"
        "  fi\n"
        "else\n"
        "  note 'FEX binaries already installed.'\n"
        "fi\n"
        "if ! command -v FEXBash >/dev/null 2>&1 || ! command -v FEXRootFSFetcher >/dev/null 2>&1; then\n"
        "  echo 'FEX install completed, but FEXBash/FEXRootFSFetcher is still missing.' >&2\n"
        "  exit 1\n"
        "fi\n"
        "if ! rootfs_present; then\n"
        "  note 'No FEX rootfs found; downloading Ubuntu 24.04 x86_64 rootfs.'\n"
        "  retry_step 'Fetching FEX x86_64 rootfs' \"$LOGDIR/fex-rootfs.log\" 3 fetch_rootfs\n"
        "else\n"
        "  note 'FEX rootfs already present.'\n"
        "fi\n"
        "must_step 'Writing HaSe FEX environment' \"$LOGDIR/fex-env.log\" write_env\n"
        ". \"$ENV\"\n"
        "must_step 'Testing FEXBash x86_64 shell' \"$LOGDIR/fex-test.log\" FEXBash -c 'uname -m'\n"
        "note \"FEX ready at $ROOT\"\n"
        "printf '>> Logs are in %s if anything needs inspection.\\n' \"$LOGDIR\"\n",
        0755);

    path_join(path, sizeof path, runtime, "install-steam.sh");
    write_file(path,
        "#!/bin/sh\n"
        "# Install and configure Linux Steam for the HaSe VM.\n"
        "set -eu\n"
        "/mnt/hase/runtime/start-session.sh >/dev/null\n"
        "/mnt/hase/runtime/install-fex.sh\n"
        ". /mnt/hase/fex/env.sh\n"
        "LOGDIR=/tmp/hase-steam-install\n"
        "mkdir -p \"$LOGDIR\"\n"
        "banner() {\n"
        "  printf '\\n'\n"
        "  printf '========================================\\n'\n"
        "  printf ' HaSe Steam Integration\\n'\n"
        "  printf ' Linux Steam Automated Setup\\n'\n"
        "  printf '========================================\\n'\n"
        "}\n"
        "note() {\n"
        "  printf '>> %s\\n' \"$1\"\n"
        "}\n"
        "run_step() {\n"
        "  label=\"$1\"; log=\"$2\"; shift 2\n"
        "  status=\"$LOGDIR/step-$$.status\"\n"
        "  rm -f \"$status\"\n"
        "  printf '  %-54s ' \"$label\"\n"
        "  ( set +e; \"$@\" >\"$log\" 2>&1; rc=$?; printf '%s\\n' \"$rc\" >\"$status\" ) &\n"
        "  pid=$!\n"
        "  tick=0\n"
        "  while [ ! -s \"$status\" ]; do\n"
        "    tick=$((tick + 1))\n"
        "    case $((tick % 4)) in 0) mark='|' ;; 1) mark='/' ;; 2) mark='-' ;; *) mark='+' ;; esac\n"
        "    printf '\\r  %-54s [%s]' \"$label\" \"$mark\"\n"
        "    sleep 0.2\n"
        "  done\n"
        "  wait \"$pid\" 2>/dev/null || true\n"
        "  rc=\"$(cat \"$status\")\"; rm -f \"$status\"\n"
        "  if [ \"$rc\" = 0 ]; then printf '\\r  %-54s [ok]\\n' \"$label\"; return 0; fi\n"
        "  printf '\\r  %-54s [failed]\\n' \"$label\" >&2\n"
        "  printf '  log: %s\\n' \"$log\" >&2\n"
        "  tail -n 40 \"$log\" >&2 || true\n"
        "  return \"$rc\"\n"
        "}\n"
        "must_step() {\n"
        "  run_step \"$@\" || exit \"$?\"\n"
        "}\n"
        "ensure_swap() {\n"
        "  if swapon --noheadings --show=NAME 2>/dev/null | grep -qx '/swapfile'; then\n"
        "    sudo sysctl -w vm.swappiness=80 >/dev/null 2>&1 || true\n"
        "    return 0\n"
        "  fi\n"
        "  if [ ! -f /swapfile ]; then\n"
        "    size=\"${HASE_SWAP_SIZE:-8G}\"\n"
        "    sudo fallocate -l \"$size\" /swapfile 2>/dev/null || sudo dd if=/dev/zero of=/swapfile bs=1M count=8192 status=none\n"
        "    sudo chmod 600 /swapfile\n"
        "    sudo mkswap /swapfile >/dev/null\n"
        "  fi\n"
        "  sudo swapon /swapfile 2>/dev/null || { sudo mkswap /swapfile >/dev/null && sudo swapon /swapfile; }\n"
        "  sudo sysctl -w vm.swappiness=80 >/dev/null 2>&1 || true\n"
        "}\n"
        "banner\n"
        "  need_ui_deps=0\n"
        "  for pkg in xvfb matchbox-window-manager x11-apps xdotool x11-utils wmctrl netpbm dbus-x11 xdg-desktop-portal xdg-desktop-portal-gtk upower libgl1-mesa-dri libglx-mesa0 mesa-vulkan-drivers vulkan-tools; do\n"
        "    dpkg-query -W -f='${Status}' \"$pkg\" 2>/dev/null | grep -q 'install ok installed' || need_ui_deps=1\n"
        "  done\n"
        "  if [ \"$need_ui_deps\" = 1 ]; then\n"
        "    must_step 'Installing UI dependencies' \"$LOGDIR/ui-deps.log\" \\\n"
        "      sh -c 'sudo apt-get update -qq && sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -qq xvfb matchbox-window-manager x11-apps xdotool x11-utils wmctrl netpbm dbus-x11 xdg-desktop-portal xdg-desktop-portal-gtk upower libgl1-mesa-dri libglx-mesa0 mesa-vulkan-drivers vulkan-tools'\n"
        "    /mnt/hase/runtime/start-session.sh >/dev/null\n"
        "  fi\n"
        "  note 'Configuring kernel for Steam sandbox...'\n"
        "  sudo sysctl -w kernel.unprivileged_userns_clone=1 >/dev/null 2>&1 || true\n"
        "  sudo sysctl -w kernel.apparmor_restrict_unprivileged_userns=0 >/dev/null 2>&1 || true\n"
        "  must_step 'Ensuring Steam swap space' \"$LOGDIR/swap.log\" ensure_swap\n"
        "  note 'Configuring X11 permissions...'\n"
        "  export DISPLAY=\"${HASE_DISPLAY:-:99}\"\n"
        "  xhost +local: >/dev/null 2>&1 || true\n"
        "if [ ! -f /mnt/hase/steam-env/usr/bin/steam ]; then\n"
        "  note 'Downloading Linux Steam Bootstrapper...'\n"
        "  must_step 'Fetching Steam .deb' \"$LOGDIR/steam-download.log\" \\\n"
        "    curl -fL --retry 3 -o \"/tmp/steam.deb\" \\\n"
        "    \"https://repo.steampowered.com/steam/archive/precise/steam_latest.deb\"\n"
        "  must_step 'Extracting Steam' \"$LOGDIR/steam-extract.log\" \\\n"
        "    sh -c \"mkdir -p /tmp/steam-deb && dpkg -x /tmp/steam.deb /tmp/steam-deb && cp -r /tmp/steam-deb/usr /mnt/hase/steam-env/\"\n"
        "  rm -rf /tmp/steam.deb /tmp/steam-deb\n"
        "else\n"
        "  note 'Steam environment already present.'\n"
        "fi\n"
        "note 'Steam ready to launch via hasectl steam.'\n",
        0755);

    path_join(path, sizeof path, runtime, "launch-steam.sh");
    write_file(path,
        "#!/bin/sh\n"
        "set -eu\n"
        "/mnt/hase/runtime/start-session.sh\n"
        "/mnt/hase/runtime/install-steam.sh\n"
        ". /mnt/hase/fex/env.sh\n"
        "export DISPLAY=\"${HASE_DISPLAY:-:99}\"\n"
        "export CHEESEBRIDGE_HOST=\"${CHEESEBRIDGE_HOST:-tcp:host.lima.internal:43210}\"\n"
        "# Use Mesa Lavapipe (software Vulkan) for Steam UI rendering.\n"
        "# CheeseBridge ICD does not expose VK_KHR_xlib_surface, which Steam needs.\n"
        "# Games will use CheeseBridge via Proton's own VK_ICD_FILENAMES override.\n"
        "export VK_ICD_FILENAMES=\"/usr/share/vulkan/icd.d/lvp_icd.json\"\n"
        "export LIBGL_ALWAYS_SOFTWARE=1\n"
        "export GALLIUM_DRIVER=\"${GALLIUM_DRIVER:-llvmpipe}\"\n"
        "export MESA_LOADER_DRIVER_OVERRIDE=\"${MESA_LOADER_DRIVER_OVERRIDE:-llvmpipe}\"\n"
        "export __GLX_VENDOR_LIBRARY_NAME=\"${__GLX_VENDOR_LIBRARY_NAME:-mesa}\"\n"
        "export MESA_GL_VERSION_OVERRIDE=4.3\n"
        "export SDL_VIDEODRIVER=\"${SDL_VIDEODRIVER:-x11}\"\n"
        "export CHEESEBRIDGE_HOST_ICD=\"/mnt/hase/vulkan/icd.d/cheesebridge_icd.json\"\n"
        "export CHEESEBRIDGE_ICD_PATH=\"/mnt/hase/vulkan/icd.d/libCheeseBridge_icd.so\"\n"
        "steam_running() {\n"
        "  pgrep -f '/\\.local/share/Steam/ubuntu12_32/steam([[:space:]]|$)' >/dev/null 2>&1 || \\\n"
        "  pgrep -f '/\\.local/share/Steam/steam\\.sh' >/dev/null 2>&1 || \\\n"
        "  pgrep -f 'steamwebhelper_sniper_wrap' >/dev/null 2>&1\n"
        "}\n"
        "steam_pid() {\n"
        "  pgrep -f '/\\.local/share/Steam/ubuntu12_32/steam([[:space:]]|$)' | head -n1 || \\\n"
        "  pgrep -f '/\\.local/share/Steam/steam\\.sh' | head -n1 || \\\n"
        "  pgrep -f 'steamwebhelper_sniper_wrap' | head -n1 || true\n"
        "}\n"
        "mkdir -p /tmp/hase\n"
        "if [ -f /tmp/hase/dbus.env ]; then . /tmp/hase/dbus.env >/dev/null 2>&1 || true; fi\n"
        "if command -v dbus-launch >/dev/null 2>&1; then\n"
        "  if [ -z \"${DBUS_SESSION_BUS_PID:-}\" ] || ! kill -0 \"$DBUS_SESSION_BUS_PID\" >/dev/null 2>&1; then\n"
        "    dbus-launch --sh-syntax >/tmp/hase/dbus.env\n"
        "    . /tmp/hase/dbus.env\n"
        "  fi\n"
        "  export DBUS_SESSION_BUS_ADDRESS DBUS_SESSION_BUS_PID\n"
        "fi\n"
        "if [ -f /mnt/hase/steam-env/usr/bin/steam ]; then\n"
        "  if steam_running; then\n"
        "    echo \"Steam is already running (PID: $(steam_pid | xargs)).\"\n"
        "  else\n"
        "    echo 'Launching Steam in hidden HaSe session...'\n"
        "    STEAM_ARGS=\"${HASE_STEAM_ARGS:--cef-disable-gpu --cef-disable-gpu-compositing --cef-disable-gpu-rasterization --cef-disable-zero-copy --cef-disable-dev-shm-usage --cef-disable-accelerated-video-decode -no-cef-sandbox}\"\n"
        "    nohup env HASE_STEAM_ARGS=\"$STEAM_ARGS\" FEXBash -c \\\n"
        "      'STEAM_RUNTIME=1 exec /mnt/hase/steam-env/usr/bin/steam $HASE_STEAM_ARGS' \\\n"
        "      >/tmp/steam.log 2>&1 &\n"
        "  fi\n"
        "  # Experimental fast path: raw XWD frames and shared-file input queue.\n"
        "  pkill -f capture-daemon.sh >/dev/null 2>&1 || true\n"
        "  pkill -f input-daemon.sh >/dev/null 2>&1 || true\n"
        "  HASE_CAPTURE_FORMAT=\"${HASE_CAPTURE_FORMAT:-xwd}\" \\\n"
        "  HASE_CAPTURE_DELAY=\"${HASE_CAPTURE_DELAY:-0.016}\" \\\n"
        "    nohup /mnt/hase/runtime/capture-daemon.sh >/tmp/hase/capture.log 2>&1 &\n"
        "  HASE_INPUT_DELAY=\"${HASE_INPUT_DELAY:-0.004}\" \\\n"
        "    nohup /mnt/hase/runtime/input-daemon.sh >/tmp/hase/input.log 2>&1 &\n"
        "  echo 'Tailing logs for 15s (Ctrl+C to stop)...'\n"
        "  timeout 15 tail -f /tmp/steam.log 2>/dev/null || true\n"
        "  echo '\\nCheck /tmp/steam.log inside VM for further progress.'\n"
        "else\n"
        "  echo 'Steam not found; run hasectl install-steam <bottle> first.' >&2\n"
        "  exit 1\n"
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

static int cmd_refresh_runtime(const hase_config_t *cfg) {
    ensure_bottle_exists(cfg);
    write_lima_yaml(cfg);
    sync_existing_lima_yaml(cfg, true);
    write_runtime_scripts(cfg);
    write_metadata(cfg);
    printf("Refreshed HaSe runtime scripts: %s/runtime\n", cfg->bottle);
    printf("Manual FEX installer path inside VM: /mnt/hase/runtime/install-fex.sh\n");
    return 0;
}

static int cmd_start(const hase_config_t *cfg) {
    ensure_bottle_exists(cfg);
    write_runtime_scripts(cfg);
    write_lima_yaml(cfg);
    int rc = 0;
    if (lima_instance_exists(cfg)) {
        sync_existing_lima_yaml(cfg, false);
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

static int cmd_steam(const hase_config_t *cfg, const char *argv0) {
    ensure_bottle_exists(cfg);
    write_runtime_scripts(cfg);

    /* Start hase_window_host on macOS in the background. */
    char host_path[PATH_MAX];
    if (realpath(argv0, host_path)) {
        char *slash = strrchr(host_path, '/');
        if (slash) {
            strcpy(slash + 1, "hase_window_host");
            if (access(host_path, X_OK) == 0) {
                printf("Starting macOS window manager: %s %s\n", host_path, cfg->name);
                pid_t pid = fork();
                if (pid == 0) {
                    execl(host_path, host_path, cfg->name, (char *)NULL);
                    _exit(1);
                }
            }
        }
    }

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

static int cmd_install_steam(const hase_config_t *cfg) {
    ensure_bottle_exists(cfg);
    write_runtime_scripts(cfg);

    if (!lima_instance_exists(cfg)) {
        fprintf(stderr, "hasectl: VM not running; start it first with: "
                        "hasectl start %s\n", cfg->name);
        return 1;
    }

    char *argv[] = {
        "limactl", "--tty=false", "shell", "--workdir=/mnt/hase", (char *)cfg->vm_name,
        "sh", "-lc", "/mnt/hase/runtime/install-steam.sh",
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
    if (!strcmp(argv[1], "steam")) return cmd_steam(&cfg, argv[0]);
    if (!strcmp(argv[1], "demo-window")) return cmd_demo_window(&cfg);
    if (!strcmp(argv[1], "windows")) return cmd_windows(&cfg);
    if (!strcmp(argv[1], "refresh-runtime")) return cmd_refresh_runtime(&cfg);
    if (!strcmp(argv[1], "install-icd")) return cmd_install_icd(&cfg, argv[0]);
    if (!strcmp(argv[1], "install-fex")) return cmd_install_fex(&cfg);
    if (!strcmp(argv[1], "install-steam")) return cmd_install_steam(&cfg);
    if (!strcmp(argv[1], "run-clear-demo")) return cmd_run_clear_demo(&cfg);
    if (!strcmp(argv[1], "run-triangle-demo")) return cmd_run_triangle_demo(&cfg);
    if (!strcmp(argv[1], "paths")) return cmd_paths(&cfg);

    usage(stderr);
    die("unknown command: %s", argv[1]);
}
