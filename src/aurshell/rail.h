/* rail.h — the AurOS shell surface: Cards on a Rail.
 *
 * ─────────────────────────────────────────────────────────────────
 *  There is no desktop, no taskbar, no dock, no launcher, no overview.
 *
 *  Everything open is one horizontal row of large cards. The focused
 *  card sits centre-screen; its neighbours peek in at both edges. Click
 *  a peeking card and it slides to centre. Nothing overlaps, nothing
 *  minimises, nothing is ever hidden behind anything else.
 *
 *  The far-left card is always Home. You cannot lose it: it is the end
 *  of a line, and a line has ends.
 * ─────────────────────────────────────────────────────────────────
 *
 *  WHY, for the person this is for:
 *
 *  Every other desktop separates the thing you are looking at from the
 *  mechanism for reaching other things -- a taskbar, a dock, Activities,
 *  a bar plus a launcher. That second layer is where non-technical
 *  people lose things ("it disappeared", "how do I get back?"). Here the
 *  row IS both. What you can reach is always on screen, at all times,
 *  at full size or peeking.
 *
 *  Closest prior art is webOS cards and phone recents screens. The
 *  difference that matters: those are a MODE you enter and leave. Here
 *  the row is the permanent surface -- there is no other view to return
 *  to, so there is no mode to be lost inside.
 *
 *  Two concepts total: cards, and left/right. That is the whole model.
 */
#ifndef AUROS_RAIL_H
#define AUROS_RAIL_H

#include "draw.h"
#include "anim.h"
#include "../common/font.h"
#include "../common/theme.h"

#define RAIL_MAX_CARDS 24
#define TILE_MAX       12

typedef enum {
    CARD_HOME,      /* always index 0, never closable */
    CARD_APP,
    CARD_PLACEHOLDER
} card_kind;

typedef struct {
    card_kind kind;
    char      title[64];
    char      subtitle[96];
    int       icon;             /* tile_icon, for the card's own badge */
    uint32_t  tint;             /* accent used by this card            */
    surface  *content;          /* live view, or NULL for a stub       */
} card;

/* The six things on Home. Deliberately six: enough to cover what this
 * user does, few enough to read without scanning. */
typedef enum {
    ICON_GLOBE, ICON_MAIL, ICON_PHOTOS, ICON_FILES,
    ICON_SETTINGS, ICON_HELP, ICON_WINDOW, ICON_PLUS
} tile_icon;

typedef struct {
    char      label[32];
    char      hint[48];
    tile_icon icon;
    uint32_t  tint;
} home_tile;

typedef struct {
    /* theme-derived */
    int   radius, radius_sm, margin, padding, border;
    int   bar_h, blur_r, shadow_r;
    float panel_a, shadow_a;
    uint32_t bg, bg_alt, surface_c, surface_hi, overlay;
    uint32_t muted, subtle, fg, fg_hi;
    uint32_t accent, accent_alt, accent_warm, err, ok, info;
    char  brand[48];

    /* rail state */
    card  cards[RAIL_MAX_CARDS];
    int   n_cards;
    int   focus;            /* index of the centred card              */
    tween slide;            /* animates toward focus                  */
    int   hover;            /* card under the pointer, -1 for none    */
    int   hover_tile;       /* home tile under the pointer, -1 none   */

    home_tile tiles[TILE_MAX];
    int   n_tiles;
} rail;

void rail_init(rail *r, const theme_t *t);
void rail_theme(rail *r, const theme_t *t);

/* Geometry, so hit-testing and painting agree by construction. */
rect rail_card_rect(const rail *r, int w, int h, int index);
rect rail_tile_rect(const rail *r, rect card_area, int index);

int  rail_hit_card(const rail *r, int w, int h, int mx, int my);
int  rail_hit_tile(const rail *r, int w, int h, int mx, int my);

void rail_focus(rail *r, int index, float anim_s);
void rail_open(rail *r, const char *title, const char *sub, tile_icon ic, uint32_t tint);
void rail_close(rail *r, int index);
int  rail_step(rail *r, float dt);      /* 1 while animating */

void rail_paint(rail *r, surface *s, font *big, font *mid, font *small,
                const surface *wallpaper);

#endif
