/*
 * theme_highland.h - DuncOS "Highland" theme tokens
 * Created by Bryan Duncan
 *
 * Colours are 0x00RRGGBB. Alpha values are 0-255 (255 = opaque).
 * Change values here to restyle the whole desktop.
 */
#ifndef THEME_HIGHLAND_H
#define THEME_HIGHLAND_H

/* ---- Palette ---- */
#define HL_NAVY          0x0A1428u   /* desktop base / sky              */
#define HL_RIDGE_FAR     0x13213Du   /* back mountain layer             */
#define HL_RIDGE_MID     0x0F1A31u
#define HL_RIDGE_NEAR    0x0A1324u   /* front mountain layer            */
#define HL_MOON          0xFFC83Du   /* moon glow (drawn at low alpha)  */

#define HL_GOLD          0xFFC83Du   /* crown, highlights               */
#define HL_CYAN          0x4FD1E8u   /* accent: links, selection, logo  */
#define HL_GREEN         0x4ADE80u
#define HL_RED           0xFF6B6Bu

#define HL_TEXT          0xE6EDF7u   /* primary text                    */
#define HL_TEXT_DIM      0xB8C4D6u   /* secondary text                  */
#define HL_TEXT_FAINT    0x8A98AEu   /* captions, status                */

#define HL_BAR_BG        0x080E1Cu   /* top bar                         */
#define HL_BAR_ALPHA     184
#define HL_WIN_BG        0x10192Eu   /* window body                     */
#define HL_WIN_ALPHA     245
#define HL_WIN_BORDER    0x1E2A44u
#define HL_TERM_BG       0x090F1Du
#define HL_DOCK_BG       0x0C1426u
#define HL_DOCK_ALPHA    200
#define HL_SHADOW        0x000000u
#define HL_SHADOW_ALPHA  70

/* ---- Accent (change this one value to recolour the UI) ---- */
#define HL_ACCENT        HL_CYAN

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
