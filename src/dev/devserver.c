/*
 * cinder dev — native HTTP/WebSocket dev server
 *
 * Architecture:
 *   1. Spawn esbuild --bundle --watch → writes .cinder/bundle.js
 *   2. Optionally spawn tailwindcss --watch → writes .cinder/style.css
 *   3. HTTP server (Winsock2/BSD) serves static files + .cinder artifacts
 *   4. ReadDirectoryChangesW watches .cinder/ for rebuilt artifacts
 *   5. WebSocket broadcasts {"type":"reload"} to all browser clients
 *
 * Result: server ready in < 5ms, JS rebuilds in 10-50ms.
 */

#include "devserver.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <bcrypt.h>
#  pragma comment(lib, "ws2_32.lib")
#  pragma comment(lib, "bcrypt.lib")
typedef SOCKET sock_t;
#  define SOCK_INVALID  INVALID_SOCKET
#  define sock_close(s) closesocket(s)
#  define sock_error()  WSAGetLastError()
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <unistd.h>
#  include <fcntl.h>
#  include <errno.h>
#  include <signal.h>
#  include <sys/wait.h>
#  include <sys/select.h>
#  include <sys/stat.h>
#  include <sys/types.h>
#  include <dirent.h>
#  include <pthread.h>
#  include <sys/inotify.h>
#  include <poll.h>
typedef int sock_t;
typedef pid_t HANDLE;
#  define SOCK_INVALID  (-1)
#  define sock_close(s) close(s)
#  define sock_error()  errno
#endif

/* ── Constants ───────────────────────────────────────────────────────────────── */

#define CINDER_DIR     ".cinder"
#define BUNDLE_FILE    ".cinder/bundle.js"
#define STYLE_FILE     ".cinder/style.css"
#define MAX_WS_CLIENTS 64
#define RECV_BUF_SIZE  8192
#define WS_GUID        "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
#define TW_CDN         "https://cdn.tailwindcss.com"

static int g_use_tw_cdn = 0; /* inject Tailwind CDN if no local CLI */

/* HMR client script injected into index.html */
static const char *HMR_SCRIPT =
    "<script>\n"
    "(function(){\n"
    "  var ws = new WebSocket('ws://'+location.host+'/_cinder_ws');\n"
    "  ws.onmessage = function(e){\n"
    "    var m = JSON.parse(e.data);\n"
    "    if(m.type==='reload') location.reload();\n"
    "  };\n"
    "  ws.onerror = function(){\n"
    "    setTimeout(function(){\n"
    "      var r=new XMLHttpRequest();r.open('GET','/_cinder_ping');\n"
    "      r.onload=function(){location.reload();};\n"
    "      r.send();\n"
    "    }, 500);\n"
    "  };\n"
    "}());\n"
    "</script>\n";

/* ── Global state ────────────────────────────────────────────────────────────── */

static sock_t g_ws_clients[MAX_WS_CLIENTS];
static int    g_ws_count = 0;
static int    g_server_running = 1;
static HANDLE g_esbuild_proc = NULL;
static HANDLE g_tw_proc = NULL;
static char   g_root[4096];
static char   g_public_dir[4096];
static char   g_entry[512];

#ifdef _WIN32
static CRITICAL_SECTION g_ws_lock;
#define WS_LOCK()   EnterCriticalSection(&g_ws_lock)
#define WS_UNLOCK() LeaveCriticalSection(&g_ws_lock)
#else
static pthread_mutex_t g_ws_lock = PTHREAD_MUTEX_INITIALIZER;
#define WS_LOCK()   pthread_mutex_lock(&g_ws_lock)
#define WS_UNLOCK() pthread_mutex_unlock(&g_ws_lock)
#endif

/* ── SHA-1 + Base64 for WebSocket handshake ─────────────────────────────────── */

static void sha1_bcrypt(const unsigned char *data, size_t len,
                        unsigned char out[20]) {
#ifdef _WIN32
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_HASH_HANDLE hash = NULL;
    BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA1_ALGORITHM, NULL, 0);
    BCryptCreateHash(alg, &hash, NULL, 0, NULL, 0, 0);
    BCryptHashData(hash, (PUCHAR)data, (ULONG)len, 0);
    BCryptFinishHash(hash, out, 20, 0);
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);
#else
    /* Portable SHA-1 (RFC 3174) */
    uint32_t h[5] = {0x67452301,0xEFCDAB89,0x98BADCFE,0x10325476,0xC3D2E1F0};
    uint64_t bit_len = (uint64_t)len * 8;
    size_t pad_len = ((len + 8) / 64 + 1) * 64;
    unsigned char *msg = (unsigned char*)calloc(1, pad_len);
    memcpy(msg, data, len);
    msg[len] = 0x80;
    for (int i = 0; i < 8; i++)
        msg[pad_len - 1 - i] = (unsigned char)(bit_len >> (i * 8));
    for (size_t blk = 0; blk < pad_len; blk += 64) {
        uint32_t w[80]; int i;
        for (i = 0; i < 16; i++) {
            w[i] = ((uint32_t)msg[blk+i*4]<<24)|((uint32_t)msg[blk+i*4+1]<<16)|
                   ((uint32_t)msg[blk+i*4+2]<<8)|(uint32_t)msg[blk+i*4+3];
        }
        for (i=16;i<80;i++){uint32_t v=w[i-3]^w[i-8]^w[i-14]^w[i-16];w[i]=(v<<1)|(v>>31);}
        uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4];
        for (i=0;i<80;i++){
            uint32_t f,k;
            if(i<20){f=(b&c)|(~b&d);k=0x5A827999;}
            else if(i<40){f=b^c^d;k=0x6ED9EBA1;}
            else if(i<60){f=(b&c)|(b&d)|(c&d);k=0x8F1BBCDC;}
            else{f=b^c^d;k=0xCA62C1D6;}
            uint32_t tmp=((a<<5)|(a>>27))+f+e+k+w[i];
            e=d;d=c;c=(b<<30)|(b>>2);b=a;a=tmp;
        }
        h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d;h[4]+=e;
    }
    free(msg);
    for (int i=0;i<5;i++){out[i*4]=(h[i]>>24)&0xFF;out[i*4+1]=(h[i]>>16)&0xFF;
        out[i*4+2]=(h[i]>>8)&0xFF;out[i*4+3]=h[i]&0xFF;}
#endif
}

static const char b64chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void base64_encode(const unsigned char *in, size_t len, char *out) {
    size_t i = 0, j = 0;
    for (; i + 2 < len; i += 3) {
        out[j++] = b64chars[(in[i]>>2)&0x3F];
        out[j++] = b64chars[((in[i]&3)<<4)|(in[i+1]>>4)];
        out[j++] = b64chars[((in[i+1]&0xF)<<2)|(in[i+2]>>6)];
        out[j++] = b64chars[in[i+2]&0x3F];
    }
    if (i < len) {
        out[j++] = b64chars[(in[i]>>2)&0x3F];
        if (i+1 < len) {
            out[j++] = b64chars[((in[i]&3)<<4)|(in[i+1]>>4)];
            out[j++] = b64chars[((in[i+1]&0xF)<<2)];
        } else {
            out[j++] = b64chars[(in[i]&3)<<4];
            out[j++] = '=';
        }
        out[j++] = '=';
    }
    out[j] = '\0';
}

/* ── WebSocket helpers ───────────────────────────────────────────────────────── */

static void ws_handshake(sock_t s, const char *key) {
    char accept_src[128];
    snprintf(accept_src, sizeof(accept_src), "%s%s", key, WS_GUID);

    unsigned char sha[20];
    sha1_bcrypt((const unsigned char*)accept_src, strlen(accept_src), sha);

    char accept_b64[48];
    base64_encode(sha, 20, accept_b64);

    char resp[512];
    int resp_len = snprintf(resp, sizeof(resp),
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: %s\r\n\r\n",
        accept_b64);
    send(s, resp, resp_len, 0);
}

static void ws_send_text(sock_t s, const char *msg) {
    size_t len = strlen(msg);
    unsigned char frame[16];
    int hdr_len;
    frame[0] = 0x81; /* FIN + text opcode */
    if (len < 126) {
        frame[1] = (unsigned char)len;
        hdr_len = 2;
    } else {
        frame[1] = 126;
        frame[2] = (unsigned char)(len >> 8);
        frame[3] = (unsigned char)(len & 0xFF);
        hdr_len = 4;
    }
    send(s, (char*)frame, hdr_len, 0);
    send(s, msg, (int)len, 0);
}

static void ws_broadcast(const char *msg) {
    WS_LOCK();
    for (int i = 0; i < g_ws_count; ) {
        int ok = 1;
        /* send + check for disconnect */
        size_t len = strlen(msg);
        unsigned char frame[4];
        frame[0] = 0x81;
        frame[1] = (unsigned char)(len < 126 ? len : 126);
        if (len >= 126) { frame[2]=(unsigned char)(len>>8); frame[3]=(unsigned char)(len&0xFF); }
        int hlen = len < 126 ? 2 : 4;
        if (send(g_ws_clients[i], (char*)frame, hlen, 0) < 0) ok = 0;
        if (ok && send(g_ws_clients[i], msg, (int)len, 0) < 0) ok = 0;
        if (!ok) {
            sock_close(g_ws_clients[i]);
            g_ws_clients[i] = g_ws_clients[--g_ws_count];
        } else {
            i++;
        }
    }
    WS_UNLOCK();
}

/* ── MIME type ───────────────────────────────────────────────────────────────── */

static const char *mime_for(const char *path) {
    const char *ext = strrchr(path, '.');
    if (!ext) return "application/octet-stream";
    if (!strcmp(ext, ".html") || !strcmp(ext, ".htm")) return "text/html";
    if (!strcmp(ext, ".js")   || !strcmp(ext, ".mjs")) return "application/javascript";
    if (!strcmp(ext, ".ts")   || !strcmp(ext, ".tsx") || !strcmp(ext, ".jsx"))
        return "application/javascript";
    if (!strcmp(ext, ".css"))  return "text/css";
    if (!strcmp(ext, ".json")) return "application/json";
    if (!strcmp(ext, ".svg"))  return "image/svg+xml";
    if (!strcmp(ext, ".png"))  return "image/png";
    if (!strcmp(ext, ".jpg")   || !strcmp(ext, ".jpeg")) return "image/jpeg";
    if (!strcmp(ext, ".ico"))  return "image/x-icon";
    if (!strcmp(ext, ".woff")) return "font/woff";
    if (!strcmp(ext, ".woff2"))return "font/woff2";
    if (!strcmp(ext, ".ttf"))  return "font/ttf";
    if (!strcmp(ext, ".map"))  return "application/json";
    return "application/octet-stream";
}

/* ── File helpers ────────────────────────────────────────────────────────────── */

static char *read_file(const char *path, long *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    fread(buf, 1, (size_t)sz, f);
    buf[sz] = '\0';
    fclose(f);
    if (out_size) *out_size = sz;
    return buf;
}

static int file_exists(const char *path) {
#ifdef _WIN32
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
#else
    struct stat st;
    return stat(path, &st) == 0;
#endif
}

static void mkdirp_single(const char *dir) {
#ifdef _WIN32
    CreateDirectoryA(dir, NULL);
#else
    mkdir(dir, 0755);
#endif
}

/* ── HTTP response helpers ───────────────────────────────────────────────────── */

static void send_response(sock_t s, int code, const char *status,
                          const char *mime, const char *body, long body_len) {
    char hdr[512];
    int hdr_len = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %ld\r\n"
        "Cache-Control: no-cache\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Connection: close\r\n\r\n",
        code, status, mime, body_len);
    send(s, hdr, hdr_len, 0);
    if (body && body_len > 0) {
        /* Loop to handle partial sends (important for large bodies) */
        const char *p = body;
        long remaining = body_len;
        while (remaining > 0) {
            int chunk = (remaining > 65536) ? 65536 : (int)remaining;
            int sent = send(s, p, chunk, 0);
            if (sent <= 0) break;
            p += sent;
            remaining -= sent;
        }
    }
}

/* Stream a file directly to socket without loading it all into memory */
static void send_file(sock_t s, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        const char *msg = "File not found";
        send_response(s, 404, "Not Found", "text/plain", msg, (long)strlen(msg));
        return;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    char hdr[512];
    int hdr_len = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %ld\r\n"
        "Cache-Control: no-cache\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Connection: close\r\n\r\n",
        mime_for(path), sz);
    send(s, hdr, hdr_len, 0);

    char chunk[65536];
    size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) {
        const char *p = chunk;
        size_t rem = n;
        while (rem > 0) {
            int sent = send(s, p, (int)rem, 0);
            if (sent <= 0) { fclose(f); return; }
            p += sent; rem -= (size_t)sent;
        }
    }
    fclose(f);
}

/* Inject HMR script + cinder bundle/CSS references into index.html */
/* Single-pass HTML patching: inject bundle, CSS, Tailwind CDN, and HMR script */
static void serve_html(sock_t s, const char *html_path) {
    long sz;
    char *html = read_file(html_path, &sz);
    if (!html) {
        const char *msg = "index.html not found";
        send_response(s, 404, "Not Found", "text/plain", msg, (long)strlen(msg));
        return;
    }

    const char *css_link  = "  <link rel=\"stylesheet\" href=\"/@cinder/style.css\">\n";
    const char *tw_tag    = "  <script src=\"" TW_CDN "\"></script>\n";
    const char *bundle_src = "src=\"/@cinder/bundle.js\"";
    int inject_css = file_exists(STYLE_FILE);

    size_t extra = 256 + strlen(HMR_SCRIPT)
                       + (inject_css ? strlen(css_link) : 0)
                       + (g_use_tw_cdn ? strlen(tw_tag) : 0);
    char *out = (char *)malloc((size_t)sz + extra);
    if (!out) { free(html); return; }

    char *dst = out;
    const char *src = html;
    const char *end = html + sz;

    while (src < end) {
        /* Before </head>: inject CSS link and/or Tailwind CDN */
        if (strncmp(src, "</head>", 7) == 0) {
            if (inject_css) {
                memcpy(dst, css_link, strlen(css_link));
                dst += strlen(css_link);
            }
            if (g_use_tw_cdn) {
                memcpy(dst, tw_tag, strlen(tw_tag));
                dst += strlen(tw_tag);
            }
            memcpy(dst, "</head>", 7); dst += 7; src += 7;
            continue;
        }

        /* Replace src="/src/..." with bundle path */
        if (strncmp(src, "src=\"/src/", 10) == 0 || strncmp(src, "src='/src/", 10) == 0) {
            char quote = src[4];
            const char *attr_end = strchr(src + 5, quote);
            if (attr_end) {
                memcpy(dst, bundle_src, strlen(bundle_src));
                dst += strlen(bundle_src);
                src = attr_end + 1;
                continue;
            }
        }

        /* Before </body>: inject HMR WebSocket client */
        if (strncmp(src, "</body>", 7) == 0) {
            memcpy(dst, HMR_SCRIPT, strlen(HMR_SCRIPT));
            dst += strlen(HMR_SCRIPT);
            memcpy(dst, "</body>", 7); dst += 7; src += 7;
            continue;
        }

        *dst++ = *src++;
    }
    *dst = '\0';

    send_response(s, 200, "OK", "text/html", out, (long)(dst - out));
    free(out);
    free(html);
}

/* ── HTTP request parser + dispatcher ───────────────────────────────────────── */

static void handle_connection(sock_t s) {
    char buf[RECV_BUF_SIZE];
    int received = recv(s, buf, sizeof(buf) - 1, 0);
    if (received <= 0) { sock_close(s); return; }
    buf[received] = '\0';

    /* Parse first line: METHOD /path HTTP/1.x */
    char method[16], path[2048];
    sscanf(buf, "%15s %2047s", method, path);

    /* Decode simple %20 etc. */
    /* (skip full URL decode for brevity — paths are usually clean) */

    /* WebSocket upgrade */
    if (strstr(buf, "Upgrade: websocket") || strstr(buf, "Upgrade: WebSocket")) {
        char ws_key[128] = {0};
        const char *key_hdr = strstr(buf, "Sec-WebSocket-Key: ");
        if (key_hdr) {
            key_hdr += 19;
            const char *eol = strstr(key_hdr, "\r\n");
            size_t klen = eol ? (size_t)(eol - key_hdr) : strlen(key_hdr);
            if (klen >= sizeof(ws_key)) klen = sizeof(ws_key) - 1;
            memcpy(ws_key, key_hdr, klen);
            ws_key[klen] = '\0';
        }
        ws_handshake(s, ws_key);
        WS_LOCK();
        if (g_ws_count < MAX_WS_CLIENTS) {
            g_ws_clients[g_ws_count++] = s;
        } else {
            sock_close(s);
        }
        WS_UNLOCK();
        return; /* keep socket open — caller must not close it */
    }

    /* Ping endpoint (used by HMR reconnect) */
    if (strcmp(path, "/_cinder_ping") == 0) {
        send_response(s, 200, "OK", "text/plain", "ok", 2);
        sock_close(s);
        return;
    }

    /* Cinder virtual files */
    if (strcmp(path, "/@cinder/bundle.js") == 0) {
        if (file_exists(BUNDLE_FILE)) send_file(s, BUNDLE_FILE);
        else {
            const char *msg = "/* bundle not ready yet */";
            send_response(s, 200, "OK", "application/javascript",
                          msg, (long)strlen(msg));
        }
        sock_close(s);
        return;
    }
    if (strcmp(path, "/@cinder/style.css") == 0) {
        if (file_exists(STYLE_FILE)) send_file(s, STYLE_FILE);
        else {
            send_response(s, 200, "OK", "text/css", "", 0);
        }
        sock_close(s);
        return;
    }

    /* Root → serve index.html */
    if (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0) {
        char html_path[4096];
        snprintf(html_path, sizeof(html_path), "%s/index.html", g_public_dir);
        if (!file_exists(html_path))
            snprintf(html_path, sizeof(html_path), "%s/index.html", g_root);
        serve_html(s, html_path);
        sock_close(s);
        return;
    }

    /* Try public dir first, then root */
    char fpath[4096];
    snprintf(fpath, sizeof(fpath), "%s%s", g_public_dir, path);
    if (!file_exists(fpath))
        snprintf(fpath, sizeof(fpath), "%s%s", g_root, path);

    if (file_exists(fpath)) {
        send_file(s, fpath);
    } else {
        /* SPA fallback → index.html */
        char html_path[4096];
        snprintf(html_path, sizeof(html_path), "%s/index.html", g_public_dir);
        if (!file_exists(html_path))
            snprintf(html_path, sizeof(html_path), "%s/index.html", g_root);
        serve_html(s, html_path);
    }
    sock_close(s);
}

/* Thread entry point: call handle_connection then close the socket */
#ifdef _WIN32
static DWORD WINAPI handle_connection_thread(LPVOID arg) {
    sock_t s = (sock_t)(uintptr_t)arg;
    handle_connection(s);
    /* WebSocket sockets stay open; HTTP sockets are closed inside handle_connection */
    return 0;
}
#else
static void *handle_connection_thread(void *arg) {
    sock_t *sp = (sock_t*)arg;
    sock_t s = *sp;
    free(sp);
    handle_connection(s);
    return NULL;
}
#endif

/* ── Process spawner ─────────────────────────────────────────────────────────── */

#ifdef _WIN32
static HANDLE spawn_background(const char *cmd) {
    STARTUPINFOA si = {0};
    PROCESS_INFORMATION pi = {0};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    /* Give child its own null stdin so it doesn't inherit ours */
    SECURITY_ATTRIBUTES sa = {sizeof(sa), NULL, TRUE};
    si.hStdInput  = CreateFileA("nul", GENERIC_READ,
                                FILE_SHARE_READ|FILE_SHARE_WRITE,
                                &sa, OPEN_EXISTING, 0, NULL);
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError  = GetStdHandle(STD_ERROR_HANDLE);

    char buf[4096];
    strncpy(buf, cmd, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    BOOL ok = CreateProcessA(NULL, buf, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi);
    if (si.hStdInput != INVALID_HANDLE_VALUE) CloseHandle(si.hStdInput);
    if (!ok) return NULL;
    CloseHandle(pi.hThread);
    return pi.hProcess;
}
#else
static pid_t spawn_background(const char *cmd) {
    pid_t pid = fork();
    if (pid == 0) { execlp("sh", "sh", "-c", cmd, NULL); _exit(127); }
    return pid;
}
#endif

/* ── File watcher thread ─────────────────────────────────────────────────────── */

#ifdef _WIN32
static DWORD WINAPI watcher_thread(LPVOID arg) {
    (void)arg;
    char cinder_path[512];
    snprintf(cinder_path, sizeof(cinder_path), "%s\\.cinder", g_root);

    HANDLE dir = CreateFileA(cinder_path,
        FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
        NULL);
    if (dir == INVALID_HANDLE_VALUE) return 1;

    char notify_buf[4096];
    DWORD bytes;
    while (g_server_running) {
        if (ReadDirectoryChangesW(dir, notify_buf, sizeof(notify_buf), FALSE,
            FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_FILE_NAME |
            FILE_NOTIFY_CHANGE_SIZE,
            &bytes, NULL, NULL)) {
            /* Debounce: wait a bit for esbuild to finish writing */
            Sleep(80);
            ws_broadcast("{\"type\":\"reload\"}");
        }
    }
    CloseHandle(dir);
    return 0;
}
#else
static void *watcher_thread(void *arg) {
    (void)arg;
    int ifd = inotify_init();
    if (ifd < 0) {
        /* inotify unavailable: fall back to polling */
        time_t last_bundle = 0, last_style = 0;
        while (g_server_running) {
            struct stat st;
            time_t t1 = 0, t2 = 0;
            if (stat(BUNDLE_FILE, &st) == 0) t1 = st.st_mtime;
            if (stat(STYLE_FILE, &st) == 0) t2 = st.st_mtime;
            if ((t1 && t1 != last_bundle) || (t2 && t2 != last_style)) {
                last_bundle = t1; last_style = t2;
                ws_broadcast("{\"type\":\"reload\"}");
            }
            usleep(100000);
        }
        return NULL;
    }

    inotify_add_watch(ifd, CINDER_DIR, IN_CLOSE_WRITE | IN_MOVED_TO);

    char evt_buf[4096]
        __attribute__((aligned(__alignof__(struct inotify_event))));
    struct pollfd pfd = { .fd = ifd, .events = POLLIN };

    while (g_server_running) {
        int ret = poll(&pfd, 1, 200);
        if (ret > 0 && (pfd.revents & POLLIN)) {
            /* Drain all pending events */
            while (read(ifd, evt_buf, sizeof(evt_buf)) > 0) {}
            /* Short debounce for esbuild multi-write */
            usleep(30000);
            while (read(ifd, evt_buf, sizeof(evt_buf)) > 0) {}
            ws_broadcast("{\"type\":\"reload\"}");
        }
    }
    close(ifd);
    return NULL;
}
#endif

/* ── devserver_discover ──────────────────────────────────────────────────────── */

int devserver_discover(DevServerConfig *cfg, const char *root) {
    cfg->port = 5173;
    cfg->root = root;
    cfg->public_dir = "public";
    cfg->entry = NULL;
    cfg->css_entry = NULL;
    cfg->esbuild_bin = NULL;
    cfg->tw_bin = NULL;

    /* Detect entry point from index.html */
    static char entry_buf[512];
    char html[4096];
    snprintf(html, sizeof(html), "%s/index.html", root);
    long sz;
    char *html_content = read_file(html, &sz);
    if (html_content) {
        const char *src_attr = strstr(html_content, "src=\"/src/");
        if (!src_attr) src_attr = strstr(html_content, "src='/src/");
        if (src_attr) {
            char quote = src_attr[4];
            src_attr += 5; /* skip src=" */
            const char *end = strchr(src_attr, quote);
            size_t elen = end ? (size_t)(end - src_attr) : strlen(src_attr);
            if (elen < sizeof(entry_buf) - 1) {
                memcpy(entry_buf, src_attr + 1, elen - 1); /* skip leading / */
                entry_buf[elen - 1] = '\0';
                cfg->entry = entry_buf;
            }
        }
        free(html_content);
    }
    if (!cfg->entry) cfg->entry = "src/main.tsx";

    /* Detect CSS entry */
    static const char *css_candidates[] = {
        "src/index.css", "src/main.css", "src/App.css",
        "src/styles.css", "src/global.css", NULL
    };
    for (int i = 0; css_candidates[i]; i++) {
        if (file_exists(css_candidates[i])) {
            cfg->css_entry = css_candidates[i];
            break;
        }
    }

    /* Find esbuild */
    static const char *esbuild_candidates[] = {
        "node_modules/.bin/esbuild.exe",
        "node_modules/.bin/esbuild",
        "node_modules/esbuild/bin/esbuild",
        NULL
    };
    for (int i = 0; esbuild_candidates[i]; i++) {
        if (file_exists(esbuild_candidates[i])) {
            cfg->esbuild_bin = esbuild_candidates[i];
            break;
        }
    }

    /* Find tailwindcss */
    static const char *tw_candidates[] = {
        "node_modules/.bin/tailwindcss.exe",
        "node_modules/.bin/tailwindcss",
        "node_modules/@tailwindcss/cli/bin/tailwindcss.js",
        NULL
    };
    for (int i = 0; tw_candidates[i]; i++) {
        if (file_exists(tw_candidates[i])) {
            cfg->tw_bin = tw_candidates[i];
            break;
        }
    }

    return 0;
}

/* ── devserver_run ───────────────────────────────────────────────────────────── */

int devserver_run(const DevServerConfig *cfg) {
    if (!cfg->esbuild_bin) {
        fprintf(stderr, "cinder dev: esbuild not found in node_modules/.bin\n"
                        "            Run 'cinder add -D esbuild' first.\n");
        return 1;
    }

#ifdef _WIN32
    InitializeCriticalSection(&g_ws_lock);
#endif

    /* Setup dirs */
    snprintf(g_root, sizeof(g_root), "%s", cfg->root ? cfg->root : ".");
    snprintf(g_public_dir, sizeof(g_public_dir), "%s",
             cfg->public_dir ? cfg->public_dir : "public");
    snprintf(g_entry, sizeof(g_entry), "%s",
             cfg->entry ? cfg->entry : "src/main.tsx");

    mkdirp_single(CINDER_DIR);

#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

    /* ── Spawn esbuild --bundle --watch ── */
    g_use_tw_cdn = (cfg->tw_bin == NULL && cfg->css_entry != NULL);
    char esbuild_cmd[2048];
    snprintf(esbuild_cmd, sizeof(esbuild_cmd),
        "%s %s "
        "--bundle "
        "--format=esm "
        "--jsx=automatic "
        "--jsx-import-source=react "
        "--loader:.tsx=tsx "
        "--loader:.ts=ts "
        "--loader:.jsx=jsx "
        "--loader:.js=js "
        "--loader:.css=empty "
        "--loader:.svg=dataurl "
        "--loader:.png=dataurl "
        "--loader:.jpg=dataurl "
        "--loader:.woff=dataurl "
        "--loader:.woff2=dataurl "
        "--outfile=%s "
        "--sourcemap=inline "
        "--target=esnext "
        "--watch=forever",
        cfg->esbuild_bin, cfg->entry, BUNDLE_FILE);

    g_esbuild_proc = spawn_background(esbuild_cmd);

    /* ── Spawn tailwindcss --watch if available and CSS entry found ── */
    if (cfg->tw_bin && cfg->css_entry) {
        char tw_cmd[1024];
        /* Check if it's a .js file (needs node) */
        const char *ext = strrchr(cfg->tw_bin, '.');
        if (ext && strcmp(ext, ".js") == 0) {
            snprintf(tw_cmd, sizeof(tw_cmd),
                "node %s --input %s --output %s --watch",
                cfg->tw_bin, cfg->css_entry, STYLE_FILE);
        } else {
            snprintf(tw_cmd, sizeof(tw_cmd),
                "%s --input %s --output %s --watch",
                cfg->tw_bin, cfg->css_entry, STYLE_FILE);
        }
        g_tw_proc = spawn_background(tw_cmd);
    }

    /* ── Start file watcher thread ── */
#ifdef _WIN32
    HANDLE wt = CreateThread(NULL, 0, watcher_thread, NULL, 0, NULL);
    (void)wt;
#else
    pthread_t wt;
    pthread_create(&wt, NULL, watcher_thread, NULL);
    pthread_detach(wt);
#endif

    /* ── Start HTTP server ── */
    sock_t server = socket(AF_INET, SOCK_STREAM, 0);
    if (server == SOCK_INVALID) {
        fprintf(stderr, "cinder dev: failed to create socket\n");
        return 1;
    }

    int reuse = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, (char*)&reuse, sizeof(reuse));

    struct sockaddr_in addr = {0};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons((uint16_t)cfg->port);

    if (bind(server, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "cinder dev: port %d already in use\n", cfg->port);
        sock_close(server);
        return 1;
    }
    listen(server, 64);

    /* Print startup banner */
    printf("\n");
    printf("  \033[36mcinder dev\033[0m  \033[2mv0.1.0\033[0m\n\n");
    printf("  \033[32m->\033[0m  Local:   \033[36mhttp://localhost:%d/\033[0m\n", cfg->port);
    if (g_use_tw_cdn)
        printf("  \033[33m!\033[0m   Tailwind CDN (dev only) — no @tailwindcss/cli found\n");
    printf("\n");
    printf("  \033[2mBundling with esbuild...\033[0m\n\n");
    fflush(stdout);

    /* ── Accept loop ── */
    while (g_server_running) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(server, &rfds);
        struct timeval tv = {0, 100000}; /* 100ms timeout */
        int ready = select((int)server + 1, &rfds, NULL, NULL, &tv);
        if (ready <= 0) continue;

        struct sockaddr_in client_addr;
        socklen_t clen = sizeof(client_addr);
        sock_t client = accept(server, (struct sockaddr*)&client_addr, &clen);
        if (client == SOCK_INVALID) continue;

        /* Spawn a thread per connection so WebSocket clients don't block the accept loop */
#ifdef _WIN32
        HANDLE ct = CreateThread(NULL, 0,
            (LPTHREAD_START_ROUTINE)(void*)handle_connection_thread,
            (LPVOID)(uintptr_t)client, 0, NULL);
        if (ct) CloseHandle(ct);
        else { handle_connection(client); sock_close(client); }
#else
        pthread_t ct;
        sock_t *cp = (sock_t*)malloc(sizeof(sock_t));
        *cp = client;
        if (pthread_create(&ct, NULL, handle_connection_thread, cp) == 0)
            pthread_detach(ct);
        else { handle_connection(client); sock_close(client); free(cp); }
#endif
    }

    sock_close(server);
#ifdef _WIN32
    WSACleanup();
    if (g_esbuild_proc) TerminateProcess(g_esbuild_proc, 0);
    if (g_tw_proc) TerminateProcess(g_tw_proc, 0);
#endif
    return 0;
}
