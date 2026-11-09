/*
 * 3dsToJoy - Nintendo 3DS jako kontroler PC przez Wi-Fi (UDP).
 *
 * Co robi:
 *   - przy pierwszym uruchomieniu pyta o adres IP komputera (klawiatura systemowa),
 *   - zapisuje IP i ustawienia w sdmc:/3ds/3dsToJoy/config.txt,
 *   - ok. 60 razy na sekunde wysyla pakiet UDP ze stanem przyciskow, galek,
 *     ekranu dotykowego, zyroskopu i akcelerometru na <IP>:5000.
 *
 * Gorny ekran: podglad tego, co jest wysylane.
 * Dolny ekran: touchpad (cala powierzchnia), kropka pod palcem.
 *              Zielona ramka = wysylanie, pomaranczowa = pauza (menu),
 *              czerwony "X" = dotyk wylaczony.
 *
 * Menu: przytrzymaj SELECT+START przez ok. 1 s (wysylanie sie zatrzymuje).
 *   D-pad gora/dol = ruch kursora, A = wybierz/przelacz, B = wznow.
 *   Opcje: tryb pada (DS4 / XInput), zyroskop+akcelerometr ON/OFF,
 *          dotyk ON/OFF, zmiana IP, wznow, wyjscie.   HOME tez zamyka aplikacje.
 */

#include <3ds.h>

#include <malloc.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define APP_NAME          "3dsToJoy"
#define APP_VERSION       "0.3"
#define PORT              5000
#define SOC_BUF_SIZE      0x100000
#define CONFIG_DIR        "sdmc:/3ds/3dsToJoy"
#define CONFIG_FILE       CONFIG_DIR "/config.txt"
#define MENU_HOLD_FRAMES  60

/* Bity przyciskow wysylane do PC: A B SELECT START D-pad R L X Y + ZL ZR + dotyk.
 * (hidKeysHeld zawiera tez bity "galka jako D-pad", ktore tu pomijamy.) */
#define KEY_MASK (0x0FFF | KEY_ZL | KEY_ZR | KEY_TOUCH)

/* Bity pola flags w pakiecie (ustawienia wybrane w menu na 3DS) */
#define FLAG_DS4    (1 << 0)   /* 1 = wirtualny DS4, 0 = XInput (Xbox 360) */
#define FLAG_GYRO   (1 << 1)   /* 1 = zyroskop + akcelerometr wlaczone     */
#define FLAG_TOUCH  (1 << 2)   /* 1 = dotyk wlaczony                       */

/*
 * Format pakietu (little-endian, 36 bajtow).
 * Python: struct.Struct("<4sHIhhhhHHhhhhhhH")
 */
typedef struct __attribute__((packed)) {
    char     magic[4];             /* "3DJ3"                                   */
    uint16_t seq;                  /* numer pakietu (zawija sie), do ignorowania starych */
    uint32_t keys;                 /* bity KEY_* z libctru                     */
    int16_t  cpad_x, cpad_y;       /* Circle Pad, ok. -156..156                */
    int16_t  cstick_x, cstick_y;   /* C-Stick (New 3DS / Circle Pad Pro)       */
    uint16_t touch_x, touch_y;     /* ekran dotykowy 320x240, 0 gdy brak dotyku */
    int16_t  gyro_x, gyro_y, gyro_z;    /* zyroskop (surowe wartosci)          */
    int16_t  accel_x, accel_y, accel_z; /* akcelerometr (surowe wartosci)      */
    uint16_t flags;                /* FLAG_* - tryb i wlaczone funkcje         */
} Packet;

_Static_assert(sizeof(Packet) == 36, "Packet musi miec 36 bajtow");

/* ------------------------------------------------------------------ */
/* Ustawienia                                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    bool ds4;     /* true = DS4, false = XInput */
    bool gyro;    /* zyroskop + akcelerometr    */
    bool touch;   /* dotyk                      */
} Settings;

static Settings g_set = { true, true, true };   /* domyslnie: DS4, wszystko wlaczone */

static uint16_t current_flags(void)
{
    return (g_set.ds4   ? FLAG_DS4   : 0)
         | (g_set.gyro  ? FLAG_GYRO  : 0)
         | (g_set.touch ? FLAG_TOUCH : 0);
}

/* Sensory ruchu wlaczamy tylko, gdy sa potrzebne (oszczednosc baterii). */
static bool s_motion_on = false;

static void motion_set(bool on)
{
    if (on == s_motion_on) return;
    if (on) {
        HIDUSER_EnableGyroscope();
        HIDUSER_EnableAccelerometer();
    } else {
        HIDUSER_DisableAccelerometer();
        HIDUSER_DisableGyroscope();
    }
    s_motion_on = on;
}

/* ------------------------------------------------------------------ */
/* IP i konfiguracja                                                   */
/* ------------------------------------------------------------------ */

static bool parse_ip(const char *s, uint32_t *out)
{
    unsigned a, b, c, d;
    char tail;
    if (sscanf(s, "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4) return false;
    if (a > 255 || b > 255 || c > 255 || d > 255) return false;
    *out = (a << 24) | (b << 16) | (c << 8) | d;
    return true;
}

static void parse_setting(const char *line)
{
    if      (!strcmp(line, "mode=ds4"))    g_set.ds4   = true;
    else if (!strcmp(line, "mode=xinput")) g_set.ds4   = false;
    else if (!strcmp(line, "gyro=1"))      g_set.gyro  = true;
    else if (!strcmp(line, "gyro=0"))      g_set.gyro  = false;
    else if (!strcmp(line, "touch=1"))     g_set.touch = true;
    else if (!strcmp(line, "touch=0"))     g_set.touch = false;
}

/* config.txt: 1. linia = IP, kolejne = ustawienia (mode=, gyro=, touch=).
 * Stary plik z samym IP tez zadziala (reszta zostaje domyslna). */
static bool load_config(char *ip, size_t len)
{
    FILE *f = fopen(CONFIG_FILE, "r");
    if (!f) return false;

    char line[64] = {0};
    bool ok = fgets(line, sizeof line, f) != NULL;

    if (ok) {
        line[strcspn(line, "\r\n")] = '\0';

        uint32_t tmp;
        ok = parse_ip(line, &tmp);
        if (ok) {
            snprintf(ip, len, "%s", line);
            while (fgets(line, sizeof line, f)) {
                line[strcspn(line, "\r\n")] = '\0';
                parse_setting(line);
            }
        }
    }

    fclose(f);
    return ok;
}

static void save_config(const char *ip)
{
    mkdir("sdmc:/3ds", 0777);      /* bledy ignorujemy - katalog moze juz istniec */
    mkdir(CONFIG_DIR, 0777);

    FILE *f = fopen(CONFIG_FILE, "w");
    if (!f) return;
    fprintf(f, "%s\nmode=%s\ngyro=%d\ntouch=%d\n",
            ip, g_set.ds4 ? "ds4" : "xinput", g_set.gyro ? 1 : 0, g_set.touch ? 1 : 0);
    fclose(f);
}

/* Klawiatura systemowa. Zwraca true, gdy wpisano poprawny IP (zapisany do ip). */
static bool prompt_ip(char *ip, size_t len)
{
    char buf[32];
    char initial[32];
    const char *hint = "PC IP, e.g. 192.168.0.10";

    snprintf(initial, sizeof initial, "%s", ip);

    for (;;) {
        SwkbdState swkbd;
        swkbdInit(&swkbd, SWKBD_TYPE_NUMPAD, 2, 15);
        swkbdSetNumpadKeys(&swkbd, '.', 0);           /* kropka po lewej stronie zera */
        swkbdSetHintText(&swkbd, hint);
        swkbdSetValidation(&swkbd, SWKBD_NOTEMPTY_NOTBLANK, 0, 0);
        if (initial[0]) swkbdSetInitialText(&swkbd, initial);

        buf[0] = '\0';
        if (swkbdInputText(&swkbd, buf, sizeof buf) != SWKBD_BUTTON_CONFIRM)
            return false;

        uint32_t tmp;
        if (parse_ip(buf, &tmp)) {
            snprintf(ip, len, "%s", buf);
            return true;
        }

        /* zly format - pytamy jeszcze raz, zostawiajac to, co wpisano */
        snprintf(initial, sizeof initial, "%s", buf);
        hint = "Invalid IP! Try again";
    }
}

/* ------------------------------------------------------------------ */
/* Pakiety                                                             */
/* ------------------------------------------------------------------ */

/* Pusty pakiet (nic wcisniete) - ale z AKTUALNYMI flagami, zeby PC nie
 * przelaczylo trybu pada przez przypadek. */
static void make_neutral(Packet *p, uint16_t seq)
{
    memset(p, 0, sizeof *p);
    memcpy(p->magic, "3DJ3", 4);
    p->seq   = seq;
    p->flags = current_flags();
}

static void fill_packet(Packet *p, uint16_t seq)
{
    circlePosition cpad, cstick;
    touchPosition  touch;

    u32 held = hidKeysHeld();
    hidCircleRead(&cpad);
    hidCstickRead(&cstick);

    make_neutral(p, seq);
    p->keys     = held & KEY_MASK;
    p->cpad_x   = cpad.dx;
    p->cpad_y   = cpad.dy;
    p->cstick_x = cstick.dx;
    p->cstick_y = cstick.dy;

    if (g_set.touch) {
        if (held & KEY_TOUCH) {
            hidTouchRead(&touch);
            p->touch_x = touch.px;
            p->touch_y = touch.py;
        }
    } else {
        p->keys &= ~KEY_TOUCH;          /* dotyk wylaczony - PC o nim nie wie */
    }

    if (g_set.gyro) {
        angularRate gyro;
        accelVector accel;
        hidGyroRead(&gyro);
        hidAccelRead(&accel);
        p->gyro_x  = gyro.x;
        p->gyro_y  = gyro.y;
        p->gyro_z  = gyro.z;
        p->accel_x = accel.x;
        p->accel_y = accel.y;
        p->accel_z = accel.z;
    }
}

static bool send_packet(int sock, const struct sockaddr_in *dest, const Packet *p)
{
    return sendto(sock, p, sizeof *p, 0,
                  (const struct sockaddr *)dest, sizeof *dest) >= 0;
}

/* ------------------------------------------------------------------ */
/* Gorny ekran (konsola tekstowa)                                      */
/* ------------------------------------------------------------------ */

/* Nazwy przyciskow pokazywane na ekranie (kolejnosc = kolejnosc wyswietlania) */
static const struct { u32 bit; const char *name; } BUTTON_NAMES[] = {
    { KEY_A, "A" },       { KEY_B, "B" },         { KEY_X, "X" },       { KEY_Y, "Y" },
    { KEY_L, "L" },       { KEY_R, "R" },         { KEY_ZL, "ZL" },     { KEY_ZR, "ZR" },
    { KEY_DUP, "Up" },    { KEY_DDOWN, "Down" },  { KEY_DLEFT, "Left" }, { KEY_DRIGHT, "Right" },
    { KEY_START, "Start" }, { KEY_SELECT, "Select" }, { KEY_TOUCH, "Touch" },
};

/* Ostatnio narysowany stan - zeby przerysowywac tylko, gdy cos sie zmienilo */
static Packet g_shown;

static void draw_main(const char *ip)
{
    consoleClear();
    printf("\x1b[1;1H%s v%s", APP_NAME, APP_VERSION);
    printf("\x1b[3;1HTarget: %s:%d", ip, PORT);
    printf("\x1b[4;1HMode:   %s", g_set.ds4 ? "DS4" : "XInput (Xbox 360)");
    printf("\x1b[5;1HGyro: %s   Touch: %s", g_set.gyro ? "ON" : "OFF", g_set.touch ? "ON" : "OFF");
    printf("\x1b[19;1HHold SELECT+START for 1 s = menu");
    printf("\x1b[20;1HHOME = exit");

    memset(&g_shown, 0xFF, sizeof g_shown);   /* wymus przerysowanie wejsc */
}

/* Pokazuje to, co faktycznie leci do PC (ten sam pakiet, po masce przyciskow). */
static void draw_inputs(const Packet *p)
{
    Packet cur = *p;
    cur.seq = 0;                               /* numer pakietu zmienia sie zawsze */
    if (memcmp(&cur, &g_shown, sizeof cur) == 0) return;
    g_shown = cur;

    char   btn[96] = "";
    size_t n = 0;
    for (size_t i = 0; i < sizeof BUTTON_NAMES / sizeof BUTTON_NAMES[0]; i++) {
        if (!(p->keys & BUTTON_NAMES[i].bit)) continue;
        if (n >= sizeof btn) break;
        n += snprintf(btn + n, sizeof btn - n, "%s%s", n ? "+" : "", BUTTON_NAMES[i].name);
    }
    if (n == 0) snprintf(btn, sizeof btn, "-");

    printf("\x1b[10;1HButtons:   %-38.38s", btn);
    printf("\x1b[11;1HRaw keys:  0x%05lX     ", (unsigned long)p->keys);
    printf("\x1b[12;1HCircle:    %4d %4d   ", p->cpad_x, p->cpad_y);
    printf("\x1b[13;1HC-Stick:   %4d %4d   ", p->cstick_x, p->cstick_y);
    printf("\x1b[14;1HTouch:     %3u %3u   ", (unsigned)p->touch_x, (unsigned)p->touch_y);
    printf("\x1b[15;1HGyro:      %6d %6d %6d  ", p->gyro_x, p->gyro_y, p->gyro_z);
    printf("\x1b[16;1HAccel:     %6d %6d %6d  ", p->accel_x, p->accel_y, p->accel_z);
}

static void draw_counters(unsigned long sent, unsigned long errors)
{
    printf("\x1b[7;1HSent: %-10lu Errors: %-10lu", sent, errors);
    if (errors > 0 && sent == 0)
        printf("\x1b[8;1HNo packets going out - is Wi-Fi on?");
}

/* ------------------------------------------------------------------ */
/* Menu                                                                */
/* ------------------------------------------------------------------ */

enum { MI_MODE, MI_GYRO, MI_TOUCH, MI_IP, MI_RESUME, MI_QUIT, MI_COUNT };

static void draw_menu(const char *ip, int sel)
{
    char item[MI_COUNT][40];
    snprintf(item[MI_MODE],   sizeof item[0], "Controller:   %s", g_set.ds4 ? "DS4" : "XInput (Xbox 360)");
    snprintf(item[MI_GYRO],   sizeof item[0], "Gyro + accel: %s", g_set.gyro ? "ON" : "OFF");
    snprintf(item[MI_TOUCH],  sizeof item[0], "Touchpad:     %s", g_set.touch ? "ON" : "OFF");
    snprintf(item[MI_IP],     sizeof item[0], "Change PC IP...");
    snprintf(item[MI_RESUME], sizeof item[0], "Resume");
    snprintf(item[MI_QUIT],   sizeof item[0], "Quit");

    consoleClear();
    printf("\x1b[1;1H%s - MENU (input paused)", APP_NAME);
    printf("\x1b[3;1HTarget: %s:%d", ip, PORT);

    for (int i = 0; i < MI_COUNT; i++)
        printf("\x1b[%d;1H%c %s", 6 + i, i == sel ? '>' : ' ', item[i]);

    printf("\x1b[15;1HD-pad up/down: move   A: select");
    printf("\x1b[16;1HB: resume");
}

/* ------------------------------------------------------------------ */
/* Dolny ekran (touchpad, rysowanie prosto w framebuffer)              */
/* ------------------------------------------------------------------ */

#define SCREEN_W   320
#define SCREEN_H   240

#define COL_BG      0x0A0A12
#define COL_GRID    0x22222E
#define COL_OK      0x30C060
#define COL_PAUSED  0xE08020
#define COL_RING    0x40A0FF
#define COL_DOT     0xFFFFFF
#define COL_OFF     0x70283A

/* Framebuffer 3DS jest obrocony: kolumny po 240 pikseli, oś Y liczona od dolu,
 * format BGR (3 bajty na piksel). x,y podajemy normalnie (0,0 = lewy gorny rog). */
static void put_px(u8 *fb, int x, int y, u32 rgb)
{
    if (x < 0 || x >= SCREEN_W || y < 0 || y >= SCREEN_H) return;
    u8 *p = fb + 3 * (SCREEN_H * x + (SCREEN_H - 1 - y));
    p[0] = rgb & 0xFF;            /* B */
    p[1] = (rgb >> 8) & 0xFF;     /* G */
    p[2] = (rgb >> 16) & 0xFF;    /* R */
}

static void fill_rect(u8 *fb, int x, int y, int w, int h, u32 rgb)
{
    for (int yy = y; yy < y + h; yy++)
        for (int xx = x; xx < x + w; xx++)
            put_px(fb, xx, yy, rgb);
}

static void frame_rect(u8 *fb, int x, int y, int w, int h, int t, u32 rgb)
{
    fill_rect(fb, x,         y,         w, t, rgb);
    fill_rect(fb, x,         y + h - t, w, t, rgb);
    fill_rect(fb, x,         y,         t, h, rgb);
    fill_rect(fb, x + w - t, y,         t, h, rgb);
}

static void clear_screen(u8 *fb, u32 rgb)
{
    const u8 b = rgb & 0xFF, g = (rgb >> 8) & 0xFF, r = (rgb >> 16) & 0xFF;
    for (int i = 0; i < SCREEN_W * SCREEN_H; i++) {
        fb[0] = b; fb[1] = g; fb[2] = r;
        fb += 3;
    }
}

static void draw_pad(bool touching, int tx, int ty, bool paused, bool touch_enabled)
{
    u8 *fb = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL);

    clear_screen(fb, COL_BG);

    /* osie przez srodek */
    fill_rect(fb, SCREEN_W / 2 - 1, 0, 2, SCREEN_H, COL_GRID);
    fill_rect(fb, 0, SCREEN_H / 2 - 1, SCREEN_W, 2, COL_GRID);

    /* dotyk wylaczony: czerwony "X" przez caly pad */
    if (!touch_enabled) {
        for (int y = 0; y < SCREEN_H; y++) {
            int x = y * SCREEN_W / SCREEN_H;
            for (int t = -1; t <= 1; t++) {
                put_px(fb, x + t, y, COL_OFF);
                put_px(fb, SCREEN_W - 1 - x + t, y, COL_OFF);
            }
        }
    }

    /* ramka: zielona = wysylamy, pomaranczowa = pauza */
    frame_rect(fb, 0, 0, SCREEN_W, SCREEN_H, 3, paused ? COL_PAUSED : COL_OK);

    /* kropka pod palcem */
    if (touch_enabled && touching && !paused) {
        frame_rect(fb, tx - 8, ty - 8, 17, 17, 2, COL_RING);
        fill_rect(fb, tx - 3, ty - 3, 7, 7, COL_DOT);
    }
}

/* ------------------------------------------------------------------ */
/* Glowna petla                                                        */
/* ------------------------------------------------------------------ */

static void present(void)
{
    gfxFlushBuffers();
    gfxSwapBuffers();
    gspWaitForVBlank();
}

static void wait_for_start(void)
{
    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START) break;
        present();
    }
}

static void run(void)
{
    char ip[32] = "";

    if (!load_config(ip, sizeof ip)) {
        if (!prompt_ip(ip, sizeof ip)) {
            consoleClear();
            printf("No IP set.\n\nPress START to exit.\n");
            wait_for_start();
            return;
        }
        save_config(ip);
    }

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        consoleClear();
        printf("Cannot create socket.\nIs Wi-Fi enabled?\n\nPress START to exit.\n");
        wait_for_start();
        return;
    }

    struct sockaddr_in dest;
    memset(&dest, 0, sizeof dest);
    dest.sin_family = AF_INET;
    dest.sin_port   = htons(PORT);

    uint32_t ip_host = 0;
    parse_ip(ip, &ip_host);
    dest.sin_addr.s_addr = htonl(ip_host);

    motion_set(g_set.gyro);

    bool          menu   = false;
    bool          quit   = false;
    int           sel    = 0;
    int           combo  = 0;
    int           frame  = 0;
    uint16_t      seq    = 0;
    unsigned long sent   = 0;
    unsigned long errors = 0;

    draw_main(ip);

    while (aptMainLoop()) {
        hidScanInput();
        u32 held = hidKeysHeld();
        u32 down = hidKeysDown();

        if (!menu) {
            if ((held & (KEY_SELECT | KEY_START)) == (KEY_SELECT | KEY_START))
                combo++;
            else
                combo = 0;

            if (combo >= MENU_HOLD_FRAMES) {
                /* kilka "pustych" pakietow, zeby PC puscil wszystkie przyciski */
                for (int i = 0; i < 3; i++) {
                    Packet n;
                    make_neutral(&n, seq++);
                    send_packet(sock, &dest, &n);
                }
                menu  = true;
                combo = 0;
                sel   = 0;
                draw_menu(ip, sel);
            } else {
                Packet p;
                fill_packet(&p, seq++);
                if (send_packet(sock, &dest, &p)) sent++; else errors++;

                draw_inputs(&p);
                if (++frame % 30 == 0) draw_counters(sent, errors);
            }
        } else {
            bool changed = false;

            if (down & KEY_DUP)   { sel = (sel + MI_COUNT - 1) % MI_COUNT; changed = true; }
            if (down & KEY_DDOWN) { sel = (sel + 1) % MI_COUNT;            changed = true; }

            if (down & KEY_A) {
                switch (sel) {
                case MI_MODE:
                    g_set.ds4 = !g_set.ds4;
                    save_config(ip);
                    break;
                case MI_GYRO:
                    g_set.gyro = !g_set.gyro;
                    motion_set(g_set.gyro);
                    save_config(ip);
                    break;
                case MI_TOUCH:
                    g_set.touch = !g_set.touch;
                    save_config(ip);
                    break;
                case MI_IP:
                    if (prompt_ip(ip, sizeof ip)) {
                        save_config(ip);
                        if (parse_ip(ip, &ip_host))
                            dest.sin_addr.s_addr = htonl(ip_host);
                    }
                    break;
                case MI_RESUME:
                    menu = false;
                    break;
                case MI_QUIT:
                    quit = true;
                    break;
                }
                changed = true;
            }
            if (down & KEY_B) menu = false;

            if (quit) break;

            if (!menu)        draw_main(ip);          /* wznowienie */
            else if (changed) draw_menu(ip, sel);
        }

        /* dolny ekran: touchpad */
        touchPosition tp;
        hidTouchRead(&tp);
        draw_pad((held & KEY_TOUCH) != 0, tp.px, tp.py, menu, g_set.touch);

        present();
    }

    motion_set(false);
    close(sock);
}

int main(void)
{
    gfxInitDefault();
    consoleInit(GFX_TOP, NULL);

    u32 *soc_buf = memalign(0x1000, SOC_BUF_SIZE);

    if (soc_buf && R_SUCCEEDED(socInit(soc_buf, SOC_BUF_SIZE))) {
        run();
        socExit();
    } else {
        printf("Network init failed.\n\nPress START to exit.\n");
        wait_for_start();
    }

    free(soc_buf);
    gfxExit();
    return 0;
}
