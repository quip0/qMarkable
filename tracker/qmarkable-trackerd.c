/*
 * qmarkable-trackerd — passive pen-contact study tracker for reMarkable.
 *
 * Reads the pen digitizer's evdev node WITHOUT grabbing it (xochitl keeps
 * receiving every event). An EVIOCSMASK filter asks the kernel to deliver only
 * BTN_TOUCH (pen down / pen up), so the process sleeps in poll() and is woken
 * roughly twice per stroke — never for the hundreds of position/pressure
 * updates per second, and never at all while the pen is idle.
 *
 * Time is only counted while a document is open. The homepage QML mod writes
 * the open document's id/name to /run/qmarkable/current-doc (tmpfs, no flash
 * wear); an empty file means "no document open".
 *
 * Two measures are kept per local hour:
 *   active  — pen contact plus pauses between strokes of <= gap seconds
 *             (what the heatmap shows: "time spent writing")
 *   contact — raw pen-on-glass time
 *
 * Storage (written at most once per flush interval, only when changed):
 *   <datadir>/index.txt           "# qmarkable 1 tzoff=<min> today=<key>"
 *                                 then "<YYYY-MM-DD> <active secs>" per day
 *   <datadir>/days/<key>.txt      "a <24 ints>", "c <24 ints>",
 *                                 "d <id> <secs> <name>" per document
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define CONFIG_PATH "/home/root/.config/qmarkable/tracker.conf"
#define DEFAULT_DATADIR "/home/root/.local/share/qmarkable"
static const char *DOCFILE = "/run/qmarkable/current-doc";
#define MAX_DOCS 256
#define MAX_DAYS 8192
#define MAX_CONTACT_SEC 600.0 /* longer single contact = glitch, ignore */

enum { ACTIVE = 0, CONTACT = 1 };

struct doc {
    char id[64];
    char name[160];
    double secs; /* active seconds */
};

struct day {
    char key[11];
    double h[2][24];
    struct doc docs[MAX_DOCS];
    int ndocs;
};

struct idx {
    char key[11];
    long secs;
};

static struct {
    char datadir[256];
    char device[128];
    char tz[128];
    double gap;
    int flush_sec;
} cfg;

static struct day cur;
static struct idx days[MAX_DAYS];
static int ndays;
static int dirty;
static volatile sig_atomic_t stop;

/* current document, cached by mtime/size of DOCFILE */
static struct {
    int state; /* 0 none open, 1 open, 2 unknown (file missing) */
    char id[64];
    char name[160];
    struct timespec mtime;
    off_t size;
    int have;
} doc;

static double mono_now_raw(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
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

static void load_config(void)
{
    snprintf(cfg.datadir, sizeof cfg.datadir, "%s", DEFAULT_DATADIR);
    cfg.device[0] = 0;
    cfg.tz[0] = 0;
    cfg.gap = 15;
    cfg.flush_sec = 60;
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
        else if (!strcmp(k, "gap"))
            cfg.gap = atof(v);
        else if (!strcmp(k, "flush"))
            cfg.flush_sec = atoi(v);
        else if (!strcmp(k, "device"))
            snprintf(cfg.device, sizeof cfg.device, "%s", v);
        else if (!strcmp(k, "datadir"))
            snprintf(cfg.datadir, sizeof cfg.datadir, "%s", v);
    }
    fclose(f);
    if (cfg.gap < 0)
        cfg.gap = 0;
    if (cfg.flush_sec < 5)
        cfg.flush_sec = 5;
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

/* ---------- pen device discovery ---------- */

#define BITS_LONG (sizeof(long) * 8)
#define NLONGS(x) (((x) + BITS_LONG - 1) / BITS_LONG)
static int test_bit(int bit, const unsigned long *arr)
{
    return (arr[bit / BITS_LONG] >> (bit % BITS_LONG)) & 1;
}

static int is_pen(int fd)
{
    unsigned long keys[NLONGS(KEY_CNT)] = {0};
    unsigned long abs[NLONGS(ABS_CNT)] = {0};
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keys), keys) < 0)
        return 0;
    if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof abs), abs) < 0)
        return 0;
    return test_bit(BTN_TOOL_PEN, keys) && test_bit(BTN_TOUCH, keys) && test_bit(ABS_PRESSURE, abs);
}

static int open_pen(void)
{
    if (cfg.device[0]) {
        int fd = open(cfg.device, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0)
            logmsg("cannot open %s: %s", cfg.device, strerror(errno));
        return fd;
    }
    DIR *d = opendir("/dev/input");
    if (!d)
        return -1;
    struct dirent *e;
    int found = -1;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, "event", 5))
            continue;
        char p[300];
        snprintf(p, sizeof p, "/dev/input/%s", e->d_name);
        int fd = open(p, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0)
            continue;
        if (is_pen(fd)) {
            char name[128] = "?";
            ioctl(fd, EVIOCGNAME(sizeof name), name);
            logmsg("using %s (%s)", p, name);
            found = fd;
            break;
        }
        close(fd);
    }
    closedir(d);
    return found;
}

/* Only wake for packets containing BTN_TOUCH. EV_SYN/SYN_REPORT must stay
 * enabled: evdev wakes readers on SYN_REPORT and drops empty reports, so
 * reports that carried only filtered events never wake us. */
static void set_mask(int fd)
{
    unsigned long types[NLONGS(EV_CNT)] = {0};
    unsigned long syn[NLONGS(SYN_CNT)] = {0};
    unsigned long keys[NLONGS(KEY_CNT)] = {0};
    types[EV_SYN / BITS_LONG] |= 1UL << (EV_SYN % BITS_LONG);
    types[EV_KEY / BITS_LONG] |= 1UL << (EV_KEY % BITS_LONG);
    syn[SYN_REPORT / BITS_LONG] |= 1UL << (SYN_REPORT % BITS_LONG);
    syn[SYN_DROPPED / BITS_LONG] |= 1UL << (SYN_DROPPED % BITS_LONG);
    keys[BTN_TOUCH / BITS_LONG] |= 1UL << (BTN_TOUCH % BITS_LONG);

    struct input_mask m;
    int ok = 1;
    m.type = 0; /* type 0 = mask of event types */
    m.codes_size = sizeof types;
    m.codes_ptr = (uint64_t)(uintptr_t)types;
    ok &= ioctl(fd, EVIOCSMASK, &m) == 0;
    m.type = EV_SYN;
    m.codes_size = sizeof syn;
    m.codes_ptr = (uint64_t)(uintptr_t)syn;
    ok &= ioctl(fd, EVIOCSMASK, &m) == 0;
    m.type = EV_KEY;
    m.codes_size = sizeof keys;
    m.codes_ptr = (uint64_t)(uintptr_t)keys;
    ok &= ioctl(fd, EVIOCSMASK, &m) == 0;
    logmsg(ok ? "kernel event mask active (BTN_TOUCH only)" : "EVIOCSMASK failed (%s); filtering in userspace", strerror(errno));
}

/* ---------- current document ---------- */

static void refresh_doc(void)
{
    struct stat st;
    if (stat(DOCFILE, &st) < 0) {
        doc.state = 2;
        snprintf(doc.id, sizeof doc.id, "unknown");
        snprintf(doc.name, sizeof doc.name, "(unknown document)");
        doc.have = 0;
        return;
    }
    if (doc.have && st.st_mtim.tv_sec == doc.mtime.tv_sec && st.st_mtim.tv_nsec == doc.mtime.tv_nsec && st.st_size == doc.size)
        return;
    doc.have = 1;
    doc.mtime = st.st_mtim;
    doc.size = st.st_size;
    doc.state = 0;
    doc.id[0] = doc.name[0] = 0;
    FILE *f = fopen(DOCFILE, "r");
    if (!f)
        return;
    char line[512];
    if (fgets(line, sizeof line, f)) {
        trim(line);
        /* ids are uuids; keep only safe chars */
        size_t j = 0;
        for (size_t i = 0; line[i] && j < sizeof doc.id - 1; i++)
            if ((line[i] >= '0' && line[i] <= '9') || (line[i] >= 'a' && line[i] <= 'z') || (line[i] >= 'A' && line[i] <= 'Z') || line[i] == '-')
                doc.id[j++] = line[i];
        doc.id[j] = 0;
    }
    if (fgets(line, sizeof line, f)) {
        trim(line);
        snprintf(doc.name, sizeof doc.name, "%s", line);
    }
    fclose(f);
    if (doc.id[0]) {
        doc.state = 1;
        if (!doc.name[0])
            snprintf(doc.name, sizeof doc.name, "(untitled)");
    }
}

/* ---------- day storage ---------- */

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
        if ((line[0] == 'a' || line[0] == 'c') && line[1] == ' ') {
            int k = line[0] == 'a' ? ACTIVE : CONTACT;
            char *s = line + 2;
            for (int i = 0; i < 24; i++) {
                char *end;
                long v = strtol(s, &end, 10);
                if (end == s)
                    break;
                cur.h[k][i] = v;
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
        t += cur.h[ACTIVE][i];
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
    /* insert sorted */
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

/* ---------- timezone ----------
 * Follows the system timezone (/etc/localtime, which xochitl's date/time
 * settings change via systemd-timedated). musl caches the zone keyed on the
 * TZ string, so we point TZ at the symlink's resolved target and re-point it
 * whenever the target changes. A "tz=" config entry overrides this. */
static char tz_current[300];
static double tz_checked;

static void sync_timezone(int force)
{
    double now = mono_now_raw();
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
        char target[256];
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

static void write_index(void)
{
    static char buf[MAX_DAYS * 24 + 128];
    char tk[11];
    long tz;
    today_key(tk, &tz);
    int n = snprintf(buf, sizeof buf, "# qmarkable 1 tzoff=%ld today=%s gap=%g\n", tz, tk, cfg.gap);
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
    static char buf[MAX_DOCS * 256 + 1024];
    int n = 0;
    for (int k = 0; k < 2; k++) {
        n += snprintf(buf + n, sizeof buf - n, "%c", k == ACTIVE ? 'a' : 'c');
        for (int i = 0; i < 24; i++)
            n += snprintf(buf + n, sizeof buf - n, " %ld", lround(cur.h[k][i]));
        n += snprintf(buf + n, sizeof buf - n, "\n");
    }
    for (int i = 0; i < cur.ndocs; i++)
        n += snprintf(buf + n, sizeof buf - n, "d %s %ld %s\n", cur.docs[i].id, lround(cur.docs[i].secs), cur.docs[i].name);
    char p[400];
    day_path(p, sizeof p, cur.key);
    if (write_atomic(p, buf, (size_t)n) < 0)
        logmsg("write %s failed: %s", p, strerror(errno));
    index_set(cur.key, day_total());
}

static struct timespec last_flush;

static double mono_now(void) { return mono_now_raw(); }

static void flush_all(void)
{
    if (!dirty)
        return;
    sync_timezone(0);
    flush_day();
    write_index();
    dirty = 0;
    clock_gettime(CLOCK_MONOTONIC, &last_flush);
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

/* Add [start,end) (unix seconds, realtime) to hourly buckets, splitting at
 * local hour and day boundaries. */
static void add_span(double start, double end, int kind, const char *id, const char *name)
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
        cur.h[kind][tm.tm_hour] += seg;
        if (kind == ACTIVE) {
            struct doc *d = doc_slot(id, name);
            if (d)
                d->secs += seg;
        }
        dirty = 1;
        start += seg;
    }
}

/* ---------- pen state machine ---------- */

static struct {
    int down;
    double t_down;
    double active_start;
    double last_up;
    int counting;
    char id[64];
    char name[160];
    char last_id[64];
} pen;

static double ev_time(const struct input_event *ev)
{
    return ev->input_event_sec + ev->input_event_usec / 1e6;
}

static void on_touch(int value, double t)
{
    if (value && !pen.down) {
        pen.down = 1;
        pen.t_down = t;
        sync_timezone(0);
        refresh_doc();
        pen.counting = doc.state != 0;
        if (!pen.counting)
            return;
        snprintf(pen.id, sizeof pen.id, "%s", doc.id);
        snprintf(pen.name, sizeof pen.name, "%s", doc.name);
        double gapv = t - pen.last_up;
        if (pen.last_up > 0 && gapv >= 0 && gapv <= cfg.gap && !strcmp(pen.last_id, pen.id))
            pen.active_start = pen.last_up; /* short pause: still writing */
        else
            pen.active_start = t;
    } else if (!value && pen.down) {
        pen.down = 0;
        if (!pen.counting)
            return;
        double dur = t - pen.t_down;
        if (dur < 0 || dur > MAX_CONTACT_SEC) {
            pen.last_up = 0;
            return;
        }
        add_span(pen.active_start, t, ACTIVE, pen.id, pen.name);
        add_span(pen.t_down, t, CONTACT, pen.id, pen.name);
        pen.last_up = t;
        snprintf(pen.last_id, sizeof pen.last_id, "%s", pen.id);
    }
}

static void on_signal(int sig)
{
    (void)sig;
    stop = 1;
}

/* Test mode: "qmarkable-trackerd --replay" reads "<unix-time> <1|0>" lines
 * (pen down / up) from stdin. Use QMARKABLE_DATADIR / QMARKABLE_DOCFILE to
 * keep test data away from the real files. */
static int replay(void)
{
    char line[128];
    while (fgets(line, sizeof line, stdin)) {
        double t;
        int v;
        if (sscanf(line, "%lf %d", &t, &v) == 2)
            on_touch(v, t);
    }
    flush_all();
    return 0;
}

int main(int argc, char **argv)
{
    load_config();
    if (getenv("QMARKABLE_DATADIR"))
        snprintf(cfg.datadir, sizeof cfg.datadir, "%s", getenv("QMARKABLE_DATADIR"));
    if (getenv("QMARKABLE_DOCFILE"))
        DOCFILE = getenv("QMARKABLE_DOCFILE");
    int replay_mode = argc > 1 && !strcmp(argv[1], "--replay");
    sync_timezone(1);
    setpriority(PRIO_PROCESS, 0, 19);

    char p[400];
    snprintf(p, sizeof p, "%s/days", cfg.datadir);
    mkdirs(p);
    if (!replay_mode)
        mkdirs("/run/qmarkable");

    load_index();
    char tk[11];
    today_key(tk, NULL);
    load_day(tk);
    dirty = 1;
    flush_all(); /* refresh index header (tz offset / today) for the UI */
    if (replay_mode)
        return replay();

    int fd = open_pen();
    if (fd < 0) {
        logmsg("no pen input device found");
        return 1;
    }
    set_mask(fd);

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal; /* no SA_RESTART: poll returns EINTR */
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);

    struct pollfd pfd = {.fd = fd, .events = POLLIN};
    struct input_event evs[64];
    clock_gettime(CLOCK_MONOTONIC, &last_flush);

    while (!stop) {
        int timeout = -1; /* sleep indefinitely when nothing is pending */
        if (dirty) {
            double since = mono_now() - (last_flush.tv_sec + last_flush.tv_nsec / 1e9);
            double left = cfg.flush_sec - since;
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
            if (!pen.down)
                flush_all();
            else
                clock_gettime(CLOCK_MONOTONIC, &last_flush); /* retry after stroke */
            continue;
        }
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            logmsg("input device went away");
            break;
        }
        for (;;) {
            ssize_t n = read(fd, evs, sizeof evs);
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                if (errno != EAGAIN)
                    logmsg("read: %s", strerror(errno));
                break;
            }
            int cnt = (int)(n / (ssize_t)sizeof evs[0]);
            for (int i = 0; i < cnt; i++) {
                if (evs[i].type == EV_KEY && evs[i].code == BTN_TOUCH)
                    on_touch(evs[i].value, ev_time(&evs[i]));
                else if (evs[i].type == EV_SYN && evs[i].code == SYN_DROPPED)
                    pen.down = 0; /* lost sync: drop the in-flight stroke */
            }
            if (n < (ssize_t)sizeof evs)
                break;
        }
    }
    flush_all();
    logmsg("exiting");
    return 0;
}
