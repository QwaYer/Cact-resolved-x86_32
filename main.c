/*
 * resolved — кэширующий DNS-резолвер CactOS (упрощённый аналог systemd-resolved).
 *
 * Ядро умеет резолвить A-записи само (rust_net_dns_resolve_a через /dev/net,
 * CACT_NETCTL_DNS_RESOLVE) — resolved делает этот сервис доступным другим
 * процессам: слушает AF_UNIX-сокет /run/resolved.sock, принимает по одной
 * строке-имени за соединение и отвечает "OK <IPv4>" либо "ERR <rc>".
 *
 * Дополнительно resolved кэширует успешные ответы на время TTL (по умолчанию
 * 60 c), чтобы не дёргать сеть на повторных запросах одного имени.
 *
 * Запускается супервизором cgoct как /sbin/resolved.
 *
 * /etc/resolved.conf (все ключи необязательны; создаётся при первом запуске):
 *   file=/var/log/resolved.log
 *   console=0
 *   cache_ttl=60
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>

#include <socket.h>
#include <poll.h>

#define CONFIG_PATH "/etc/resolved.conf"
#define SOCK_PATH   "/run/resolved.sock"
#define LOG_DEFAULT "/var/log/resolved.log"
#define LINE_MAX    256
#define CACHE_MAX   16
#define CACHE_TTL   60

static char log_path[128] = LOG_DEFAULT;
static int  console_on    = 0;
static int  cache_ttl     = CACHE_TTL;
static int  out_fd        = -1;

struct cache_entry {
    char     name[128];
    uint32_t ip_host;
    long     expire_sec;
};

static struct cache_entry cache[CACHE_MAX];
static int                cache_count = 0;

/* Конфиг по умолчанию: пишется при первом запуске, если файла ещё нет. */
static const char default_config[] =
    "# resolved config - auto-generated on first start.\n"
    "#\n"
    "# file      - журнал событий\n"
    "# console   - дублировать на /dev/console (0|1)\n"
    "# cache_ttl - время жизни кэша DNS-ответов (сек)\n"
    "\n"
    "file=/var/log/resolved.log\n"
    "console=0\n"
    "cache_ttl=60\n";

static void ensure_dir(const char *path) {
    (void)mkdir(path, 0755);
}

static void config_write_default(void) {
    int fd = open(CONFIG_PATH, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) return;
    write(fd, default_config, sizeof(default_config) - 1);
    close(fd);
}

static void config_load(void) {
    FILE *f = fopen(CONFIG_PATH, "r");
    if (!f) {
        config_write_default();
        f = fopen(CONFIG_PATH, "r");
        if (!f) return;
    }
    char line[160];
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\0') continue;
        char *eq = p;
        while (*eq && *eq != '=' && *eq != '\n') eq++;
        if (*eq != '=') continue;
        *eq = '\0';
        char *key = p;
        char *val = eq + 1;
        int vlen = (int)strlen(val);
        while (vlen > 0 && (val[vlen - 1] == '\n' || val[vlen - 1] == '\r' ||
                            val[vlen - 1] == ' ' || val[vlen - 1] == '\t'))
            val[--vlen] = '\0';
        if (strcmp(key, "file") == 0) {
            strncpy(log_path, val, sizeof(log_path) - 1);
            log_path[sizeof(log_path) - 1] = '\0';
        } else if (strcmp(key, "console") == 0) {
            console_on = (val[0] == '1' || val[0] == 'y' || val[0] == 'Y');
        } else if (strcmp(key, "cache_ttl") == 0) {
            int v = atoi(val);
            if (v >= 0 && v <= 86400) cache_ttl = v;
        }
    }
    fclose(f);
}

static void log_event(const char *msg) {
    if (out_fd >= 0) {
        write(out_fd, msg, strlen(msg));
    }
    if (console_on) {
        int cfd = open("/dev/console", O_WRONLY);
        if (cfd >= 0) {
            write(cfd, msg, strlen(msg));
            close(cfd);
        }
    }
}

static long mono_sec(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
        return ts.tv_sec;
    }
    return 0;
}

static int cache_lookup(const char *name, uint32_t *out) {
    long now = mono_sec();
    int i;
    for (i = 0; i < cache_count; i++) {
        if (strcmp(cache[i].name, name) == 0) {
            if (now < cache[i].expire_sec) {
                *out = cache[i].ip_host;
                return 1; /* hit */
            }
            return -1;    /* истёкшая запись */
        }
    }
    return 0;
}

static void cache_add(const char *name, uint32_t ip) {
    if (cache_ttl <= 0) return;
    int i;
    for (i = 0; i < cache_count; i++) {
        if (strcmp(cache[i].name, name) == 0) {
            cache[i].ip_host    = ip;
            cache[i].expire_sec = mono_sec() + cache_ttl;
            return;
        }
    }
    if (cache_count >= CACHE_MAX) cache_count = CACHE_MAX - 1;
    strncpy(cache[cache_count].name, name, sizeof(cache[cache_count].name) - 1);
    cache[cache_count].name[sizeof(cache[cache_count].name) - 1] = '\0';
    cache[cache_count].ip_host    = ip;
    cache[cache_count].expire_sec = mono_sec() + cache_ttl;
    cache_count++;
}

/* Резолв имени и запись ответа клиенту. */
static void handle_name(int cl, char *name) {
    char resp[192];
    uint32_t ip_host = 0;

    int hit = cache_lookup(name, &ip_host);
    if (hit == 1) {
        snprintf(resp, sizeof(resp), "OK %u.%u.%u.%u\n",
                 (unsigned)((ip_host >> 24) & 0xFF),
                 (unsigned)((ip_host >> 16) & 0xFF),
                 (unsigned)((ip_host >> 8) & 0xFF),
                 (unsigned)(ip_host & 0xFF));
        send(cl, resp, (uint32_t)strlen(resp), 0);
        char line[160];
        snprintf(line, sizeof(line), "resolved: cache hit %s -> %s",
                 name, resp + 3);
        log_event(line);
        return;
    }

    if (dns_resolve(name, &ip_host) == 0) {
        cache_add(name, ip_host);
        snprintf(resp, sizeof(resp), "OK %u.%u.%u.%u\n",
                 (unsigned)((ip_host >> 24) & 0xFF),
                 (unsigned)((ip_host >> 16) & 0xFF),
                 (unsigned)((ip_host >> 8) & 0xFF),
                 (unsigned)(ip_host & 0xFF));
    } else {
        snprintf(resp, sizeof(resp), "ERR no such name\n");
    }
    send(cl, resp, (uint32_t)strlen(resp), 0);

    char line[160];
    snprintf(line, sizeof(line), "resolved: %s %s", name, resp);
    log_event(line);
    printf("resolved: %s %s", name, resp);
}

static void handle_client(int cl) {
    char req[LINE_MAX];
    char b;
    int  got = 0;
    int  n;

    while (got < LINE_MAX - 1) {
        n = (int)recv(cl, &b, 1, 0);
        if (n <= 0) break;
        if (b == '\n' || b == '\r') break;
        req[got++] = b;
    }
    req[got] = '\0';

    if (got == 0) return;

    /* Только одно имя за соединение; лишние строки игнорируем. */
    handle_name(cl, req);
}

static int bind_listener(void) {
    int srv = socket(AF_UNIX, SOCK_STREAM, 0);
    if (srv < 0) return -1;

    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    strncpy(sa.sun_path, SOCK_PATH, sizeof(sa.sun_path) - 1);

    if (bind(srv, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        close(srv);
        return -1;
    }
    if (listen(srv, 4) < 0) {
        close(srv);
        return -1;
    }
    return srv;
}

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    printf("resolved: starting\n");
    config_load();
    ensure_dir("/var/log");
    ensure_dir("/run");

    out_fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (out_fd < 0) {
        printf("resolved: cannot open %s\n", log_path);
    }
    log_event("resolved: starting\n");

    int srv = -1;
    for (;;) {
        if (srv < 0) {
            srv = bind_listener();
            if (srv < 0) {
                sleep(3);
                continue;
            }
            printf("resolved: listening on %s\n", SOCK_PATH);
            log_event("resolved: listening\n");
        }

        struct pollfd pfd;
        pfd.fd = srv;
        pfd.events = POLLIN;
        pfd.revents = 0;

        if (poll(&pfd, 1, 1000) > 0 && (pfd.revents & POLLIN)) {
            int cl = accept(srv, 0, 0);
            if (cl >= 0) {
                handle_client(cl);
                close(cl);
            }
        }
    }
    return 0;
}
