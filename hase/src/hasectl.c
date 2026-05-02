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
        "  hasectl windows <bottle> [--root DIR]\n"
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
        "- location: \"https://cloud-images.ubuntu.com/releases/24.04/release/ubuntu-24.04-server-cloudimg-arm64.img\"\n"
        "  arch: \"aarch64\"\n"
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
        "      vulkan-tools wmctrl x11-utils x11-xserver-utils xdotool xvfb\n"
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
    printf("root=%s\n", cfg->root);
    printf("bottle=%s\n", cfg->bottle);
    printf("lima_config=%s\n", cfg->lima_yaml);
    return 0;
}

static int cmd_start(const hase_config_t *cfg) {
    ensure_bottle_exists(cfg);
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
    char *argv[] = {
        "limactl", "shell", "--workdir=/mnt/hase", (char *)cfg->vm_name,
        "sh", "-lc", "/mnt/hase/runtime/launch-steam.sh",
        NULL
    };
    return run_wait(argv);
}

static int cmd_windows(const hase_config_t *cfg) {
    ensure_bottle_exists(cfg);
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
    if (!strcmp(argv[1], "windows")) return cmd_windows(&cfg);
    if (!strcmp(argv[1], "paths")) return cmd_paths(&cfg);

    usage(stderr);
    die("unknown command: %s", argv[1]);
}
