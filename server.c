© 2026 deeyeahh6. All rights reserved.
/* DineSync backend - build: gcc -O2 -o dinesync server.c && ./dinesync [port]
 *
 * Data structures
 *   Hash table    : table sessions (by session id) and menu items (by item id)
 *   Linked list   : every session's cart / order lines
 *   Queue (FIFO)  : kitchen order queue
 *   Priority queue: binary min-heap of staff assistance requests
 *   Stack         : per-session undo history of recent cart operations
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>

#define CANCEL_SECS 300          /* five-minute cancellation window */
#define TABLES 12
#define SESSION_TTL 14400        /* a session expires after 4 hours */
#define HSIZE 128

enum { PENDING, CONFIRMED, PREPARING, COMPLETED, CANCELLED };
static const char *STATUS[] = {"Pending", "Confirmed", "Preparing", "Completed", "Cancelled"};

/* ---------- Hash table (separate chaining) ---------- */
typedef struct HNode { char key[48]; void *val; struct HNode *next; } HNode;
typedef struct { HNode *b[HSIZE]; } HTable;
static unsigned hash(const char *s) { unsigned h = 5381; while (*s) h = h * 33 + (unsigned char)*s++; return h; }
static void ht_put(HTable *t, const char *k, void *v) {
    unsigned i = hash(k) % HSIZE; HNode *n = calloc(1, sizeof *n);
    snprintf(n->key, sizeof n->key, "%s", k); n->val = v; n->next = t->b[i]; t->b[i] = n;
}
static void *ht_get(HTable *t, const char *k) {
    for (HNode *n = t->b[hash(k) % HSIZE]; n; n = n->next) if (!strcmp(n->key, k)) return n->val;
    return NULL;
}

/* ---------- Menu ---------- */
typedef struct { const char *id, *name, *desc, *cat, *img; int price; double rating; } Menu;
#define IMG "https://images.unsplash.com/photo-"
static Menu MENU[] = {
 {"st1","Paneer Tikka","Charred cottage cheese, mint chutney, pickled onion","Starters",IMG"1567188040759-fb8a883dc6d8?w=640&q=70",249,4.6},
 {"st2","Corn Soup","A warm, creamy soup made with tender sweet corn, fresh vegetables, and mild spices","Starters",IMG"1665593998976-d957f2827fe7?q=80&w=1172&auto=format&fit=crop&ixlib=rb-4.1.0&ixid=NHwxMjA3fDA%3D",179,4.3},
 {"st3","Chicken 65","Spiced, crisp-fried chicken with green chilli","Starters",IMG"1610057099443-fde8c4d50f91?w=640&q=70",269,4.7},
 {"mc1","Butter Chicken","Tandoori chicken in a slow-cooked tomato butter gravy","Main Course",IMG"1565557623262-b51c2513a641?w=640&q=70",329,4.8},
 {"mc2","Dal Makhani","Black lentils simmered overnight with cream","Main Course",IMG"1546833999-b9f581a1996d?w=640&q=70",249,4.5},
 {"mc3","Garlic Naan Basket","Three tandoor naans brushed with garlic butter","Main Course",IMG"1756821752957-00bfcadc3748?q=80&w=687&auto=format&fit=crop&ixlib=rb-4.1.0&ixid=NHwxMjA3fDA%3D",119,4.4},
 {"bi1","Hyderabadi Chicken Biryani","Dum-cooked basmati, saffron, tender chicken","Biryani",IMG"1563379091339-03b21ab4a4f8?w=640&q=70",349,4.9},
 {"bi2","Veg Dum Biryani","Seasonal vegetables layered with fragrant rice","Biryani",IMG"1589302168068-964664d93dc0?w=640&q=70",279,4.4},
 {"bi3","Mutton Biryani","Slow-cooked mutton, whole spices, burani raita","Biryani",IMG"1633945274405-b6c8069047b0?w=640&q=70",429,4.8},
 {"de1","Gulab Jamun","Warm milk dumplings in rose-cardamom syrup","Desserts",IMG"1666190092159-3171cf0fbb12?q=80&w=1170&auto=format&fit=crop&ixlib=rb-4.1.0&ixid=NHwxMjA3fDA%3D",129,4.5},
 {"de2","Chocolate Lava Cake","Dark chocolate centre, vanilla bean cream","Desserts",IMG"1578985545062-69928b1d9587?w=640&q=70",169,4.7},
 {"be1","Masala Chai","Assam tea brewed with ginger and cardamom","Beverages",IMG"1571934811356-5cc061b6821f?w=640&q=70",59,4.6},
 {"be2","Fresh Lime Soda","Sweet, salted or mixed, over crushed ice","Beverages",IMG"1513558161293-cdaf765ed2fd?w=640&q=70",89,4.2},
 {"sp1","Chef's Thali","Five curries, dal, rice, bread and a sweet","Specials",IMG"1559561724-732dbca7be1e?q=80&w=1965&auto=format&fit=crop&ixlib=rb-4.1.0&ixid=NHwxMjA3fDA%3D",399,4.8},
};
#define NMENU ((int)(sizeof MENU / sizeof *MENU))
static HTable MENUH, SESS;

/* ---------- Sessions, cart (linked list), undo (stack) ---------- */
typedef struct Item { int id, menu, status; time_t added; struct Item *next; } Item;
typedef struct Op { int item; struct Op *next; } Op;
typedef struct { char sid[20]; int table, active; time_t created; Item *head, *tail; Op *undo; } Session;
typedef struct { char key[20]; Session *cur; } Table;
static Table TBL[TABLES + 1];
static int next_item = 1;

static void rnd(char *o) {
    unsigned char r[8]; FILE *f = fopen("/dev/urandom", "rb");
    if (!f || fread(r, 1, 8, f) != 8) for (int i = 0; i < 8; i++) r[i] = rand();
    if (f) fclose(f);
    for (int i = 0; i < 8; i++) sprintf(o + 2 * i, "%02x", r[i]);
}
static Session *new_session(int t) {
    Session *s = calloc(1, sizeof *s); rnd(s->sid);
    s->table = t; s->active = 1; s->created = time(NULL);
    ht_put(&SESS, s->sid, s); TBL[t].cur = s; return s;
}
static int alive(Session *s) {
    if (s->active && time(NULL) - s->created > SESSION_TTL) s->active = 0;
    return s->active;
}
static void refresh(Item *i) { if (i->status == PENDING && time(NULL) - i->added >= CANCEL_SECS) i->status = CONFIRMED; }
static int left_secs(Item *i) { refresh(i); if (i->status != PENDING) return 0; return CANCEL_SECS - (int)(time(NULL) - i->added); }

/* ---------- Kitchen queue (FIFO) ---------- */
typedef struct QNode { Item *it; Session *s; struct QNode *next; } QNode;
static QNode *qh, *qt;
static void enqueue(Item *it, Session *s) {
    QNode *n = calloc(1, sizeof *n); n->it = it; n->s = s;
    if (qt) qt->next = n; else qh = n; qt = n;
}
static void prune_queue(void) {   /* drop cancelled / completed tickets */
    QNode *prev = NULL, *n = qh;
    while (n) {
        QNode *nx = n->next;
        if (n->it->status == CANCELLED || n->it->status == COMPLETED) {
            if (prev) prev->next = nx; else qh = nx;
            if (n == qt) qt = prev;
            free(n);
        } else prev = n;
        n = nx;
    }
}

/* ---------- Assistance requests (binary min-heap) ---------- */
typedef struct { int id, table, type, pri, done; time_t at; } Req;
static const char *RKEY[]  = {"call","help","tissues","cutlery","water","plates","sauces","straws"};
static const char *RNAME[] = {"Call Staff","Need Assistance","Extra Tissues","Cutlery Set","Drinking Water","Extra Plates","Sauces & Condiments","Straws"};
static const int RPRI[]    = {1, 2, 3, 3, 3, 3, 3, 3};   /* lower number = more urgent */
#define NREQ 8
static Req heap[256]; static int hn, next_req = 1;
static int before(Req *a, Req *b) { return a->pri < b->pri || (a->pri == b->pri && a->id < b->id); }
static void hpush(Req *h, int *n, Req r) {
    int i = (*n)++; h[i] = r;
    while (i && before(&h[i], &h[(i - 1) / 2])) { Req t = h[i]; h[i] = h[(i - 1) / 2]; h[(i - 1) / 2] = t; i = (i - 1) / 2; }
}
static Req hpop(Req *h, int *n) {
    Req top = h[0]; h[0] = h[--(*n)]; int i = 0;
    for (;;) {
        int l = 2 * i + 1, r = l + 1, m = i;
        if (l < *n && before(&h[l], &h[m])) m = l;
        if (r < *n && before(&h[r], &h[m])) m = r;
        if (m == i) break;
        Req t = h[i]; h[i] = h[m]; h[m] = t; i = m;
    }
    return top;
}
static void heap_compact(void) {
    Req keep[256]; int k = 0;
    for (int i = 0; i < hn; i++) if (!heap[i].done) keep[k++] = heap[i];
    hn = 0; for (int i = 0; i < k; i++) hpush(heap, &hn, keep[i]);
}
static int req_open(int table, int type) {
    for (int i = 0; i < hn; i++) if (!heap[i].done && heap[i].table == table && heap[i].type == type) return 1;
    return 0;
}

/* ---------- Small helpers: output buffer, JSON, query ---------- */
typedef struct { char *p; size_t n, cap; } Buf;
static void bp(Buf *b, const char *f, ...) {
    char t[1024]; va_list a; va_start(a, f); int k = vsnprintf(t, sizeof t, f, a); va_end(a);
    if (k >= (int)sizeof t) k = sizeof t - 1;
    if (b->n + k + 1 > b->cap) { b->cap = (b->n + k + 1) * 2; b->p = realloc(b->p, b->cap); }
    memcpy(b->p + b->n, t, k + 1); b->n += k;
}
static int jstr(const char *body, const char *key, char *out, size_t n) {
    char pat[48]; snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(body, pat); if (!p) return 0;
    p = strchr(p + strlen(pat), ':'); if (!p) return 0;
    p++; while (*p == ' ') p++;
    size_t i = 0;
    if (*p == '"') { p++; while (*p && *p != '"' && i < n - 1) out[i++] = *p++; }
    else while (*p && *p != ',' && *p != '}' && *p != ' ' && i < n - 1) out[i++] = *p++;
    out[i] = 0; return i > 0;
}
static int jint(const char *b, const char *k, int *v) { char t[32]; if (!jstr(b, k, t, sizeof t)) return 0; *v = atoi(t); return 1; }
static int qget(const char *q, const char *key, char *out, size_t n) {
    char pat[48]; snprintf(pat, sizeof pat, "%s=", key);
    const char *p = q;
    while ((p = strstr(p, pat))) {
        if (p == q || p[-1] == '&') {
            p += strlen(pat); size_t i = 0;
            while (*p && *p != '&' && i < n - 1) out[i++] = *p++;
            out[i] = 0; return 1;
        }
        p++;
    }
    return 0;
}
static int err(Buf *o, int code, const char *m) { bp(o, "{\"error\":\"%s\"}", m); return code; }
static int ok(Buf *o) { bp(o, "{\"ok\":true}"); return 200; }

/* Resolve the session for a mutating call; closed or expired sessions are refused (410). */
static Session *active_sess(const char *body, Buf *o, int *code) {
    char sid[40]; Session *s;
    if (!jstr(body, "sid", sid, sizeof sid) || !(s = ht_get(&SESS, sid))) { *code = err(o, 404, "Unknown table session"); return NULL; }
    if (!alive(s)) { *code = err(o, 410, "This table session has ended"); return NULL; }
    return s;
}
static Item *find_item(Session *s, int id) { for (Item *i = s->head; i; i = i->next) if (i->id == id) return i; return NULL; }

/* Cancellation is validated here, on the server clock. */
static int cancel_item(Item *it, Buf *o) {
    refresh(it);
    if (it->status == CANCELLED) return err(o, 409, "Already cancelled");
    if (it->status != PENDING) return err(o, 409, "The 5-minute cancellation window has ended");
    it->status = CANCELLED; return ok(o);
}
static int total_of(Session *s, int *count) {
    int t = 0; *count = 0;
    for (Item *i = s->head; i; i = i->next) if (i->status != CANCELLED) { t += MENU[i->menu].price; (*count)++; }
    return t;
}

/* ---------- SHA-256 (used to derive permanent, unguessable QR keys) ---------- */
static const unsigned K256[64]={0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
#define ROR(x,n) (((x)>>(n))|((x)<<(32-(n))))
static void sha256(const unsigned char *m, size_t len, unsigned char out[32]) {
    unsigned h[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    size_t total = ((len + 9 + 63) / 64) * 64; unsigned char *b = calloc(total, 1);
    memcpy(b, m, len); b[len] = 0x80;
    unsigned long long bits = (unsigned long long)len * 8; for (int i = 0; i < 8; i++) b[total - 1 - i] = (unsigned char)(bits >> (8 * i));
    for (size_t off = 0; off < total; off += 64) {
        unsigned w[64], a, bb, c, d, e, f, g, hh;
        for (int i = 0; i < 16; i++) w[i] = (unsigned)b[off+4*i] << 24 | b[off+4*i+1] << 16 | b[off+4*i+2] << 8 | b[off+4*i+3];
        for (int i = 16; i < 64; i++) { unsigned s0 = ROR(w[i-15],7)^ROR(w[i-15],18)^(w[i-15]>>3), s1 = ROR(w[i-2],17)^ROR(w[i-2],19)^(w[i-2]>>10); w[i] = w[i-16] + s0 + w[i-7] + s1; }
        a=h[0]; bb=h[1]; c=h[2]; d=h[3]; e=h[4]; f=h[5]; g=h[6]; hh=h[7];
        for (int i = 0; i < 64; i++) {
            unsigned S1 = ROR(e,6)^ROR(e,11)^ROR(e,25), ch = (e&f)^(~e&g), t1 = hh + S1 + ch + K256[i] + w[i];
            unsigned S0 = ROR(a,2)^ROR(a,13)^ROR(a,22), mj = (a&bb)^(a&c)^(bb&c), t2 = S0 + mj;
            hh=g; g=f; f=e; e=d+t1; d=c; c=bb; bb=a; a=t1+t2;
        }
        h[0]+=a; h[1]+=bb; h[2]+=c; h[3]+=d; h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
    }
    free(b); for (int i = 0; i < 8; i++) { out[4*i]=h[i]>>24; out[4*i+1]=h[i]>>16; out[4*i+2]=h[i]>>8; out[4*i+3]=h[i]; }
}

/* ---------- Staff / kitchen login: PINs + random session tokens ---------- */
typedef struct { char tok[20]; int role; } Tok;           /* role 1 = kitchen, 2 = staff */
static Tok toks[64]; static int ntok; static char g_tok[48];
static char PIN[3][40]; static int fails; static time_t fail_t;
static int role_of(void) {
    if (!g_tok[0]) return 0;
    for (int i = 0; i < 64; i++) if (toks[i].role && !strcmp(toks[i].tok, g_tok)) return toks[i].role;
    return 0;
}

/* ---------- API routes ---------- */
static int route(const char *m, const char *path, const char *q, const char *body, Buf *o) {
    char tmp[64]; int a, code = 0, first; Session *s; time_t now = time(NULL);
    int post = !strcmp(m, "POST");

    if (!strncmp(path, "/api/kitchen", 12) && role_of() != 1) return err(o, 401, "Kitchen login required");
    if (!strncmp(path, "/api/staff", 10) && role_of() != 2) return err(o, 401, "Staff login required");
    if (post && !strcmp(path, "/api/login")) {
        char role[16], pin[48]; if (now - fail_t > 60) fails = 0;
        if (fails >= 8) return err(o, 429, "Too many attempts. Please wait a minute");
        int r = jstr(body, "role", role, sizeof role) ? (!strcmp(role, "kitchen") ? 1 : !strcmp(role, "staff") ? 2 : 0) : 0;
        if (!r || !jstr(body, "pin", pin, sizeof pin) || strcmp(pin, PIN[r])) { fails++; fail_t = now; return err(o, 401, "Wrong PIN"); }
        Tok *t = &toks[ntok++ % 64]; rnd(t->tok); t->role = r;
        bp(o, "{\"token\":\"%s\"}", t->tok); return 200;
    }

    if (!strcmp(path, "/api/menu")) {
        bp(o, "[");
        for (int i = 0; i < NMENU; i++)
            bp(o, "%s{\"id\":\"%s\",\"name\":\"%s\",\"desc\":\"%s\",\"cat\":\"%s\",\"img\":\"%s\",\"price\":%d,\"rating\":%.1f}",
               i ? "," : "", MENU[i].id, MENU[i].name, MENU[i].desc, MENU[i].cat, MENU[i].img, MENU[i].price, MENU[i].rating);
        bp(o, "]"); return 200;
    }
    if (post && !strcmp(path, "/api/join")) {     /* QR scan: table number + secret key */
        if (!jint(body, "table", &a) || a < 1 || a > TABLES) return err(o, 400, "Invalid table");
        if (!jstr(body, "key", tmp, sizeof tmp) || strcmp(tmp, TBL[a].key)) return err(o, 403, "This QR code is not valid");
        s = TBL[a].cur; if (!s || !alive(s)) s = new_session(a);
        bp(o, "{\"sid\":\"%s\",\"table\":%d}", s->sid, a); return 200;
    }
    if (!strcmp(path, "/api/state")) {
        char sid[40] = ""; qget(q, "sid", sid, sizeof sid);
        if (!(s = ht_get(&SESS, sid))) return err(o, 404, "Unknown table session");
        int cnt, tot = total_of(s, &cnt); first = 1;
        bp(o, "{\"table\":%d,\"active\":%s,\"total\":%d,\"count\":%d,\"items\":[", s->table, alive(s) ? "true" : "false", tot, cnt);
        for (Item *i = s->head; i; i = i->next) {
            refresh(i); if (i->status == CANCELLED) continue;
            bp(o, "%s{\"id\":%d,\"item\":\"%s\",\"name\":\"%s\",\"price\":%d,\"status\":\"%s\",\"left\":%d}",
               first ? "" : ",", i->id, MENU[i->menu].id, MENU[i->menu].name, MENU[i->menu].price, STATUS[i->status], left_secs(i));
            first = 0;
        }
        bp(o, "],\"requests\":["); first = 1;
        for (int i = 0; i < hn; i++) if (!heap[i].done && heap[i].table == s->table) { bp(o, "%s%d", first ? "" : ",", heap[i].type); first = 0; }
        bp(o, "]}"); return 200;
    }
    if (post && !strcmp(path, "/api/cart/add")) {
        if (!(s = active_sess(body, o, &code))) return code;
        Menu *mi; if (!jstr(body, "item", tmp, sizeof tmp) || !(mi = ht_get(&MENUH, tmp))) return err(o, 404, "Unknown menu item");
        Item *it = calloc(1, sizeof *it); it->id = next_item++; it->menu = (int)(mi - MENU); it->added = now; it->status = PENDING;
        if (s->tail) s->tail->next = it; else s->head = it; s->tail = it;
        Op *op = malloc(sizeof *op); op->item = it->id; op->next = s->undo; s->undo = op;   /* push */
        enqueue(it, s); bp(o, "{\"ok\":true,\"line\":%d}", it->id); return 200;
    }
    if (post && !strcmp(path, "/api/cart/cancel")) {       /* by line id, or newest cancellable line of a menu item */
        if (!(s = active_sess(body, o, &code))) return code;
        Item *it = NULL;
        if (jint(body, "line", &a)) { if (!(it = find_item(s, a))) return err(o, 404, "Item not found"); }
        else if (jstr(body, "item", tmp, sizeof tmp)) {
            Menu *mi = ht_get(&MENUH, tmp); if (!mi) return err(o, 404, "Unknown menu item");
            for (Item *i = s->head; i; i = i->next) { refresh(i); if (i->menu == mi - MENU && i->status == PENDING) it = i; }
            if (!it) return err(o, 409, "The 5-minute cancellation window has ended");
        } else return err(o, 400, "Missing line or item");
        return cancel_item(it, o);
    }
    if (post && !strcmp(path, "/api/cart/undo")) {         /* pop the stack until a still-cancellable add is found */
        if (!(s = active_sess(body, o, &code))) return code;
        while (s->undo) {
            Op *op = s->undo; s->undo = op->next; Item *it = find_item(s, op->item); free(op);
            if (it) { refresh(it); if (it->status == PENDING) { it->status = CANCELLED; return ok(o); } }
        }
        return err(o, 409, "Nothing to undo");
    }
    if (post && !strcmp(path, "/api/assist")) {
        if (!(s = active_sess(body, o, &code))) return code;
        int t = -1; jstr(body, "type", tmp, sizeof tmp);
        for (int k = 0; k < NREQ; k++) if (!strcmp(tmp, RKEY[k])) t = k;
        if (t < 0) return err(o, 400, "Unknown request type");
        if (req_open(s->table, t)) return err(o, 429, "Already requested. Staff have been notified");
        if (hn >= 256) heap_compact();
        if (hn >= 256) return err(o, 503, "Staff queue is full, please wave to a team member");
        Req r = {next_req++, s->table, t, RPRI[t], 0, now}; hpush(heap, &hn, r); return ok(o);
    }
    if (!strcmp(path, "/api/kitchen")) {
        prune_queue(); first = 1; bp(o, "[");
        for (QNode *n = qh; n; n = n->next) {
            refresh(n->it);
            bp(o, "%s{\"line\":%d,\"table\":%d,\"name\":\"%s\",\"qty\":1,\"status\":\"%s\",\"left\":%d}",
               first ? "" : ",", n->it->id, n->s->table, MENU[n->it->menu].name, STATUS[n->it->status], left_secs(n->it));
            first = 0;
        }
        bp(o, "]"); return 200;
    }
    if (post && !strcmp(path, "/api/kitchen/status")) {
        int line; if (!jint(body, "line", &line) || !jstr(body, "status", tmp, sizeof tmp)) return err(o, 400, "Missing fields");
        int st = !strcmp(tmp, "preparing") ? PREPARING : !strcmp(tmp, "completed") ? COMPLETED : -1;
        if (st < 0) return err(o, 400, "Unknown status");
        for (QNode *n = qh; n; n = n->next) if (n->it->id == line) {
            if (n->it->status == CANCELLED) return err(o, 409, "Cancelled by customer");
            n->it->status = st; return ok(o);
        }
        return err(o, 404, "Order not found");
    }
    if (!strcmp(path, "/api/staff")) {
        bp(o, "{\"tables\":[");
        for (int t = 1; t <= TABLES; t++) {
            s = TBL[t].cur; int live = s && alive(s), cnt = 0, tot = s ? total_of(s, &cnt) : 0;
            bp(o, "%s{\"table\":%d,\"active\":%s,\"items\":%d,\"total\":%d,\"mins\":%d,\"key\":\"%s\"}",
               t > 1 ? "," : "", t, live ? "true" : "false", cnt, tot, live ? (int)((now - s->created) / 60) : 0, TBL[t].key);
        }
        bp(o, "],\"requests\":["); Req copy[256]; int n = hn; memcpy(copy, heap, sizeof(Req) * hn); first = 1;
        while (n) {      /* pop a copy of the heap: most urgent first */
            Req r = hpop(copy, &n); if (r.done) continue;
            bp(o, "%s{\"id\":%d,\"table\":%d,\"type\":\"%s\",\"urgent\":%s,\"ago\":%d}", first ? "" : ",", r.id, r.table, RNAME[r.type], r.pri == 1 ? "true" : "false", (int)(now - r.at));
            first = 0;
        }
        bp(o, "]}"); return 200;
    }
    if (post && !strcmp(path, "/api/staff/done")) {
        if (!jint(body, "id", &a)) return err(o, 400, "Missing id");
        for (int i = 0; i < hn; i++) if (heap[i].id == a) { heap[i].done = 1; return ok(o); }
        return err(o, 404, "Request not found");
    }
    if (post && !strcmp(path, "/api/staff/close")) {       /* close, then open a fresh session for the next guests */
        if (!jint(body, "table", &a) || a < 1 || a > TABLES) return err(o, 400, "Invalid table");
        if (TBL[a].cur) TBL[a].cur->active = 0;
        new_session(a); return ok(o);
    }
    return err(o, 404, "Not found");
}

/* ---------- HTTP plumbing ---------- */
static void reply(int fd, int code, const char *ctype, const char *body, size_t len) {
    const char *txt = code == 200 ? "OK" : code == 404 ? "Not Found" : "Error";
    char h[256]; int n = snprintf(h, sizeof h, "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n", code, txt, ctype, len);
    if (write(fd, h, n) < 0) return;
    size_t off = 0; while (off < len) { ssize_t w = write(fd, body + off, len - off); if (w <= 0) break; off += w; }
}
static void serve_file(int fd, const char *path) {
    char f[600]; if (strstr(path, "..")) { reply(fd, 404, "text/plain", "Not found", 9); return; }
    snprintf(f, sizeof f, "public%s", strcmp(path, "/") ? path : "/index.html");
    FILE *fp = fopen(f, "rb"); if (!fp) { reply(fd, 404, "text/plain", "Not found", 9); return; }
    fseek(fp, 0, SEEK_END); long n = ftell(fp); rewind(fp);
    char *d = malloc(n + 1); if (fread(d, 1, n, fp) != (size_t)n) n = 0; fclose(fp);
    const char *ct = strstr(f, ".html") ? "text/html; charset=utf-8" : strstr(f, ".css") ? "text/css" : strstr(f, ".js") ? "application/javascript" : "application/octet-stream";
    reply(fd, 200, ct, d, n); free(d);
}
static void handle(int fd) {
    static char req[16384]; int len = 0;
    while (len < (int)sizeof req - 1) {
        int r = read(fd, req + len, sizeof req - 1 - len); if (r <= 0) break; len += r; req[len] = 0;
        char *he = strstr(req, "\r\n\r\n");
        if (he) { char *cl = strcasestr(req, "content-length:"); int need = cl ? atoi(cl + 15) : 0; if (len - (he + 4 - req) >= need) break; }
    }
    req[len] = 0; char m[8], target[512];
    if (sscanf(req, "%7s %511s", m, target) != 2) return;
    char *body = strstr(req, "\r\n\r\n"); body = body ? body + 4 : "";
    char *q = strchr(target, '?'); if (q) *q++ = 0; else q = "";
    if (strncmp(target, "/api/", 5)) { serve_file(fd, target); return; }
    g_tok[0] = 0; char *th = strcasestr(req, "x-token:");
    if (th) { th += 8; while (*th == ' ') th++; size_t i = 0; while (*th && *th != '\r' && *th != '\n' && i < sizeof g_tok - 1) g_tok[i++] = *th++; g_tok[i] = 0; }
    Buf o = {0}; int code = route(m, target, q, body, &o);
    reply(fd, code, "application/json", o.p ? o.p : "{}", o.n); free(o.p);
}

int main(int argc, char **argv) {
    int port = argc > 1 ? atoi(argv[1]) : 8080;
    signal(SIGPIPE, SIG_IGN); srand(time(NULL));
    for (int i = 0; i < NMENU; i++) ht_put(&MENUH, MENU[i].id, &MENU[i]);
    char secret[80] = ""; const char *es = getenv("QR_SECRET");
    if (es && *es) snprintf(secret, sizeof secret, "%s", es);
    else {
        FILE *sf = fopen("qrsecret.txt", "r");
        if (!sf || fscanf(sf, "%79s", secret) != 1) {
            rnd(secret); rnd(secret + 16); FILE *w = fopen("qrsecret.txt", "w"); if (w) { fprintf(w, "%s\n", secret); fclose(w); }
            printf("QR_SECRET is not set, so a random one was made. Set QR_SECRET to keep QR codes valid after a redeploy.\n");
        }
        if (sf) fclose(sf);
    }
    for (int t = 1; t <= TABLES; t++) {                /* permanent per-table key = first 8 bytes of SHA-256(secret + table) */
        char in[200]; unsigned char d[32]; snprintf(in, sizeof in, "dinesync|%s|table|%d", secret, t);
        sha256((unsigned char *)in, strlen(in), d); for (int i = 0; i < 8; i++) sprintf(TBL[t].key + 2 * i, "%02x", d[i]);
    }
    const char *ev[3] = {NULL, getenv("KITCHEN_PIN"), getenv("STAFF_PIN")};
    for (int r = 1; r <= 2; r++) {
        if (ev[r] && *ev[r]) snprintf(PIN[r], sizeof PIN[r], "%s", ev[r]);
        else { char hx[20]; rnd(hx); snprintf(PIN[r], sizeof PIN[r], "%06lu", strtoul(hx, NULL, 16) % 1000000);
               printf("%s_PIN is not set. Temporary PIN: %s\n", r == 1 ? "KITCHEN" : "STAFF", PIN[r]); }
    }
    for (int t = 1; t <= TABLES; t++) new_session(t);

    int s = socket(AF_INET, SOCK_STREAM, 0), on = 1; setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    struct sockaddr_in a = {0}; a.sin_family = AF_INET; a.sin_addr.s_addr = INADDR_ANY; a.sin_port = htons(port);
    if (bind(s, (struct sockaddr *)&a, sizeof a) < 0 || listen(s, 64) < 0) { perror("bind/listen"); return 1; }
    printf("DineSync running on http://localhost:%d\n  Customer (table 1): http://localhost:%d/?table=1&k=%s\n  Kitchen: http://localhost:%d/#/kitchen\n  Staff:   http://localhost:%d/#/staff\n", port, port, TBL[1].key, port, port);
    fflush(stdout);
    for (;;) {
        int c = accept(s, NULL, NULL); if (c < 0) continue;
        struct timeval tv = {2, 0}; setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        handle(c); close(c);
    }
}
