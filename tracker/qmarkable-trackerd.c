/*
 * qmarkable-trackerd — notebook study-time tracker for reMarkable.
 *
 * Counts time spent with a notebook open on screen. The homepage QML mod
 * writes /run/qmarkable/current-doc (tmpfs: RAM only) whenever that changes:
 *
 *     "<id>\n<name>\n"   a notebook is open, the screen is awake and visible
 *     "\n"               anything else (navigator, homepage, PDF/ebook,
 *                        sleeping, locked)
 *
 * The tracker sleeps in poll() on an inotify watch, so it only wakes when the
 * marker changes (a few times per session) plus one checkpoint every few
 * minutes while a session is running. Session length is measured with
 * CLOCK_MONOTONIC, which stops during suspend, so sleep can never be counted
 * even if a sleep transition were missed.
 *
 * Storage (written on session end and at checkpoints):
 *   <datadir>/index.txt           "# qmarkable 2 tzoff=<min> today=<key>"
 *                                 then "<YYYY-MM-DD> <secs>" per day
 *   <datadir>/days/<key>.txt      "a <24 ints>" (seconds per local hour),
 *                                 "d <id> <secs> <name>" per notebook
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define CONFIG_PATH "/home/root/.config/qmarkable/tracker.conf"
#define DEFAULT_DATADIR "/home/root/.local/share/qmarkable"
#define RUNDIR "/run/qmarkable"
#define MARKER_NAME "current-doc"
#define MAX_DOCS 256
#define MAX_DAYS 8192

static const char *docfile = RUNDIR "/" MARKER_NAME;

struct doc {
    char id[64];
    char name[160];
    double secs;
};

struct day {
    char key[11];
    double h[24];
    struct doc docs[MAX_DOCS];
    int ndocs;
};

struct idx {
    char key[11];
    long secs;
};

static struct {
    char datadir[256];
    char tz[128];
    int checkpoint_sec;
} cfg;

static struct day cur;
static struct idx days[MAX_DAYS];
static int ndays;
static int dirty;
static volatile sig_atomic_t stop;

/* ---------- utils ---------- */

static double mono_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static double real_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void logmsg(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "qmarkable-trackerd: ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static void trim(char *s)
{
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' || s[n - 1] == '\t'))
        s[--n] = 0;
    size_t i = 0;
    while (s[i] == ' ' || s[i] == '\t')
        i++;
    if (i)
        memmove(s, s + i, n - i + 1);
}

static void mkdirs(const char *path)
{
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}

static void load_config(void)
{
    snprintf(cfg.datadir, sizeof cfg.datadir, "%s", DEFAULT_DATADIR);
    cfg.tz[0] = 0;
    cfg.checkpoint_sec = 300;
    FILE *f = fopen(CONFIG_PATH, "r");
    if (!f)
        return;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        trim(line);
        if (!line[0] || line[0] == '#')
            continue;
        char *eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq = 0;
        char *k = line, *v = eq + 1;
        trim(k);
        trim(v);
        if (!strcmp(k, "tz"))
            snprintf(cfg.tz, sizeof cfg.tz, "%s", v);
        else if (!strcmp(k, "checkpoint"))
            cfg.checkpoint_sec = atoi(v);
        else if (!strcmp(k, "datadir"))
            snprintf(cfg.datadir, sizeof cfg.datadir, "%s", v);
    }
    fclose(f);
    if (cfg.checkpoint_sec < 30)
        cfg.checkpoint_sec = 30;
}

/* ---------- timezone ----------
 * Follows the system timezone (/etc/localtime, which xochitl's date/time
 * settings change via systemd-timedated). musl caches the zone keyed on the
 * TZ string, so TZ points at the symlink's resolved target and is re-pointed
 * whenever that changes. A "tz=" config entry overrides this. */
static char tz_current[300];
static double tz_checked = -1e9;

static void sync_timezone(int force)
{
    double now = mono_now();
    if (!force && now - tz_checked < 60)
        return;
    tz_checked = now;
    char want[300];
    if (cfg.tz[0]) {
        if (cfg.tz[0] == '/')
            snprintf(want, sizeof want, ":%s", cfg.tz);
        else if (strchr(cfg.tz, '/'))
            snprintf(want, sizeof want, ":/usr/share/zoneinfo/%s", cfg.tz);
        else
            snprintf(want, sizeof want, "%s", cfg.tz); /* POSIX TZ string */
    } else {
        char target[PATH_MAX];
        char *rp = realpath("/etc/localtime", target);
        snprintf(want, sizeof want, ":%s", rp ? rp : "/etc/localtime");
    }
    if (strcmp(want, tz_current)) {
        snprintf(tz_current, sizeof tz_current, "%s", want);
        setenv("TZ", tz_current, 1);
        tzset();
        logmsg("timezone: %s", tz_current + (tz_current[0] == ':'));
    }
}

static void today_key(char *out, long *tzoff_min)
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(out, 11, "%Y-%m-%d", &tm);
    if (tzoff_min)
        *tzoff_min = tm.tm_gmtoff / 60;
}

/* ---------- storage ---------- */

static void day_path(char *out, size_t n, const char *key)
{
    snprintf(out, n, "%s/days/%s.txt", cfg.datadir, key);
}

static void load_day(const char *key)
{
    memset(&cur, 0, sizeof cur);
    snprintf(cur.key, sizeof cur.key, "%s", key);
    char p[400];
    day_path(p, sizeof p, key);
    FILE *f = fopen(p, "r");
    if (!f)
        return;
    char line[600];
    while (fgets(line, sizeof line, f)) {
        if (line[0] == 'a' && line[1] == ' ') {
            char *s = line + 2;
            for (int i = 0; i < 24; i++) {
                char *end;
                long v = strtol(s, &end, 10);
                if (end == s)
                    break;
                cur.h[i] = v;
                s = end;
            }
        } else if (line[0] == 'd' && line[1] == ' ' && cur.ndocs < MAX_DOCS) {
            struct doc *d = &cur.docs[cur.ndocs];
            char *s = line + 2, *sp = strchr(s, ' ');
            if (!sp)
                continue;
            *sp = 0;
            snprintf(d->id, sizeof d->id, "%s", s);
            s = sp + 1;
            char *end;
            d->secs = strtol(s, &end, 10);
            if (*end == ' ')
                end++;
            snprintf(d->name, sizeof d->name, "%s", end);
            trim(d->name);
            cur.ndocs++;
        }
    }
    fclose(f);
}

static int write_atomic(const char *path, const char *data, size_t len)
{
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0)
        return -1;
    size_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, data + off, len - off);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            close(fd);
            unlink(tmp);
            return -1;
        }
        off += (size_t)w;
    }
    close(fd);
    return rename(tmp, path);
}

static long day_total(void)
{
    double t = 0;
    for (int i = 0; i < 24; i++)
        t += cur.h[i];
    return lround(t);
}

static void index_set(const char *key, long secs)
{
    for (int i = 0; i < ndays; i++)
        if (!strcmp(days[i].key, key)) {
            days[i].secs = secs;
            return;
        }
    if (ndays >= MAX_DAYS)
        return;
    int pos = ndays;
    while (pos > 0 && strcmp(days[pos - 1].key, key) > 0)
        pos--;
    memmove(&days[pos + 1], &days[pos], (size_t)(ndays - pos) * sizeof days[0]);
    snprintf(days[pos].key, sizeof days[pos].key, "%s", key);
    days[pos].secs = secs;
    ndays++;
}

static void load_index(void)
{
    char p[400];
    snprintf(p, sizeof p, "%s/index.txt", cfg.datadir);
    FILE *f = fopen(p, "r");
    if (!f)
        return;
    char line[128];
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#')
            continue;
        char key[16];
        long secs;
        if (sscanf(line, "%10s %ld", key, &secs) == 2 && strlen(key) == 10)
            index_set(key, secs);
    }
    fclose(f);
}

static void write_index(void)
{
    static char buf[MAX_DAYS * 24 + 128];
    char tk[11];
    long tz;
    today_key(tk, &tz);
    int n = snprintf(buf, sizeof buf, "# qmarkable 2 tzoff=%ld today=%s\n", tz, tk);
    for (int i = 0; i < ndays && n < (int)sizeof buf - 32; i++)
        if (days[i].secs > 0)
            n += snprintf(buf + n, sizeof buf - n, "%s %ld\n", days[i].key, days[i].secs);
    char p[400];
    snprintf(p, sizeof p, "%s/index.txt", cfg.datadir);
    if (write_atomic(p, buf, (size_t)n) < 0)
        logmsg("write %s failed: %s", p, strerror(errno));
}

static void flush_day(void)
{
    if (!cur.key[0])
        return;
    static char buf[MAX_DOCS * 256 + 512];
    int n = snprintf(buf, sizeof buf, "a");
    for (int i = 0; i < 24; i++)
        n += snprintf(buf + n, sizeof buf - n, " %ld", lround(cur.h[i]));
    n += snprintf(buf + n, sizeof buf - n, "\n");
    for (int i = 0; i < cur.ndocs; i++)
        n += snprintf(buf + n, sizeof buf - n, "d %s %ld %s\n", cur.docs[i].id, lround(cur.docs[i].secs), cur.docs[i].name);
    char p[400];
    day_path(p, sizeof p, cur.key);
    if (write_atomic(p, buf, (size_t)n) < 0)
        logmsg("write %s failed: %s", p, strerror(errno));
    index_set(cur.key, day_total());
}

static void flush_all(void)
{
    if (!dirty)
        return;
    sync_timezone(0);
    flush_day();
    write_index();
    dirty = 0;
}

static struct doc *doc_slot(const char *id, const char *name)
{
    for (int i = 0; i < cur.ndocs; i++)
        if (!strcmp(cur.docs[i].id, id)) {
            if (name[0] && strcmp(cur.docs[i].name, name))
                snprintf(cur.docs[i].name, sizeof cur.docs[i].name, "%s", name); /* renamed */
            return &cur.docs[i];
        }
    if (cur.ndocs >= MAX_DOCS)
        return NULL;
    struct doc *d = &cur.docs[cur.ndocs++];
    snprintf(d->id, sizeof d->id, "%s", id);
    snprintf(d->name, sizeof d->name, "%s", name);
    d->secs = 0;
    return d;
}

/* Add [start,end) (unix seconds) to hourly buckets, splitting at local hour
 * and day boundaries. */
static void add_span(double start, double end, const char *id, const char *name)
{
    while (start < end) {
        time_t s = (time_t)floor(start);
        struct tm tm;
        localtime_r(&s, &tm);
        char key[11];
        strftime(key, sizeof key, "%Y-%m-%d", &tm);
        if (strcmp(key, cur.key)) {
            if (dirty) {
                flush_day(); /* finish previous day before switching */
                write_index();
            }
            load_day(key);
            dirty = 1;
        }
        double hour_end = (double)(s - tm.tm_min * 60 - tm.tm_sec) + 3600.0;
        double seg = (end < hour_end ? end : hour_end) - start;
        cur.h[tm.tm_hour] += seg;
        struct doc *d = doc_slot(id, name);
        if (d)
            d->secs += seg;
        dirty = 1;
        start += seg;
    }
}

/* ---------- sessions ---------- */

static struct {
    int open;
    double start_real; /* wall clock: where the time goes */
    double start_mono; /* monotonic: how long it was (excludes suspend) */
    char id[64];
    char name[160];
} sess;

/* Book time from the session start up to "now" and restart the session clock. */
static void sess_checkpoint(double now_real, double now_mono)
{
    if (!sess.open)
        return;
    double len = now_mono - sess.start_mono;
    if (len > 0) {
        /* anchor on the wall clock at start; length from the monotonic clock */
        sync_timezone(0);
        add_span(sess.start_real, sess.start_real + len, sess.id, sess.name);
    }
    sess.start_real = now_real;
    sess.start_mono = now_mono;
}

static void sess_end(double now_real, double now_mono)
{
    sess_checkpoint(now_real, now_mono);
    sess.open = 0;
}

static void sess_begin(const char *id, const char *name, double now_real, double now_mono)
{
    sess.open = 1;
    sess.start_real = now_real;
    sess.start_mono = now_mono;
    snprintf(sess.id, sizeof sess.id, "%s", id);
    snprintf(sess.name, sizeof sess.name, "%s", name);
}

/* Read the marker: returns 1 and fills id/name if a notebook is active. */
static int read_marker(char *id, size_t idn, char *name, size_t namen)
{
    id[0] = name[0] = 0;
    FILE *f = fopen(docfile, "r");
    if (!f)
        return 0;
    char line[512];
    if (fgets(line, sizeof line, f)) {
        trim(line);
        size_t j = 0; /* ids are uuids; keep only safe chars */
        for (size_t i = 0; line[i] && j < idn - 1; i++)
            if ((line[i] >= '0' && line[i] <= '9') || (line[i] >= 'a' && line[i] <= 'z') || (line[i] >= 'A' && line[i] <= 'Z') || line[i] == '-')
                id[j++] = line[i];
        id[j] = 0;
    }
    if (fgets(line, sizeof line, f)) {
        trim(line);
        snprintf(name, namen, "%s", line);
    }
    fclose(f);
    if (id[0] && !name[0])
        snprintf(name, namen, "(untitled)");
    return id[0] != 0;
}

/* Apply a marker state at the given time. */
static void apply_state(int active, const char *id, const char *name, double now_real, double now_mono)
{
    if (sess.open && active && !strcmp(sess.id, id)) {
        if (strcmp(sess.name, name))
            snprintf(sess.name, sizeof sess.name, "%s", name); /* renamed */
        return;
    }
    if (sess.open) {
        sess_end(now_real, now_mono);
        flush_all(); /* make the time visible on the homepage right away */
    }
    if (active)
        sess_begin(id, name, now_real, now_mono);
}

static void on_marker_change(void)
{
    char id[64], name[160];
    int active = read_marker(id, sizeof id, name, sizeof name);
    apply_state(active, id, name, real_now(), mono_now());
}

static void on_signal(int sig)
{
    (void)sig;
    stop = 1;
}

/* Test mode: "qmarkable-trackerd --replay" reads lines from stdin:
 *     <unix-time> open <id> <name...>
 *     <unix-time> close
 * Use QMARKABLE_DATADIR to keep test data away from the real files. */
static int replay(void)
{
    char line[512];
    while (fgets(line, sizeof line, stdin)) {
        double t;
        char verb[16];
        int used = 0;
        if (sscanf(line, "%lf %15s %n", &t, verb, &used) < 2)
            continue;
        if (!strcmp(verb, "open")) {
            char id[64] = "", name[160] = "";
            char *rest = line + used;
            char *sp = strchr(rest, ' ');
            if (sp) {
                *sp = 0;
                snprintf(name, sizeof name, "%s", sp + 1);
                trim(name);
            }
            snprintf(id, sizeof id, "%s", rest);
            trim(id);
            apply_state(1, id, name, t, t);
        } else {
            apply_state(0, "", "", t, t);
        }
    }
    flush_all();
    return 0;
}

int main(int argc, char **argv)
{
    load_config();
    if (getenv("QMARKABLE_DATADIR"))
        snprintf(cfg.datadir, sizeof cfg.datadir, "%s", getenv("QMARKABLE_DATADIR"));
    int replay_mode = argc > 1 && !strcmp(argv[1], "--replay");
    sync_timezone(1);
    setpriority(PRIO_PROCESS, 0, 19);

    char p[400];
    snprintf(p, sizeof p, "%s/days", cfg.datadir);
    mkdirs(p);

    load_index();
    char tk[11];
    today_key(tk, NULL);
    load_day(tk);
    dirty = 1;
    flush_all(); /* refresh index header (tz offset / today) for the UI */
    if (replay_mode)
        return replay();

    mkdirs(RUNDIR);
    int ifd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (ifd < 0 || inotify_add_watch(ifd, RUNDIR, IN_CLOSE_WRITE | IN_MOVED_TO | IN_DELETE) < 0) {
        logmsg("inotify: %s", strerror(errno));
        return 1;
    }
    logmsg("watching %s (checkpoint every %ds)", docfile, cfg.checkpoint_sec);
    on_marker_change(); /* pick up a notebook that is already open */

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal; /* no SA_RESTART: poll returns EINTR */
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);

    struct pollfd pfd = {.fd = ifd, .events = POLLIN};
    char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));

    while (!stop) {
        /* idle: sleep until the marker changes. In a session: also wake for
         * a periodic checkpoint so a crash or power loss loses little. */
        int timeout = -1;
        if (sess.open) {
            double left = cfg.checkpoint_sec - (mono_now() - sess.start_mono);
            timeout = left <= 0 ? 0 : (int)(left * 1000) + 1;
        }
        int r = poll(&pfd, 1, timeout);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            logmsg("poll: %s", strerror(errno));
            break;
        }
        if (r == 0) {
            sess_checkpoint(real_now(), mono_now());
            flush_all();
            continue;
        }
        int relevant = 0;
        for (;;) {
            ssize_t n = read(ifd, buf, sizeof buf);
            if (n <= 0)
                break;
            for (char *ptr = buf; ptr < buf + n;) {
                struct inotify_event *ev = (struct inotify_event *)ptr;
                if (ev->len && !strcmp(ev->name, MARKER_NAME))
                    relevant = 1;
                ptr += sizeof *ev + ev->len;
            }
        }
        if (relevant)
            on_marker_change();
    }
    if (sess.open)
        sess_end(real_now(), mono_now());
    flush_all();
    logmsg("exiting");
    return 0;
}
