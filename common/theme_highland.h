/*
 * theme_highland.h - theme tokens (now the AXIOM palette: black and cream; first designed as "Highland")
 * Created by Bryan Duncan
 *
 * Colours are 0x00RRGGBB. Alpha values are 0-255 (255 = opaque).
 * Change values here to restyle the whole desktop.
 */
#ifndef THEME_HIGHLAND_H
#define THEME_HIGHLAND_H

/* ---- Palette: AXIOM (black and cream) ---- */
#define HL_NAVY          0x050505u   /* desktop base                    */
#define HL_RIDGE_FAR     0x101010u   /* (Highland wallpaper layers, unused by AXIOM) */
#define HL_RIDGE_MID     0x0C0C0Cu
#define HL_RIDGE_NEAR    0x080808u
#define HL_MOON          0xF4F1DEu

#define HL_CREAM         0xF4F1DEu   /* the AXIOM logo colour           */
#define HL_GOLD          0xE3C27Au   /* warnings, minimise button       */
#define HL_CYAN          HL_CREAM
#define HL_GREEN         0x8CCB95u
#define HL_RED           0xE8705Eu

#define HL_TEXT          0xF4F1DEu   /* primary text                    */
#define HL_TEXT_DIM      0xBDB9A6u   /* secondary text                  */
#define HL_TEXT_FAINT    0x8A877Au   /* captions, status                */

#define HL_BAR_BG        0x000000u   /* top bar                         */
#define HL_BAR_ALPHA     205
#define HL_WIN_BG        0x111111u   /* window body                     */
#define HL_WIN_ALPHA     248
#define HL_WIN_BORDER    0x2A2A28u
#define HL_TERM_BG       0x090909u
#define HL_DOCK_BG       0x0D0D0Du
#define HL_DOCK_ALPHA    215
#define HL_SHADOW        0x000000u
#define HL_SHADOW_ALPHA  110

/* ---- Accent (change this one value to recolour the UI) ---- */
#include <stdint.h>
extern uint32_t hl_accent;                    /* Settings > Personalise (cream unless changed) */
#define HL_ACCENT        hl_accent
#define HL_ON_ACCENT     0x0B0B0Bu   /* text on accent-coloured buttons */

/* ---- Metrics (pixels) ---- */
#define HL_BAR_H         36
#define HL_TITLE_H       40
#define HL_WIN_RADIUS    14
#define HL_DOCK_ICON     52
#define HL_DOCK_RADIUS   22
#define HL_DOCK_PAD      8
#define HL_DOCK_GAP      6
#define HL_DOCK_MARGIN   18          /* gap between dock and screen edge */
#define HL_DOT_R         6           /* window close/min/max buttons    */
#define HL_SHADOW_OFF    14

#endif
