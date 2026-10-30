#include <cpu.h>
#include <heap.h>
#include <http.h>
#include <net.h>
#include <string.h>
#include <task.h>

/*
 * urls, and an http/1.1 client that runs on top of tcp.c. this release only
 * speaks plain http, since https needs tls, which came later.
 *
 * every request asks the server to close the connection when it's done
 * (connection: close), so the answer is simply everything that arrives until
 * then, or until content-length bytes of body have come in. the body might
 * arrive in chunks (transfer-encoding: chunked), which get joined back
 * together.
 */

/* ---- urls ---- */

static int default_port(const char *scheme)
{
    return !strcmp(scheme, "https") ? 443 : 80;
}

static char lower(char c)
{
    return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

/* a scheme is letters (and + - .) followed by a colon, before any slash. */
static int scheme_length(const char *s)
{
    int i = 0;
    while ((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z') ||
           (i > 0 && ((s[i] >= '0' && s[i] <= '9') || s[i] == '+' || s[i] == '-' || s[i] == '.')))
        i++;
    return i > 0 && s[i] == ':' ? i : 0;
}

/*
 * splits a url into its scheme, host, port and path. about: urls are just the
 * scheme and whatever comes after it. the scheme and host get lowercased, the
 * port falls back to the scheme's usual one, and the path always starts with
 * a slash.
 */
bool url_parse(const char *s, struct url *u)
{
    memset(u, 0, sizeof(*u));
    int sl = scheme_length(s);
    if (!sl || sl >= (int)sizeof(u->scheme))
        return false;
    for (int i = 0; i < sl; i++)
        u->scheme[i] = lower(s[i]);
    s += sl + 1;

    if (!strcmp(u->scheme, "about")) {
        strlcpy(u->path, s, sizeof(u->path));
        return true;
    }
    if (s[0] != '/' || s[1] != '/')
        return false;
    s += 2;

    int h = 0;
    while (*s && *s != '/' && *s != ':' && *s != '?' && *s != '#' && h < (int)sizeof(u->host) - 1)
        u->host[h++] = lower(*s++);
    u->port = default_port(u->scheme);
    if (*s == ':') {
        s++;
        int port = 0;
        while (*s >= '0' && *s <= '9')
            port = port * 10 + (*s++ - '0');
        if (port <= 0 || port > 65535)
            return false;
        u->port = port;
    }
    if (*s == '/')
        strlcpy(u->path, s, sizeof(u->path));
    else {
        u->path[0] = '/';
        strlcpy(u->path + 1, *s == '?' ? s : "", sizeof(u->path) - 1);
    }

    /*
     * the fragment (the #... part) is just for the browser, and never gets
     * sent to the server
     */
    char *hash = strchr(u->path, '#');
    if (hash)
        *hash = '\0';
    return strcmp(u->scheme, "file") == 0 || u->host[0];
}

static void format_url(const struct url *u, char *out, int size)
{
    if (!strcmp(u->scheme, "about")) {
        ksnprintf(out, size, "about:%s", u->path);
    } else if (u->port != default_port(u->scheme) && strcmp(u->scheme, "file")) {
        ksnprintf(out, size, "%s://%s:%d%s", u->scheme, u->host, u->port, u->path);
    } else {
        ksnprintf(out, size, "%s://%s%s", u->scheme, u->host, u->path);
    }
}

/* takes the "." and ".." parts out of a path, in place. */
static void normalize(char *path)
{
    char tail[URL_MAX] = "";
    char *query = strchr(path, '?');
    if (query) {
        strlcpy(tail, query, sizeof(tail));
        *query = '\0';
    }

    char out[URL_MAX];
    int len = 0;
    bool ends_in_folder = false;
    for (const char *p = path; *p;) {
        if (*p == '/') {
            p++;
            continue;
        }
        const char *end = p;
        while (*end && *end != '/')
            end++;
        int seg = end - p;
        ends_in_folder = *end == '/';
        if (seg == 1 && p[0] == '.') {
            ends_in_folder = true;
        } else if (seg == 2 && p[0] == '.' && p[1] == '.') {
            while (len > 0 && out[len - 1] != '/')
                len--;
            if (len > 0)
                len--;
            ends_in_folder = true;
        } else if (len + seg + 2 < URL_MAX) {
            out[len++] = '/';
            memcpy(out + len, p, seg);
            len += seg;
        }
        p = end;
    }
    if (len == 0 || ends_in_folder)
        out[len++] = '/';
    out[len] = '\0';
    ksnprintf(path, URL_MAX, "%s%s", out, tail);
}

/*
 * turns a link on a page into a full url, using the page's address as the
 * base. a link with its own scheme stays as it is, "//host/..." borrows the
 * page's scheme, "#..." swaps the fragment, "/..." starts from the root of
 * the same site, and anything else is relative to the page's folder, with any
 * "." and ".." tidied up afterwards.
 */
void url_resolve(const char *base, const char *link, char *out, int size)
{
    while (*link == ' ' || *link == '\n' || *link == '\t')
        link++;
    if (!*link) {
        strlcpy(out, base, size);
        return;
    }
    if (scheme_length(link)) {
        strlcpy(out, link, size);
        return;
    }

    struct url u;
    if (!url_parse(base, &u)) {
        strlcpy(out, link, size);
        return;
    }
    if (link[0] == '/' && link[1] == '/') {
        ksnprintf(out, size, "%s:%s", u.scheme, link);
        return;
    }

    if (link[0] == '#') {
        char *hash = strchr(u.path, '#');
        if (hash)
            *hash = '\0';
        strlcpy(u.path + strlen(u.path), link, sizeof(u.path) - strlen(u.path));
    } else if (link[0] == '?') {
        char *q = strchr(u.path, '?');
        if (q)
            *q = '\0';
        strlcpy(u.path + strlen(u.path), link, sizeof(u.path) - strlen(u.path));
    } else if (link[0] == '/') {
        strlcpy(u.path, link, sizeof(u.path));
        normalize(u.path);
    } else {
        char *q = strchr(u.path, '?');
        if (q)
            *q = '\0';
        char *slash = strrchr(u.path, '/');
        if (slash)
            slash[1] = '\0';
        else
            strlcpy(u.path, "/", sizeof(u.path));
        strlcpy(u.path + strlen(u.path), link, sizeof(u.path) - strlen(u.path));
        normalize(u.path);
    }
    format_url(&u, out, size);
}

/* ---- http ---- */

static bool starts_with_ci(const char *s, const char *prefix)
{
    for (; *prefix; s++, prefix++)
        if (lower(*s) != lower(*prefix))
            return false;
    return true;
}

/*
 * finds a header's value in the block of headers, and copies it out without
 * the spaces in front.
 */
static bool header(const char *headers, const char *name, char *out, int size)
{
    int nlen = strlen(name);
    for (const char *line = headers; line && *line;) {
        if (starts_with_ci(line, name) && line[nlen] == ':') {
            const char *v = line + nlen + 1;
            while (*v == ' ' || *v == '\t')
                v++;
            int i = 0;
            while (v[i] && v[i] != '\r' && v[i] != '\n' && i < size - 1) {
                out[i] = v[i];
                i++;
            }
            out[i] = '\0';
            return true;
        }
        line = strchr(line, '\n');
        if (line)
            line++;
    }
    return false;
}

static int parse_hex(const char **p)
{
    int v = 0;
    for (;; (*p)++) {
        char c = lower(**p);
        if (c >= '0' && c <= '9') v = v * 16 + c - '0';
        else if (c >= 'a' && c <= 'f') v = v * 16 + c - 'a' + 10;
        else break;
    }
    return v;
}

/*
 * joins a chunked body back together in place: a size line, the data, a line
 * break, and so on until a chunk of size 0.
 */
static int unchunk(char *body, int len)
{
    const char *p = body, *end = body + len;
    int out = 0;
    while (p < end) {
        int n = parse_hex(&p);
        while (p < end && *p != '\n')
            p++;
        p++;
        if (n <= 0 || p + n > end) {
            if (n > 0 && p < end) {
                memmove(body + out, p, end - p);
                out += end - p;
            }
            break;
        }
        memmove(body + out, p, n);
        out += n;
        p += n;
        while (p < end && (*p == '\r' || *p == '\n'))
            p++;
    }
    return out;
}

static void say(void (*progress)(const char *), const char *fmt, const char *arg)
{
    if (!progress)
        return;
    char msg[160];
    ksnprintf(msg, sizeof(msg), fmt, arg);
    progress(msg);
}

static int fail(struct http_result *res, int err, const char *msg)
{
    strlcpy(res->error, msg ? msg : net_error(err), sizeof(res->error));
    return err;
}

/*
 * a connection is a tcp socket. reads give back how many bytes they read, 0
 * if nothing has arrived yet, or a negative number once the server has
 * finished.
 */
struct conn {
    int sock;
};

static int conn_write(struct conn *c, const void *data, int len)
{
    return tcp_write(c->sock, data, len);
}

static int conn_read(struct conn *c, void *buf, int max)
{
    return tcp_read(c->sock, buf, max);
}

static void conn_close(struct conn *c)
{
    tcp_close(c->sock);
}

/* one request and its whole answer, without following any redirects. */
static int fetch_once(const struct url *u, struct http_result *res, char *location, void (*progress)(const char *))
{
    say(progress, "Looking up %s...", u->host);
    uint32_t ip;
    int err = net_resolve(u->host, &ip);
    if (err < 0) {
        char msg[160];
        ksnprintf(msg, sizeof(msg), "Couldn't find %s: %s.", u->host, net_error(err));
        return fail(res, err, msg);
    }

    struct conn c = { -1 };
    say(progress, "Connecting to %s...", u->host);
    c.sock = tcp_connect(ip, u->port, 6 * timer_hz());
    if (c.sock < 0) {
        char msg[160];
        ksnprintf(msg, sizeof(msg), "Couldn't connect to %s: %s.", u->host, net_error(c.sock));
        return fail(res, c.sock, msg);
    }

    char req[URL_MAX + 256];
    int rlen = ksnprintf(req, sizeof(req),
                         "GET %s HTTP/1.1\r\n"
                         "Host: %s\r\n"
                         "User-Agent: CraftOS (Craft Release 2)\r\n"
                         "Accept: */*\r\n"
                         "Accept-Encoding: identity\r\n"
                         "Connection: close\r\n\r\n",
                         u->path, u->host);
    err = conn_write(&c, req, rlen);
    if (err < 0) {
        conn_close(&c);
        return fail(res, err, 0);
    }

    say(progress, "Waiting for %s...", u->host);
    int cap = 32768, len = 0, header_end = -1, content_length = -1;
    char *buf = kmalloc(cap + 1);
    if (!buf) {
        conn_close(&c);
        return fail(res, NET_ERR_FULL, "Out of memory.");
    }
    uint32_t last_data = timer_ticks();
    for (;;) {
        if (len + 4096 > cap) {
            char *bigger = krealloc(buf, cap * 2 + 1);
            if (!bigger)
                break;
            buf = bigger;
            cap *= 2;
        }
        int n = conn_read(&c, buf + len, cap - len);
        if (n > 0) {
            len += n;
            last_data = timer_ticks();
            if (header_end < 0) {
                buf[len] = '\0';
                char *end = strstr(buf, "\r\n\r\n");
                if (end) {
                    header_end = end - buf + 4;
                    char value[32];
                    buf[header_end - 2] = '\0';
                    if (header(buf, "Content-Length", value, sizeof(value))) {
                        content_length = 0;
                        for (const char *p = value; *p >= '0' && *p <= '9'; p++)
                            content_length = content_length * 10 + (*p - '0');
                    }
                    buf[header_end - 2] = '\r';
                }
            }
            if (progress) {
                char msg[64];
                ksnprintf(msg, sizeof(msg), "Receiving... %d KB", len / 1024);
                progress(msg);
            }
            if (header_end >= 0 && content_length >= 0 && len - header_end >= content_length)
                break;
            continue;
        }
        if (n < 0)
            break;
        if (timer_ticks() - last_data > 15 * timer_hz())
            break;
        task_sleep(1);
    }
    conn_close(&c);

    if (header_end < 0) {
        kfree(buf);
        return fail(res, NET_ERR_CLOSED, len ? "The server's answer didn't make sense." : "The server closed the connection without answering.");
    }

    /* the status line, like "http/1.1 200 ok" */
    buf[header_end - 2] = '\0';
    const char *sp = strchr(buf, ' ');
    res->status = 0;
    for (sp = sp ? sp + 1 : buf; *sp >= '0' && *sp <= '9'; sp++)
        res->status = res->status * 10 + (*sp - '0');

    char value[URL_MAX];
    res->content_type[0] = '\0';
    header(buf, "Content-Type", res->content_type, sizeof(res->content_type));
    location[0] = '\0';
    header(buf, "Location", location, URL_MAX);
    bool chunked = header(buf, "Transfer-Encoding", value, sizeof(value)) && starts_with_ci(value, "chunked");

    int body_len = len - header_end;
    if (content_length >= 0 && body_len > content_length)
        body_len = content_length;
    memmove(buf, buf + header_end, body_len);
    if (chunked)
        body_len = unchunk(buf, body_len);
    buf[body_len] = '\0';
    res->body = buf;
    res->len = body_len;
    return 0;
}

/*
 * fetches a url, following up to five redirects. each one gets fetched with
 * fetch_once, and if the answer is a redirect with somewhere to go, the new
 * address is worked out from the old one and we go round again.
 */
int http_get(const char *url, struct http_result *res, void (*progress)(const char *msg))
{
    memset(res, 0, sizeof(*res));
    char current[URL_MAX];
    strlcpy(current, url, sizeof(current));

    for (int redirects = 0;; redirects++) {
        struct url u;
        if (!url_parse(current, &u))
            return fail(res, NET_ERR_BAD_URL, "That address doesn't look right.");
        if (!strcmp(u.scheme, "https"))
            return fail(res, NET_ERR_BAD_URL, "This release has no HTTPS: only http:// addresses can be fetched.");
        if (strcmp(u.scheme, "http"))
            return fail(res, NET_ERR_BAD_URL, "Only http:// addresses can be fetched.");

        char location[URL_MAX];
        int err = fetch_once(&u, res, location, progress);
        if (err < 0)
            return err;
        strlcpy(res->final_url, current, sizeof(res->final_url));

        bool redirect = res->status == 301 || res->status == 302 || res->status == 303 ||
                        res->status == 307 || res->status == 308;
        if (!redirect || !location[0])
            return 0;
        if (redirects >= 5) {
            http_free(res);
            return fail(res, NET_ERR_BAD_URL, "Too many redirects.");
        }
        char next[URL_MAX];
        url_resolve(current, location, next, sizeof(next));
        http_free(res);
        strlcpy(current, next, sizeof(current));
        say(progress, "Redirected to %s", current);
    }
}

void http_free(struct http_result *res)
{
    kfree(res->body);
    res->body = 0;
    res->len = 0;
}
