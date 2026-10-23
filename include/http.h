#pragma once
#include <stdbool.h>

/*
 * urls and http.
 *
 * url_parse splits "http://host:port/path" up into its parts. url_resolve
 * turns a link on a page into a full url, using the page's own address as the
 * starting point. links can be absolute, start from the root like "/a", be
 * relative like "b.html" or "../c", or just be "#top". both of them
 * understand http:, https:, file: and about:.
 */

#define URL_MAX 512

struct url {
    char scheme[8];
    char host[128];
    int port;
    char path[URL_MAX];     /* everything from the first / onwards, the query included */
};

bool url_parse(const char *s, struct url *u);
void url_resolve(const char *base, const char *link, char *out, int size);

/*
 * http_get fetches a url over http/1.1 (plain http only), following up to
 * five redirects. if it works, it returns 0 and fills in the result. body
 * comes from kmalloc and belongs to whoever called (free it with http_free).
 * if it doesn't work, it returns an error from net.h and puts a sentence
 * saying what went wrong in error. progress, if you pass one, gets told
 * what's going on ("connecting...").
 */
struct http_result {
    int status;
    char *body;
    int len;
    char content_type[64];
    char final_url[URL_MAX];
    char error[192];
};

int http_get(const char *url, struct http_result *res, void (*progress)(const char *msg));
void http_free(struct http_result *res);
