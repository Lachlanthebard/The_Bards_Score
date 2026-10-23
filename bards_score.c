#include <furi.h>
#include <furi_hal_speaker.h>
#include <gui/gui.h>
#include <gui/view_port.h>
#include <input/input.h>
#include <storage/storage.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_NOTES 256
#define SCORE_DIR EXT_PATH("apps_data/bards_score")
#define SCORE_FILE SCORE_DIR "/melody.bsc"
#define SCORE_TMP SCORE_DIR "/melody.bsc.tmp"
#define XML_FILE SCORE_DIR "/melody.musicxml"
#define XML_TMP SCORE_DIR "/melody.musicxml.tmp"
#define STAFF_BOTTOM 47
#define STAFF_SPACING 7
#define MENU_CAT_COUNT 11
#define MARK_COUNT 12
#define BARLINE_COUNT 7
#define VISIBLE_NOTES 5

/* Packed to 9 bytes (was 18). Two voices of 256 notes is about 4.5 KB. */
typedef struct {
    uint8_t pitch;
    uint8_t chord[3];
    uint8_t duration : 3;
    uint8_t rest : 1;
    uint8_t accent : 1;
    uint8_t staccato : 1;
    uint8_t fermata : 1;
    uint8_t dotted : 1;
    uint8_t slur : 2;
    uint8_t tie : 2;
    uint8_t chord_count : 2;
    uint8_t repeat_start : 1;
    uint8_t repeat_end : 1;
    uint8_t key_change : 1;
    uint8_t key_minor : 1;
    uint8_t tenuto : 1;
    uint8_t staccatissimo : 1;
    uint8_t marcato : 1;
    uint8_t ornament : 3;   /* 0 none, 1 trill, 2 mordent, 3 inverted mordent, 4 turn */
    uint8_t aftermark : 2;  /* 0 none, 1 breath mark, 2 caesura */
    uint8_t barline : 3;    /* style of the barline ending this note's measure */
    int8_t key_fifths;
} Note;
_Static_assert(sizeof(Note) <= 10, "Note must stay packed");

typedef enum {ScreenScore, ScreenPitch, ScreenDuration, ScreenMenu, ScreenTime, ScreenTempo, ScreenKey, ScreenAbout, ScreenBarline, ScreenSubmenu} Screen;

typedef struct {
    FuriMutex* mutex;
    FuriMessageQueue* queue;
    ViewPort* viewport;
    Note notes[MAX_NOTES];
    Note other_notes[MAX_NOTES];
    uint16_t other_count, other_selected;
    uint8_t about_page;
    uint8_t voice;
    bool combined, separate_staves, chord_edit;
    uint8_t chord_pitch;
    uint16_t count, selected;
    uint8_t pitch, duration, menu, menu_top, beats, beat_type, time_choice;
    int8_t fifths;
    bool minor, adding_rest, running;
    bool inserting_key;
    int8_t edit_fifths; bool edit_minor;
    Screen screen;
    char notice[22];
    uint16_t tempo;
    bool playing, speaker_owned, sounding;
    uint16_t play_index, play_end;
    uint32_t note_end_tick, sound_end_tick;
    bool play_both;
    uint32_t both_start, both_last_sound;
    uint16_t both_cycle;
    /* One-level undo for deleted notes. */
    Note undo_note;
    uint16_t undo_at;
    uint8_t undo_voice;
    bool undo_valid;
    /* Playback extras: repeats and chord arpeggiation. */
    uint8_t repeat_used[MAX_NOTES / 8];
    bool play_single;
    uint32_t chord_last;
    uint16_t chord_cycle;
    bool paused, resume_sound;
    uint32_t pause_tick;
    uint32_t note_start_tick;
    int8_t orn_off;
    uint8_t menu_cat, sub, sub_top, barline_choice;
} AppState;

static const char* const durations[] = {"Whole", "Half", "Quarter", "Eighth", "16th"};
static const char* const pitch_names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
static const char* const xml_types[] = {"whole", "half", "quarter", "eighth", "16th"};
static const uint8_t time_beats[] = {2, 3, 4, 6};
static const uint8_t time_denoms[] = {4, 4, 4, 8};
static const char* const major_names[] = {
    "Cb", "Gb", "Db", "Ab", "Eb", "Bb", "F", "C", "G", "D", "A", "E", "B", "F#", "C#"
};
static const char* const minor_names[] = {
    "Ab", "Eb", "Bb", "F", "C", "G", "D", "A", "E", "B", "F#", "C#", "G#", "D#", "A#"
};
/* Treble-clef signature order: F C G D A E B; B E A D G C F. */
static const int8_t sharp_steps[] = {8, 5, 9, 6, 3, 7, 4};
static const int8_t flat_steps[] = {4, 7, 3, 6, 2, 5, 1};
static const uint8_t sharp_letters[] = {3, 0, 4, 1, 5, 2, 6};
static const uint8_t flat_letters[] = {6, 2, 5, 1, 4, 0, 3};
static const char* const step_names[] = {"C", "D", "E", "F", "G", "A", "B"};
static const uint8_t natural_pc[] = {0, 2, 4, 5, 7, 9, 11};
static const uint8_t degree_pc[] = {0, 0, 1, 1, 2, 3, 3, 4, 4, 5, 5, 6};

static const char* const barline_names[BARLINE_COUNT] = {
    "Single (auto)", "Double", "Final", "Dashed", "Dotted", "Heavy", "Invisible"
};
static const char* const xml_bar_styles[BARLINE_COUNT] = {
    "regular", "light-light", "light-heavy", "dashed", "dotted", "heavy", "none"
};
static const char* const mark_names[MARK_COUNT] = {
    "Staccato", "Accent", "Tenuto", "Staccatissimo", "Marcato", "Fermata",
    "Trill", "Mordent", "Inv. mordent", "Turn", "Breath mark", "Caesura"
};

static bool mark_get(const Note* n, uint8_t m) {
    switch(m) {
    case 0: return n->staccato;
    case 1: return n->accent;
    case 2: return n->tenuto;
    case 3: return n->staccatissimo;
    case 4: return n->marcato;
    case 5: return n->fermata;
    case 6: return n->ornament == 1;
    case 7: return n->ornament == 2;
    case 8: return n->ornament == 3;
    case 9: return n->ornament == 4;
    case 10: return n->aftermark == 1;
    default: return n->aftermark == 2;
    }
}

/* Ornaments are mutually exclusive, as are breath mark and caesura. */
static void mark_toggle(Note* n, uint8_t m) {
    switch(m) {
    case 0: n->staccato ^= 1; break;
    case 1: n->accent ^= 1; break;
    case 2: n->tenuto ^= 1; break;
    case 3: n->staccatissimo ^= 1; break;
    case 4: n->marcato ^= 1; break;
    case 5: n->fermata ^= 1; break;
    case 6: n->ornament = n->ornament == 1 ? 0 : 1; break;
    case 7: n->ornament = n->ornament == 2 ? 0 : 2; break;
    case 8: n->ornament = n->ornament == 3 ? 0 : 3; break;
    case 9: n->ornament = n->ornament == 4 ? 0 : 4; break;
    case 10: n->aftermark = n->aftermark == 1 ? 0 : 1; break;
    default: n->aftermark = n->aftermark == 2 ? 0 : 2; break;
    }
}

/* ---- Score tools menu -------------------------------------------------
 * Two levels, like a notation palette: a list of categories, each opening
 * a list of entries. An entry is either an action id (below ActCount) or
 * a note mark (ActCount + mark index). Entries that are on/off settings
 * show a checkbox and keep the submenu open so several can be set in a row. */
typedef enum {
    ActAddRest, ActTime, ActKey, ActSlurStart, ActSlurEnd, ActTieStart, ActTieEnd,
    ActSave, ActLoad, ActExport, ActClear,
    ActPlay, ActPlayFrom, ActPreview, ActTempo,
    ActVoice, ActCombined, ActChordAdd, ActChordRemove, ActXmlLayout, ActPlayBoth,
    ActHelp, ActDotted, ActRepeatStart, ActRepeatEnd, ActKeyChange, ActKeyRemove,
    ActUndo, ActBarline,
    ActCount
} MenuAction;

#define MARK_ENTRY(m) ((uint8_t)(ActCount + (m)))

static const char* const action_names[ActCount] = {
    [ActAddRest] = "Add rest", [ActTime] = "Time signature", [ActKey] = "Key signature",
    [ActSlurStart] = "Slur start", [ActSlurEnd] = "Slur end",
    [ActTieStart] = "Tie start", [ActTieEnd] = "Tie end",
    [ActSave] = "Save score", [ActLoad] = "Load score", [ActExport] = "Export XML",
    [ActClear] = "Clear score",
    [ActPlay] = "Play score", [ActPlayFrom] = "Play from here",
    [ActPreview] = "Preview note", [ActTempo] = "Set tempo",
    [ActVoice] = "Switch voice", [ActCombined] = "Combined view",
    [ActChordAdd] = "Add chord tone", [ActChordRemove] = "Remove chord tone",
    [ActXmlLayout] = "XML: 2 staves", [ActPlayBoth] = "Play both voices",
    [ActHelp] = "About / Help", [ActDotted] = "Dotted note",
    [ActRepeatStart] = "Repeat start", [ActRepeatEnd] = "Repeat end",
    [ActKeyChange] = "Insert key change", [ActKeyRemove] = "Remove key change",
    [ActUndo] = "Undo delete", [ActBarline] = "Barline style",
};

static const uint8_t cat_notes[] = {ActAddRest, ActDotted, ActChordAdd, ActChordRemove, ActUndo};
static const uint8_t cat_keytime[] = {ActTime, ActKey, ActKeyChange, ActKeyRemove};
static const uint8_t cat_barlines[] = {ActBarline, ActRepeatStart, ActRepeatEnd};
static const uint8_t cat_slurs[] = {ActSlurStart, ActSlurEnd, ActTieStart, ActTieEnd};
static const uint8_t cat_artic[] = {MARK_ENTRY(0), MARK_ENTRY(1), MARK_ENTRY(2), MARK_ENTRY(3), MARK_ENTRY(4)};
static const uint8_t cat_orn[] = {MARK_ENTRY(6), MARK_ENTRY(7), MARK_ENTRY(8), MARK_ENTRY(9)};
static const uint8_t cat_breath[] = {MARK_ENTRY(5), MARK_ENTRY(10), MARK_ENTRY(11)};
static const uint8_t cat_voices[] = {ActVoice, ActCombined};
static const uint8_t cat_play[] = {ActPlay, ActPlayFrom, ActPreview, ActPlayBoth, ActTempo};
static const uint8_t cat_file[] = {ActSave, ActLoad, ActExport, ActXmlLayout, ActClear};
static const uint8_t cat_help[] = {ActHelp};

typedef struct {
    const char* name;
    const uint8_t* items;
    uint8_t count;
} MenuCat;

#define CAT(label, arr) {label, arr, (uint8_t)(sizeof(arr) / sizeof((arr)[0]))}
/* A category with a single entry runs it directly (Help). */
static const MenuCat menu_cats[MENU_CAT_COUNT] = {
    CAT("Notes", cat_notes),
    CAT("Key & time", cat_keytime),
    CAT("Barlines", cat_barlines),
    CAT("Slurs & ties", cat_slurs),
    CAT("Articulations", cat_artic),
    CAT("Ornaments", cat_orn),
    CAT("Breaths & pauses", cat_breath),
    CAT("Voices", cat_voices),
    CAT("Playback", cat_play),
    CAT("File", cat_file),
    CAT("Help", cat_help),
};

static const char* entry_name(uint8_t e) {
    return e >= ActCount ? mark_names[e - ActCount] : action_names[e];
}

/* -1: plain action (no checkbox). 0/1: on/off setting for the selection. */
static int entry_state(const AppState* a, uint8_t e) {
    const Note* n = a->count ? &a->notes[a->selected] : NULL;
    if(e >= ActCount) return (n && mark_get(n, e - ActCount)) ? 1 : 0;
    switch(e) {
    case ActSlurStart: return (n && n->slur == 1) ? 1 : 0;
    case ActSlurEnd: return (n && n->slur == 2) ? 1 : 0;
    case ActTieStart: return (n && n->tie == 1) ? 1 : 0;
    case ActTieEnd: return (n && n->tie == 2) ? 1 : 0;
    case ActDotted: return (n && n->dotted) ? 1 : 0;
    case ActRepeatStart: return (n && n->repeat_start) ? 1 : 0;
    case ActRepeatEnd: return (n && n->repeat_end) ? 1 : 0;
    case ActCombined: return a->combined ? 1 : 0;
    case ActXmlLayout: return a->separate_staves ? 1 : 0;
    default: return -1;
    }
}

static uint8_t duration_ticks(uint8_t d) {return (uint8_t)(32U >> d);}
static uint8_t note_ticks(Note n) {uint8_t t = duration_ticks(n.duration); return n.dotted ? t + t / 2 : t;}
static uint8_t measure_ticks(const AppState* a) {return (uint8_t)(a->beats * (32U / a->beat_type));}
static void pitch_label(uint8_t p, char* b, size_t n) {snprintf(b, n, "%s%d", pitch_names[p % 12], (int)p / 12 - 1);}
static int note_step(uint8_t p) {return ((int)p / 12 - 5) * 7 + degree_pc[p % 12] - 2;}
static int step_y(int step) {return STAFF_BOTTOM - (step * STAFF_SPACING + (step >= 0 ? 1 : -1)) / 2;}
static int note_y(uint8_t p) {return step_y(note_step(p));}

/* A key change belongs to the note at its position and remains active
 * until the next change. The MIDI pitches are never transposed. */
static void active_key(const AppState* a, const Note* notes, uint16_t count,
                       uint16_t index, int8_t* fifths, bool* minor) {
    *fifths = a->fifths;
    *minor = a->minor;
    for(uint16_t i = 0; i <= index && i < count; ++i) {
        if(notes[i].key_change) {
            *fifths = notes[i].key_fifths;
            *minor = notes[i].key_minor != 0;
        }
    }
}

static void draw_staff(Canvas* c) {
    for(int i = 0; i < 5; ++i) {
        canvas_draw_line(c, 2, STAFF_BOTTOM - i * STAFF_SPACING,
                         125, STAFF_BOTTOM - i * STAFF_SPACING);
    }

    /* Treble clef: tall, narrow upper loop */
    canvas_draw_line(c, 12, 10, 9, 14);
    canvas_draw_line(c, 9, 14, 8, 20);
    canvas_draw_line(c, 8, 20, 9, 26);
    canvas_draw_line(c, 9, 26, 12, 30);
    canvas_draw_line(c, 12, 10, 15, 14);
    canvas_draw_line(c, 15, 14, 16, 19);
    canvas_draw_line(c, 16, 19, 15, 24);
    canvas_draw_line(c, 15, 24, 12, 29);

    /* Sweeping S-shaped body */
    canvas_draw_line(c, 12, 29, 8, 33);
    canvas_draw_line(c, 8, 33, 5, 38);
    canvas_draw_line(c, 5, 38, 5, 43);
    canvas_draw_line(c, 5, 43, 8, 47);
    canvas_draw_line(c, 8, 47, 13, 49);
    canvas_draw_line(c, 13, 49, 18, 47);
    canvas_draw_line(c, 18, 47, 21, 43);
    canvas_draw_line(c, 12, 29, 10, 34);
    canvas_draw_line(c, 10, 34, 8, 39);
    canvas_draw_line(c, 8, 39, 9, 43);

    /* Spiral around the G line */
    canvas_draw_line(c, 21, 43, 21, 38);
    canvas_draw_line(c, 21, 38, 18, 35);
    canvas_draw_line(c, 18, 35, 13, 34);
    canvas_draw_line(c, 13, 34, 9, 36);
    canvas_draw_line(c, 9, 36, 8, 40);
    canvas_draw_line(c, 8, 40, 10, 43);
    canvas_draw_line(c, 10, 43, 14, 44);
    canvas_draw_line(c, 14, 44, 17, 42);
    canvas_draw_line(c, 17, 42, 17, 39);
    canvas_draw_line(c, 17, 39, 15, 38);

    /* Descending stem and hooked tail */
    canvas_draw_line(c, 11, 27, 13, 38);
    canvas_draw_line(c, 13, 38, 15, 48);
    canvas_draw_line(c, 15, 48, 16, 53);
    canvas_draw_line(c, 16, 53, 15, 57);
    canvas_draw_line(c, 15, 57, 12, 59);
    canvas_draw_line(c, 12, 59, 9, 58);
    canvas_draw_disc(c, 9, 58, 2);
}

static void draw_accidental(Canvas* c, int x, int y, int alter) {
    if(alter == 1) {
        canvas_draw_line(c, x - 2, y - 6, x - 2, y + 5);
        canvas_draw_line(c, x + 1, y - 6, x + 1, y + 5);
        canvas_draw_line(c, x - 4, y - 2, x + 3, y - 3);
        canvas_draw_line(c, x - 4, y + 2, x + 3, y + 1);
    } else if(alter == -1) {
        canvas_draw_line(c, x - 1, y - 7, x - 1, y + 5);
        canvas_draw_line(c, x - 1, y + 1, x + 3, y - 1);
        canvas_draw_line(c, x + 3, y - 1, x + 2, y + 4);
        canvas_draw_line(c, x + 2, y + 4, x - 1, y + 5);
    } else {
        canvas_draw_line(c, x - 2, y - 6, x - 2, y + 5);
        canvas_draw_line(c, x + 2, y - 5, x + 2, y + 6);
        canvas_draw_line(c, x - 2, y - 1, x + 2, y - 3);
        canvas_draw_line(c, x - 2, y + 4, x + 2, y + 2);
    }
}

static int key_alter(int8_t fifths, uint8_t letter) {
    if(fifths > 0) for(int i = 0; i < fifths; ++i) if(sharp_letters[i] == letter) return 1;
    if(fifths < 0) for(int i = 0; i < -fifths; ++i) if(flat_letters[i] == letter) return -1;
    return 0;
}

/* MIDI preserves sound. Spelling prefers the selected key signature. */
static void spell_pitch(int8_t fifths, uint8_t pitch, uint8_t* letter, int* alter) {
    int pc = pitch % 12;
    int best = 100;
    uint8_t best_letter = 0;
    int best_alter = 0;
    for(uint8_t l = 0; l < 7; ++l) {
        for(int al = -1; al <= 1; ++al) {
            if((natural_pc[l] + al + 12) % 12 != pc) continue;
            int cost = (al == key_alter(fifths, l)) ? 0 : (al == 0 ? 2 : 3);
            if(cost < best) {best = cost; best_letter = l; best_alter = al;}
        }
    }
    *letter = best_letter;
    *alter = best_alter;
}

static int spelled_step(uint8_t pitch, uint8_t letter, int alter) {
    int octave = ((int)pitch - (int)natural_pc[letter] - alter) / 12;
    /* Integer division is exact for valid spellings. */
    return (octave - 5) * 7 + (int)letter - 2;
}

static void draw_key_signature(Canvas* c, int8_t fifths) {
    int count = fifths < 0 ? -fifths : fifths;
    for(int i = 0; i < count; ++i) {
        int step = fifths > 0 ? sharp_steps[i] : flat_steps[i];
        draw_accidental(c, 23 + i * 5, step_y(step), fifths > 0 ? 1 : -1);
    }
}

static void draw_rest(Canvas* c, int x, uint8_t d) {
    if(d <= 1) {
        /* Whole rest hangs below line 4; half rest sits on line 3. */
        canvas_draw_box(c, x - 4, d == 0 ? 26 : 30, 8, 3);
    } else if(d == 2) {
        canvas_draw_line(c, x + 1, 24, x - 2, 34);
        canvas_draw_line(c, x - 2, 34, x + 3, 38);
        canvas_draw_line(c, x + 3, 38, x - 2, 43);
    } else {
        canvas_draw_line(c, x, 23, x, 40);
        canvas_draw_circle(c, x + 3, 27, 2);
        if(d == 4) canvas_draw_circle(c, x + 3, 34, 2);
    }
}

/* Barline styles: 0 single, 1 double, 2 final, 3 dashed, 4 dotted,
 * 5 heavy, 6 invisible. x is the centre of the line. */
static void draw_barline(Canvas* c, int x, uint8_t style) {
    switch(style) {
    case 1:
        canvas_draw_line(c, x - 1, 19, x - 1, STAFF_BOTTOM);
        canvas_draw_line(c, x + 2, 19, x + 2, STAFF_BOTTOM);
        break;
    case 2:
        canvas_draw_line(c, x - 2, 19, x - 2, STAFF_BOTTOM);
        canvas_draw_box(c, x + 1, 19, 3, STAFF_BOTTOM - 19 + 1);
        break;
    case 3:
        for(int y = 19; y <= STAFF_BOTTOM; y += 4)
            canvas_draw_line(c, x, y, x, y + 1 > STAFF_BOTTOM ? STAFF_BOTTOM : y + 1);
        break;
    case 4:
        for(int y = 19; y <= STAFF_BOTTOM; y += 3) canvas_draw_dot(c, x, y);
        break;
    case 5:
        canvas_draw_box(c, x - 1, 19, 3, STAFF_BOTTOM - 19 + 1);
        break;
    case 6:
        break;
    default:
        canvas_draw_line(c, x, 19, x, STAFF_BOTTOM);
        break;
    }
}

static int stem_end(int8_t fifths, Note n, bool down) {
    uint8_t letter; int alter;
    spell_pitch(fifths, n.pitch, &letter, &alter);
    int y = step_y(spelled_step(n.pitch, letter, alter));
    return y + (down ? 17 : -17);
}

static bool stem_down(int8_t fifths, Note n) {
    uint8_t letter; int alter;
    spell_pitch(fifths, n.pitch, &letter, &alter);
    return spelled_step(n.pitch, letter, alter) >= 4;
}

static void draw_note(Canvas* c, int8_t fifths, int x, Note n, bool selected, bool beamed) {
    if(n.rest) {
        draw_rest(c, x, n.duration);
        if(n.dotted) canvas_draw_disc(c, x + 7, 33, 1);
        if(selected) canvas_draw_frame(c, x - 6, 20, 13, 26);
        return;
    }
    uint8_t letter; int alter;
    spell_pitch(fifths, n.pitch, &letter, &alter);
    int step = spelled_step(n.pitch, letter, alter);
    int y = step_y(step);
    for(int s = -2; s >= step && s >= -14; s -= 2) canvas_draw_line(c, x - 6, step_y(s), x + 6, step_y(s));
    for(int s = 10; s <= step && s <= 22; s += 2) canvas_draw_line(c, x - 6, step_y(s), x + 6, step_y(s));
    if(alter != key_alter(fifths, letter)) draw_accidental(c, x - 8, y, alter);
    if(n.duration <= 1) canvas_draw_circle(c, x, y, 3);
    else canvas_draw_disc(c, x, y, 3);
    for(uint8_t k = 0; k < n.chord_count && k < 3; ++k) {
        uint8_t cl; int ca;
        spell_pitch(fifths, n.chord[k], &cl, &ca);
        int cs = spelled_step(n.chord[k], cl, ca), cy = step_y(cs);
        for(int ls = -2; ls >= cs && ls >= -14; ls -= 2)
            canvas_draw_line(c, x - 6, step_y(ls), x + 6, step_y(ls));
        for(int ls = 10; ls <= cs && ls <= 22; ls += 2)
            canvas_draw_line(c, x - 6, step_y(ls), x + 6, step_y(ls));
        if(ca != key_alter(fifths, cl)) draw_accidental(c, x - 8, cy, ca);
        if(n.duration <= 1) canvas_draw_circle(c, x, cy, 3);
        else canvas_draw_disc(c, x, cy, 3);
    }
    if(n.dotted) canvas_draw_disc(c, x + 6, y - 2, 1);
    if(n.duration != 0) {
        bool down = stem_down(fifths, n);
        int sx = x + (down ? -3 : 3);
        int end = stem_end(fifths, n, down);
        canvas_draw_line(c, sx, y, sx, end);
        if(n.duration >= 3 && !beamed) {
            int d = down ? -1 : 1;
            canvas_draw_line(c, sx, end, sx + 5, end + d * 5);
            canvas_draw_line(c, sx + 5, end + d * 5, sx + 3, end + d * 9);
            if(n.duration == 4) {
                canvas_draw_line(c, sx, end - d * 5, sx + 5, end);
                canvas_draw_line(c, sx + 5, end, sx + 3, end + d * 4);
            }
        }
    }
    /* Staccato: a 2x2 dot (a radius-1 disc looks like a plus on this
     * display). Clear any stave line behind it first. */
    if(n.staccato) {
        int dot_y = y < 28 ? y + 9 : y - 9;
        canvas_set_color(c, ColorWhite);
        canvas_draw_box(c, x - 2, dot_y - 2, 5, 5);
        canvas_set_color(c, ColorBlack);
        canvas_draw_box(c, x - 1, dot_y - 1, 2, 2);
    }
    /* Marks above the note are stacked upward, nearest first, so several
     * can be combined without overlapping. */
    int up = (n.staccato && y >= 28) ? 14 : 9;
    if(n.accent) {
        canvas_draw_line(c, x - 4, y - up, x + 2, y - up - 2);
        canvas_draw_line(c, x + 2, y - up - 2, x - 4, y - up - 4);
        up += 6;
    }
    if(n.marcato) {
        canvas_draw_line(c, x - 3, y - up, x, y - up - 4);
        canvas_draw_line(c, x, y - up - 4, x + 3, y - up);
        up += 7;
    }
    if(n.tenuto) {
        canvas_draw_line(c, x - 3, y - up - 1, x + 3, y - up - 1);
        up += 4;
    }
    if(n.staccatissimo) {
        canvas_draw_line(c, x - 1, y - up - 4, x, y - up);
        canvas_draw_line(c, x + 1, y - up - 4, x, y - up);
        canvas_draw_line(c, x, y - up - 4, x, y - up);
        up += 6;
    }
    if(n.ornament) {
        int yb = y - up;
        if(n.ornament == 1) {
            canvas_draw_str(c, x - 5, yb, "tr");
        } else if(n.ornament == 4) {
            canvas_draw_line(c, x - 4, yb - 1, x - 3, yb - 4);
            canvas_draw_line(c, x - 3, yb - 4, x - 1, yb - 4);
            canvas_draw_line(c, x - 1, yb - 4, x + 1, yb - 1);
            canvas_draw_line(c, x + 1, yb - 1, x + 3, yb);
            canvas_draw_line(c, x + 3, yb, x + 4, yb - 1);
        } else {
            /* Zigzag; the mordent (2) also has a vertical stroke. */
            canvas_draw_line(c, x - 4, yb, x - 2, yb - 3);
            canvas_draw_line(c, x - 2, yb - 3, x, yb);
            canvas_draw_line(c, x, yb, x + 2, yb - 3);
            canvas_draw_line(c, x + 2, yb - 3, x + 4, yb);
            if(n.ornament == 2) canvas_draw_line(c, x, yb - 4, x, yb + 1);
        }
        up += 8;
    }
    /* Fermata: open arch over a separate dot, above everything else. */
    if(n.fermata) {
        int fy = y - up - 3;
        canvas_draw_line(c, x - 6, fy + 2, x - 5, fy - 1);
        canvas_draw_line(c, x - 5, fy - 1, x - 2, fy - 3);
        canvas_draw_line(c, x - 2, fy - 3, x + 2, fy - 3);
        canvas_draw_line(c, x + 2, fy - 3, x + 5, fy - 1);
        canvas_draw_line(c, x + 5, fy - 1, x + 6, fy + 2);
        canvas_draw_box(c, x, fy, 2, 2);
    }
    /* Breath mark (comma) and caesura (double slash) sit after the note,
     * just above the staff. */
    if(n.aftermark == 1) {
        canvas_draw_dot(c, x + 9, 15);
        canvas_draw_line(c, x + 9, 15, x + 8, 18);
    } else if(n.aftermark == 2) {
        canvas_draw_line(c, x + 8, 22, x + 10, 14);
        canvas_draw_line(c, x + 11, 22, x + 13, 14);
    }
    if(selected) canvas_draw_frame(c, x - 5, y - 5, 11, 11);
}

static uint8_t tick_before(const AppState* a, uint16_t index) {
    uint8_t cap = measure_ticks(a), used = 0;
    for(uint16_t i = 0; i < index; ++i) {
        uint8_t ticks = note_ticks(a->notes[i]);
        if(used && used + ticks > cap) used = 0;
        used += ticks;
        if(used >= cap) used = 0;
    }
    return used;
}

static uint8_t group_ticks(const AppState* a) {return a->beat_type == 8 ? 12 : 8;}

/* True if a barline falls after note i of the active voice: the measure
 * fills, the next note would overflow it, or it is the last note. */
static bool measure_ends_after(const AppState* a, uint16_t i) {
    if(i >= a->count) return false;
    if(i + 1 >= a->count) return true;
    uint8_t cap = measure_ticks(a), used = tick_before(a, i);
    uint8_t t = note_ticks(a->notes[i]);
    if(used && used + t > cap) used = 0;
    used += t;
    if(used >= cap) return true;
    uint8_t next = note_ticks(a->notes[i + 1]);
    return used && used + next > cap;
}

static uint16_t measure_last_index(const AppState* a, uint16_t i) {
    while(i + 1 < a->count && !measure_ends_after(a, i)) ++i;
    return i;
}

static bool can_beam(const AppState* a, uint16_t left) {
    if(left + 1 >= a->count) return false;
    Note n = a->notes[left], next = a->notes[left + 1];
    if(n.rest || next.rest || n.duration < 3 || next.duration < 3) return false;
    uint8_t used = tick_before(a, left), ticks = note_ticks(n);
    uint8_t cap = measure_ticks(a), group = group_ticks(a);
    if(used + ticks >= cap || used + ticks + note_ticks(next) > cap) return false;
    if(used + ticks > group * ((used / group) + 1)) return false;
    return (used + ticks) / group == used / group;
}

static void draw_beams(Canvas* c, const AppState* a, uint16_t start, uint16_t end, int x0, int spacing) {
    for(uint16_t i = start; i + 1 < end; ++i) {
        if(!can_beam(a, i)) continue;
        Note n = a->notes[i], next = a->notes[i + 1];
        int8_t key1, key2; bool minor1, minor2;
        active_key(a, a->notes, a->count, i, &key1, &minor1);
        active_key(a, a->notes, a->count, i + 1, &key2, &minor2);
        bool down = stem_down(key1, n);
        if(stem_down(key2, next) != down) continue;
        int x1 = x0 + (i - start) * spacing + (down ? -3 : 3);
        int x2 = x0 + (i + 1 - start) * spacing + (down ? -3 : 3);
        int y1 = stem_end(key1, n, down), y2 = stem_end(key2, next, down);
        /* Both stem endpoints are joined. A second beam is used for two sixteenths. */
        canvas_draw_line(c, x1, y1, x2, y2);
        canvas_draw_line(c, x1, y1 + 1, x2, y2 + 1);
        canvas_draw_line(c, x1, y1 + 2, x2, y2 + 2);
        if(n.duration == 4 && next.duration == 4) {
            int offset = down ? -5 : 5;
            canvas_draw_line(c, x1, y1 + offset, x2, y2 + offset);
            canvas_draw_line(c, x1, y1 + offset + 1, x2, y2 + offset + 1);
        } else if(n.duration == 4 || next.duration == 4) {
            /* Partial secondary beam for mixed eighth/sixteenth grouping. */
            int offset = down ? -5 : 5;
            if(n.duration == 4) {
                canvas_draw_line(c, x1, y1 + offset, x1 + 6, y1 + offset + (y2 - y1) * 6 / spacing);
            } else {
                canvas_draw_line(c, x2 - 6, y2 + offset - (y2 - y1) * 6 / spacing, x2, y2 + offset);
            }
        }
    }
}

/* Combined score: merge both voices by their independent note onsets. */
static void draw_combined_events(Canvas* c, AppState* a, int x0, uint8_t visible) {
    uint32_t focus = 0;
    for(uint16_t i = 0; i < a->selected && i < a->count; ++i)
        focus += note_ticks(a->notes[i]);
    uint16_t i = 0, j = 0;
    uint32_t t1 = 0, t2 = 0;
    uint16_t slot = 0, focus_slot = 0;
    /* Find the selected event in the merged onset stream. */
    while((i < a->count || j < a->other_count) && slot < 2 * MAX_NOTES) {
        uint32_t t = i < a->count && (j >= a->other_count || t1 <= t2) ? t1 : t2;
        if(t >= focus) {focus_slot = slot; break;}
        if(i < a->count && t1 == t) {t1 += note_ticks(a->notes[i]); ++i;}
        if(j < a->other_count && t2 == t) {t2 += note_ticks(a->other_notes[j]); ++j;}
        ++slot;
    }
    uint16_t first = focus_slot > 2 ? focus_slot - 2 : 0;
    i = j = slot = 0; t1 = t2 = 0;
    uint8_t shown = 0;
    uint32_t cap = measure_ticks(a);
    while((i < a->count || j < a->other_count) && shown < visible) {
        uint32_t t = i < a->count && (j >= a->other_count || t1 <= t2) ? t1 : t2;
        bool has1 = i < a->count && t1 == t;
        bool has2 = j < a->other_count && t2 == t;
        if(slot >= first) {
            int x = x0 + shown * 16;
            if(t && t % cap == 0) draw_barline(c, x - 8, (has1 && i > 0) ? a->notes[i - 1].barline : 0);
            int8_t k1, k2; bool m1, m2;
            active_key(a, a->notes, a->count, i, &k1, &m1);
            active_key(a, a->other_notes, a->other_count, j, &k2, &m2);
            if(has2) draw_note(c, k2, x + (has1 ? 2 : 0), a->other_notes[j], false, false);
            if(has1) draw_note(c, k1, x - (has2 ? 2 : 0), a->notes[i], i == a->selected, false);
            ++shown;
        }
        if(has1) {t1 += note_ticks(a->notes[i]); ++i;}
        if(has2) {t2 += note_ticks(a->other_notes[j]); ++j;}
        ++slot;
    }
}

static void draw_callback(Canvas* c, void* ctx) {
    AppState* a = ctx;
    furi_mutex_acquire(a->mutex, FuriWaitForever);
    canvas_clear(c);
    canvas_set_font(c, FontPrimary);
    if(a->screen == ScreenScore) {
        char header[40];
        int8_t display_fifths; bool display_minor;
        active_key(a, a->notes, a->count, a->selected, &display_fifths, &display_minor);
        snprintf(header, sizeof(header), "V%u%s %u/%u %s%s %u", (unsigned)a->voice + 1, a->combined ? "+" : "", a->beats, a->beat_type,
            display_minor ? minor_names[display_fifths + 7] : major_names[display_fifths + 7],
            display_minor ? "m" : "", (unsigned)a->tempo);
        canvas_set_font(c, FontSecondary);
        canvas_draw_str(c, 2, 9, header);
        draw_staff(c);
        draw_key_signature(c, display_fifths);
        if(a->combined) {
            int sig_count = display_fifths < 0 ? -display_fifths : display_fifths;
            uint8_t visible = sig_count >= 5 ? 3 : (sig_count >= 3 ? 4 : VISIBLE_NOTES);
            draw_combined_events(c, a, 38 + sig_count * 5, visible);
        } else {
            uint16_t start = a->selected > 2 ? a->selected - 2 : 0;
            int sig_count = display_fifths < 0 ? -display_fifths : display_fifths;
            uint8_t visible = sig_count >= 5 ? 3 : (sig_count >= 3 ? 4 : VISIBLE_NOTES);
            uint16_t end = start + visible;
            if(end > a->count) end = a->count;
            uint8_t used = tick_before(a, start), cap = measure_ticks(a);
            int x0 = 38 + sig_count * 5;
            int spacing = 16;
            int previous_x = -1, previous_y = 0;
            uint16_t previous_i = 0;
            for(uint16_t i = start; i < end; ++i) {
                int x = x0 + (i - start) * spacing;
                uint8_t ticks = note_ticks(a->notes[i]);
                if(used && used + ticks > cap) {draw_barline(c, x - 8, i ? a->notes[i - 1].barline : 0); used = 0;}
                int8_t current_key, prev_key, next_key; bool current_minor, dummy_minor;
                active_key(a, a->notes, a->count, i, &current_key, &current_minor);
                active_key(a, a->notes, a->count, i ? i - 1 : 0, &prev_key, &dummy_minor);
                active_key(a, a->notes, a->count, i + 1, &next_key, &dummy_minor);
                bool beamed = (i > 0 && can_beam(a, i - 1) && stem_down(prev_key, a->notes[i - 1]) == stem_down(current_key, a->notes[i])) ||
                              (can_beam(a, i) && stem_down(current_key, a->notes[i]) == stem_down(next_key, a->notes[i + 1]));
                draw_note(c, current_key, x, a->notes[i], i == a->selected, beamed);
                if(a->notes[i].repeat_start) {canvas_draw_line(c,x-9,19,x-9,47);canvas_draw_disc(c,x-5,30,1);canvas_draw_disc(c,x-5,37,1);}
                if(a->notes[i].repeat_end) {canvas_draw_line(c,x+8,19,x+8,47);canvas_draw_disc(c,x+4,30,1);canvas_draw_disc(c,x+4,37,1);}
                if(a->notes[i].key_change) {
                    /* Indicate cancelled sharps/flats at the change boundary. */
                    int8_t old_key = i ? prev_key : a->fifths;
                    int cancelled = 0;
                    for(uint8_t letter = 0; letter < 7; ++letter) {
                        if(key_alter(old_key, letter) && !key_alter(current_key, letter)) {
                            draw_accidental(c, x - 9 - cancelled * 5,
                                            step_y(letter == 5 ? 3 : letter == 1 ? 6 : 5), 0);
                            ++cancelled;
                            if(cancelled == 2) break;
                        }
                    }
                    canvas_draw_str(c,x-5,18,"K");
                }
                if(previous_x >= 0 && !a->notes[i].rest && !a->notes[previous_i].rest) {
                    if(a->notes[previous_i].slur == 1 || a->notes[i].slur == 2) {
                        canvas_draw_line(c, previous_x, 13, (previous_x + x) / 2, 11);
                        canvas_draw_line(c, (previous_x + x) / 2, 11, x, 13);
                    }
                    if(a->notes[previous_i].tie == 1 && a->notes[i].tie == 2 && a->notes[previous_i].pitch == a->notes[i].pitch) {
                        int y = previous_y + 7;
                        canvas_draw_line(c, previous_x + 3, y, (previous_x + x) / 2, y + 2);
                        canvas_draw_line(c, (previous_x + x) / 2, y + 2, x - 3, y);
                    }
                }
                previous_x = x; previous_y = note_y(a->notes[i].pitch); previous_i = i;
                used += ticks;
                bool bar_after = used >= cap;
                if(bar_after) used = 0;
                if(i + 1 == a->count) {
                    if(a->notes[i].barline) draw_barline(c, x + 8, a->notes[i].barline);
                } else if(bar_after && i + 1 < end) {
                    draw_barline(c, x + 8, a->notes[i].barline);
                }
            }
            draw_beams(c, a, start, end, x0, spacing);
        }
        canvas_set_font(c, FontSecondary);
        char status[42];
        if(a->playing && a->paused) snprintf(status, sizeof(status), "OK:Resume Back:Stop");
        else if(a->playing) snprintf(status, sizeof(status), "OK:Pause Back:Stop");
        else if(a->notice[0]) snprintf(status, sizeof(status), "%s", a->notice);
        else if(a->count) {
            Note n = a->notes[a->selected]; char label[8];
            if(n.rest) snprintf(label, sizeof(label), "Rest"); else pitch_label(n.pitch, label, sizeof(label));
            snprintf(status, sizeof(status), "%s %s %u/%u", label, durations[n.duration], (unsigned)(a->selected + 1), (unsigned)a->count);
        } else snprintf(status, sizeof(status), "OK:Add Hold OK:Tools");
        canvas_draw_str(c, 2, 63, status);
    } else if(a->screen == ScreenAbout) {
        /* Lines are kept to 22 characters or fewer so they fit 128 px. */
        static const char* const help[][5] = {
            {"Bard's Score v2.1", "Notation for Flipper", "By Lachlan The Bard", "Up/Down: change page", "Back: return to tools"},
            {"1/8 - Editing", "OK: add note", "L/R: select note", "Up/Down: semitone", "Back del, hold: exit"},
            {"2/8 - Note entry", "OK: choose pitch", "Up/Down: adjust pitch", "OK: choose duration", "OK: insert note"},
            {"3/8 - Score tools", "Hold OK: open tools", "Up/Down: pick category", "OK: open / apply", "Back: up one level"},
            {"4/8 - Notation", "Rests, time, keys", "Marks and ornaments", "Barline styles", "Repeats, dots, slurs"},
            {"5/8 - Chords", "Select a note first", "Tools > Add chord tone", "Pick pitch, press OK", "Up to 4 pitches"},
            {"6/8 - Voices", "Tools > Switch voice", "Edit each voice alone", "Combined/solo toggle", "Play both in tools"},
            {"7/8 - Files / XML", "Tools: Save/Load/XML", "BSC5 native format", "XML: 1 or 2 staves", "Back up melody.bsc"},
            {"8/8 - Playback", "Play all or from here", "OK: pause / resume", "Back stop, hold: exit", "Chords are arpeggiated"},
        };
        uint8_t page = a->about_page;
        canvas_draw_str(c, 2, 11, help[page][0]);
        canvas_set_font(c, FontSecondary);
        for(uint8_t i = 1; i < 5; ++i) canvas_draw_str(c, 2, 11 + i * 11, help[page][i]);
    } else if(a->screen == ScreenMenu) {
        canvas_draw_str(c, 2, 11, "Score tools");
        canvas_set_font(c, FontSecondary);
        char pos[12];
        snprintf(pos, sizeof(pos), "%u/%u", (unsigned)a->menu + 1, (unsigned)MENU_CAT_COUNT);
        canvas_draw_str(c, 98, 11, pos);
        for(uint8_t i = a->menu_top; i < MENU_CAT_COUNT && i < a->menu_top + 4; ++i) {
            char line[28];
            snprintf(line, sizeof(line), "%c %s", a->menu == i ? '>' : ' ', menu_cats[i].name);
            canvas_draw_str(c, 3, 23 + (i - a->menu_top) * 12, line);
        }
    } else if(a->screen == ScreenSubmenu) {
        const MenuCat* cat = &menu_cats[a->menu_cat];
        canvas_draw_str(c, 2, 11, cat->name);
        canvas_set_font(c, FontSecondary);
        char pos[12];
        snprintf(pos, sizeof(pos), "%u/%u", (unsigned)a->sub + 1, (unsigned)cat->count);
        canvas_draw_str(c, 98, 11, pos);
        for(uint8_t i = a->sub_top; i < cat->count && i < a->sub_top + 4; ++i) {
            uint8_t e = cat->items[i];
            int st = entry_state(a, e);
            char line[32];
            snprintf(line, sizeof(line), "%c %s%s", a->sub == i ? '>' : ' ',
                st < 0 ? "" : (st ? "[x] " : "[ ] "), entry_name(e));
            canvas_draw_str(c, 3, 23 + (i - a->sub_top) * 12, line);
        }
    } else if(a->screen == ScreenBarline) {
        canvas_draw_str(c, 2, 11, "Barline style");
        canvas_set_font(c, FontSecondary);
        canvas_draw_str(c, 8, 33, barline_names[a->barline_choice % BARLINE_COUNT]);
        canvas_draw_str(c, 2, 61, "Up/Down OK:Set");
        draw_barline(c, 112, a->barline_choice);
    } else {
        const char* title = a->screen == ScreenPitch ? (a->chord_edit ? "Add chord pitch" : "Choose pitch") :
            a->screen == ScreenDuration ? (a->adding_rest ? "Rest length" : "Note length") :
            a->screen == ScreenTempo ? "Playback tempo" : a->screen == ScreenKey ? "Key signature" : "Time signature";
        canvas_draw_str(c, 2, 11, title);
        canvas_set_font(c, FontSecondary);
        char line[32];
        if(a->screen == ScreenPitch) {pitch_label(a->chord_edit ? a->chord_pitch : a->pitch, line, sizeof(line)); canvas_draw_str(c, 8, 33, line); canvas_draw_str(c, 2, 61, "Up/Down OK:Next");}
        else if(a->screen == ScreenDuration) {canvas_draw_str(c, 8, 33, durations[a->duration]); canvas_draw_str(c, 2, 61, "Up/Down OK:Insert");}
        else if(a->screen == ScreenTempo) {snprintf(line, sizeof(line), "%u BPM", (unsigned)a->tempo); canvas_draw_str(c, 8, 33, line); canvas_draw_str(c, 2, 61, "Up/Down +/-5 OK:Set");}
        else if(a->screen == ScreenKey) {
            snprintf(line, sizeof(line), "%s %s (%d)", (a->inserting_key ? a->edit_minor : a->minor) ? minor_names[(a->inserting_key ? a->edit_fifths : a->fifths) + 7] : major_names[(a->inserting_key ? a->edit_fifths : a->fifths) + 7], (a->inserting_key ? a->edit_minor : a->minor) ? "minor" : "major", (int)(a->inserting_key ? a->edit_fifths : a->fifths));
            canvas_draw_str(c, 3, 33, line);
            canvas_draw_str(c, 2, 61, "Up/Dn:key L/R:mode OK");
        } else {snprintf(line, sizeof(line), "%u/%u", time_beats[a->time_choice], time_denoms[a->time_choice]); canvas_draw_str(c, 8, 33, line); canvas_draw_str(c, 2, 61, "Up/Down OK:Set");}
    }
    furi_mutex_release(a->mutex);
}

static void input_callback(InputEvent* e, void* ctx) {
    AppState* a = ctx;
    if(e->type == InputTypeShort || e->type == InputTypeLong || e->type == InputTypeRepeat)
        furi_message_queue_put(a->queue, e, 0);
}

static bool file_write_text(File* f, const char* s) {size_t n = strlen(s); return storage_file_write(f, s, n) == n;}

static bool xw(File* f, bool ok, bool cond, const char* text) {
    return ok && (!cond || file_write_text(f, text));
}

/* Element order follows the MusicXML schema: pitch, duration, tie, voice,
 * type, dot, then a single <notations> block. Chord tones carry no
 * ties, slurs, ornaments or articulations of their own. The notations
 * are written piece by piece so no large buffer is needed on the stack. */
static bool write_xml_note(File* f, int8_t fifths, Note n, uint8_t voice, bool chord, uint8_t pitch) {
    char b[200];
    uint8_t ticks = note_ticks(n);
    if(n.rest) {
        snprintf(b, sizeof(b), "<note><rest/><duration>%u</duration><voice>%u</voice><type>%s</type>%s</note>\n",
            ticks, voice, xml_types[n.duration], n.dotted ? "<dot/>" : "");
        return file_write_text(f, b);
    }
    uint8_t letter; int alter;
    spell_pitch(fifths, pitch, &letter, &alter);
    int octave = ((int)pitch - (int)natural_pc[letter] - alter) / 12 - 1;
    bool tie_start = !chord && n.tie == 1;
    bool tie_stop = !chord && n.tie == 2;
    snprintf(b, sizeof(b),
        "<note>%s<pitch><step>%s</step><alter>%d</alter><octave>%d</octave></pitch><duration>%u</duration>%s%s<voice>%u</voice><type>%s</type>%s",
        chord ? "<chord/>" : "", step_names[letter], alter, octave, ticks,
        tie_stop ? "<tie type=\"stop\"/>" : "", tie_start ? "<tie type=\"start\"/>" : "",
        voice, xml_types[n.duration], n.dotted ? "<dot/>" : "");
    bool ok = file_write_text(f, b);
    bool nc = !chord;
    bool art = n.staccato || n.accent || n.tenuto || n.staccatissimo || n.marcato || n.aftermark;
    bool any = nc && (n.tie || n.slur || n.ornament || art || n.fermata);
    ok = xw(f, ok, any, "<notations>");
    ok = xw(f, ok, nc && n.tie == 1, "<tied type=\"start\"/>");
    ok = xw(f, ok, nc && n.tie == 2, "<tied type=\"stop\"/>");
    ok = xw(f, ok, nc && n.slur == 1, "<slur number=\"1\" type=\"start\"/>");
    ok = xw(f, ok, nc && n.slur == 2, "<slur number=\"1\" type=\"stop\"/>");
    ok = xw(f, ok, nc && n.ornament, "<ornaments>");
    ok = xw(f, ok, nc && n.ornament == 1, "<trill-mark/>");
    ok = xw(f, ok, nc && n.ornament == 2, "<mordent/>");
    ok = xw(f, ok, nc && n.ornament == 3, "<inverted-mordent/>");
    ok = xw(f, ok, nc && n.ornament == 4, "<turn/>");
    ok = xw(f, ok, nc && n.ornament, "</ornaments>");
    ok = xw(f, ok, nc && art, "<articulations>");
    ok = xw(f, ok, nc && n.accent, "<accent/>");
    ok = xw(f, ok, nc && n.marcato, "<strong-accent/>");
    ok = xw(f, ok, nc && n.staccato, "<staccato/>");
    ok = xw(f, ok, nc && n.tenuto, "<tenuto/>");
    ok = xw(f, ok, nc && n.staccatissimo, "<staccatissimo/>");
    ok = xw(f, ok, nc && n.aftermark == 1, "<breath-mark/>");
    ok = xw(f, ok, nc && n.aftermark == 2, "<caesura/>");
    ok = xw(f, ok, nc && art, "</articulations>");
    ok = xw(f, ok, nc && n.fermata, "<fermata/>");
    ok = xw(f, ok, any, "</notations>");
    ok = xw(f, ok, true, "</note>\n");
    return ok;
}

static bool write_xml_key(File* f, int8_t fifths, bool minor) {
    char b[100];
    snprintf(b, sizeof(b), "<attributes><key><fifths>%d</fifths><mode>%s</mode></key></attributes>\n",
             (int)fifths, minor ? "minor" : "major");
    return file_write_text(f, b);
}

/* Shared by both XML layouts. The buffer is only live while this runs,
 * never at the same time as the note buffers. */
/* Left barline carries a forward repeat; right barline carries the style
 * and/or a backward repeat. Plain single barlines are left implicit. */
static bool write_xml_barline(File* f, bool left, uint8_t style, bool repeat) {
    if(!repeat && style == 0) return true;
    const char* bs = repeat ? (left ? "heavy-light" : "light-heavy") : xml_bar_styles[style % BARLINE_COUNT];
    bool ok = file_write_text(f, left ? "<barline location=\"left\">" : "<barline location=\"right\">");
    ok = xw(f, ok, true, "<bar-style>");
    ok = xw(f, ok, true, bs);
    ok = xw(f, ok, true, "</bar-style>");
    ok = xw(f, ok, repeat, left ? "<repeat direction=\"forward\"/>" : "<repeat direction=\"backward\"/>");
    ok = xw(f, ok, true, "</barline>\n");
    return ok;
}

static bool write_xml_attributes(File* f, const AppState* a) {
    char b[208];
    snprintf(b, sizeof(b), "<attributes><divisions>8</divisions><key><fifths>%d</fifths><mode>%s</mode></key><time><beats>%u</beats><beat-type>%u</beat-type></time><clef><sign>G</sign><line>2</line></clef></attributes>\n", (int)a->fifths, a->minor ? "minor" : "major", a->beats, a->beat_type);
    return file_write_text(f, b);
}

static bool write_xml_voice(File* f, AppState* a, Note* notes, uint16_t count, uint8_t voice, bool attributes) {
    char b[48]; uint32_t position = 0;
    uint8_t cap = measure_ticks(a);
    uint16_t measure = 1;
    bool ok = true;
    /* Each voice is laid out on a tick grid, preserving independent rhythms. */
    uint16_t i = 0;
    while((i < count || (attributes && measure == 1)) && ok) {
        snprintf(b, sizeof(b), "<measure number=\"%u\">\n", measure++);
        uint32_t limit = (uint32_t)(measure - 1) * cap;
        /* Dry run: which notes land in this measure, and do they carry repeats? */
        uint16_t first = i, j = i;
        uint32_t pos = position;
        bool rs = false, re = false;
        while(j < count && pos < limit) {
            uint8_t t = note_ticks(notes[j]);
            if(pos + t > limit && pos != limit - cap) break;
            if(notes[j].repeat_start) rs = true;
            if(notes[j].repeat_end) re = true;
            pos += t; ++j;
        }
        ok = file_write_text(f, b);
        if(attributes && measure == 2 && ok) ok = write_xml_attributes(f, a);
        if(ok) ok = write_xml_barline(f, true, 0, rs);
        while(i < j && ok) {
            Note n = notes[i];
            uint8_t ticks = note_ticks(n);
            int8_t key; bool minor;
            active_key(a, notes, count, i, &key, &minor);
            if(n.key_change) ok = write_xml_key(f, key, minor);
            if(ok) ok = write_xml_note(f, key, n, voice, false, n.pitch);
            for(uint8_t k = 0; k < n.chord_count && ok; ++k)
                ok = write_xml_note(f, key, n, voice, true, n.chord[k]);
            position += ticks; ++i;
        }
        if(ok) ok = write_xml_barline(f, false, j > first ? notes[j - 1].barline : 0, re);
        ok = ok && file_write_text(f, "</measure>\n");
        /* A note that does not fit moves to the next measure. */
        if(i < count && position < limit) position = limit;
    }
    return ok;
}

/* Notes that fit in one measure of a voice, starting at `from`. */
static uint16_t measure_span(const Note* notes, uint16_t from, uint16_t count, uint32_t cap, uint32_t* used_out) {
    uint32_t used = 0;
    uint16_t j = from;
    while(j < count && used < cap) {
        uint32_t t = note_ticks(notes[j]);
        if(used + t > cap && used != 0) break;
        used += t; ++j;
    }
    *used_out = used;
    return j;
}

/* One-staff MusicXML: two independent voices use <backup> per measure.
 * Measures keep being written until BOTH voices are fully consumed, so
 * notes pushed into later measures are never dropped. Barline style and
 * repeats are taken from voice 1 first, then voice 2. */
static bool write_xml_combined(File* f, AppState* a, Note* v1, uint16_t c1, Note* v2, uint16_t c2) {
    uint16_t idx[2] = {0, 0};
    Note* voices[2] = {v1, v2};
    uint16_t counts[2] = {c1, c2};
    uint32_t cap = measure_ticks(a);
    bool ok = true;
    char b[64];
    for(uint32_t measure = 0; (measure == 0 || idx[0] < c1 || idx[1] < c2) && ok; ++measure) {
        snprintf(b, sizeof(b), "<measure number=\"%lu\">\n", (unsigned long)(measure + 1));
        ok = file_write_text(f, b);
        if(measure == 0 && ok) ok = write_xml_attributes(f, a);
        uint32_t used[2] = {0, 0};
        uint16_t end[2] = {0, 0};
        bool rs = false, re = false;
        uint8_t style = 0;
        for(uint8_t v = 0; v < 2; ++v) {
            end[v] = measure_span(voices[v], idx[v], counts[v], cap, &used[v]);
            for(uint16_t k = idx[v]; k < end[v]; ++k) {
                if(voices[v][k].repeat_start) rs = true;
                if(voices[v][k].repeat_end) re = true;
            }
            if(end[v] > idx[v] && (v == 0 || style == 0)) style = voices[v][end[v] - 1].barline;
        }
        if(ok) ok = write_xml_barline(f, true, 0, rs);
        for(uint8_t v = 0; v < 2 && ok; ++v) {
            while(idx[v] < end[v] && ok) {
                Note n = voices[v][idx[v]];
                int8_t key; bool minor;
                active_key(a, voices[v], counts[v], idx[v], &key, &minor);
                if(n.key_change) ok = write_xml_key(f, key, minor);
                if(ok) ok = write_xml_note(f, key, n, v + 1, false, n.pitch);
                for(uint8_t k = 0; k < n.chord_count && ok; ++k)
                    ok = write_xml_note(f, key, n, v + 1, true, n.chord[k]);
                ++idx[v];
            }
            if(v == 0 && ok && used[0]) {
                snprintf(b, sizeof(b), "<backup><duration>%lu</duration></backup>\n", (unsigned long)used[0]);
                ok = file_write_text(f, b);
            }
        }
        if(ok) ok = write_xml_barline(f, false, style, re);
        if(ok) ok = file_write_text(f, "</measure>\n");
    }
    return ok;
}

/* Native BSC6 writer, split out so its line buffer is not on the stack
 * during XML export. */
static bool write_bsc(File* f, const AppState* a, const Note* v1, uint16_t c1, const Note* v2, uint16_t c2) {
    char b[128];
    bool ok = file_write_text(f, "BSC6\n");
    snprintf(b, sizeof(b), "%u %u %d %u %u %u %u\n", a->beats, a->beat_type, (int)a->fifths, (unsigned)a->minor, (unsigned)a->tempo, (unsigned)a->combined, (unsigned)a->separate_staves);
    if(ok) ok = file_write_text(f, b);
    for(uint8_t v = 0; v < 2 && ok; ++v) {
        const Note* ns = v ? v2 : v1; uint16_t count = v ? c2 : c1;
        snprintf(b, sizeof(b), "VOICE %u %u\n", (unsigned)(v + 1), (unsigned)count);
        ok = file_write_text(f, b);
        for(uint16_t i = 0; i < count && ok; ++i) {
            Note n = ns[i];
            snprintf(b, sizeof(b), "%u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %d %u %u %u %u %u %u %u\n", n.pitch, n.duration, n.rest, n.slur, n.tie, n.accent, n.staccato, n.fermata, n.chord_count, n.chord[0], n.chord[1], n.chord[2], n.dotted, n.repeat_start, n.repeat_end, n.key_change, (int)n.key_fifths, n.key_minor, n.tenuto, n.staccatissimo, n.marcato, n.ornament, n.aftermark, n.barline);
            ok = file_write_text(f, b);
        }
    }
    return ok;
}

/* Writes to a temporary file first, then swaps it into place, so a failed
 * write (SD removed, card full) cannot destroy the previous save. */
static bool write_score(AppState* a, bool xml) {
    const char* dst = xml ? XML_FILE : SCORE_FILE;
    const char* tmp = xml ? XML_TMP : SCORE_TMP;
    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_common_mkdir(storage, SCORE_DIR);
    File* f = storage_file_alloc(storage);
    bool ok = storage_file_open(f, tmp, FSAM_WRITE, FSOM_CREATE_ALWAYS);
    if(ok) {
        Note* v1 = a->voice == 0 ? a->notes : a->other_notes;
        Note* v2 = a->voice == 1 ? a->notes : a->other_notes;
        uint16_t c1 = a->voice == 0 ? a->count : a->other_count;
        uint16_t c2 = a->voice == 1 ? a->count : a->other_count;
        if(!xml) {
            ok = write_bsc(f, a, v1, c1, v2, c2);
        } else if(a->separate_staves) {
            ok = file_write_text(f, "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<score-partwise version=\"4.0\"><work><work-title>Bard's Score</work-title></work><movement-title>Bard's Score</movement-title>\n<part-list><score-part id=\"P1\"><part-name>Voice 1</part-name></score-part><score-part id=\"P2\"><part-name>Voice 2</part-name></score-part></part-list>\n");
            if(ok) ok = file_write_text(f, "<part id=\"P1\">\n");
            if(ok) ok = write_xml_voice(f, a, v1, c1, 1, true);
            if(ok) ok = file_write_text(f, "</part>\n<part id=\"P2\">\n");
            if(ok) ok = write_xml_voice(f, a, v2, c2, 1, true);
            if(ok) ok = file_write_text(f, "</part></score-partwise>\n");
        } else {
            ok = file_write_text(f, "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<score-partwise version=\"4.0\"><part-list><score-part id=\"P1\"><part-name>Two Voices</part-name></score-part></part-list><part id=\"P1\">\n");
            if(ok) ok = write_xml_combined(f, a, v1, c1, v2, c2);
            if(ok) ok = file_write_text(f, "</part></score-partwise>\n");
        }
        storage_file_close(f);
    }
    storage_file_free(f);
    if(ok) {
        storage_common_remove(storage, dst);
        ok = storage_common_rename(storage, tmp, dst) == FSE_OK;
    } else {
        storage_common_remove(storage, tmp);
    }
    furi_record_close(RECORD_STORAGE);
    return ok;
}

typedef struct {
    uint8_t beats, denom;
    int fifths;
    unsigned minor, tempo, combined, separate;
    uint16_t counts[2];
} ScoreHeader;

/* Streams the file in 64-byte chunks. With commit == false it only
 * validates and counts. With commit == true it stores notes straight into
 * the live arrays, so a second copy of the score is never held in RAM. */
static bool parse_score(File* f, AppState* a, bool commit, ScoreHeader* h) {
    h->beats = 4; h->denom = 4; h->fifths = 0;
    h->minor = 0; h->tempo = 120; h->combined = 0; h->separate = 1;
    h->counts[0] = 0; h->counts[1] = 0;
    int version = 0, voice = 0; bool valid = true;
    char line[128]; size_t pos = 0; uint16_t line_no = 0;
    char chunk[64]; size_t chunk_len = 0, chunk_pos = 0;
    while(valid) {
        char ch = 0; bool got = true;
        if(chunk_pos >= chunk_len) {
            chunk_len = storage_file_read(f, chunk, sizeof(chunk));
            chunk_pos = 0;
        }
        if(chunk_len == 0) got = false; else ch = chunk[chunk_pos++];
        if(!got) {if(pos == 0) break; ch = '\n';}
        if(ch == '\r') continue;
        if(ch == '\n') {
            line[pos] = 0;
            if(line_no == 0) {
                if(strcmp(line, "BSC1") == 0) version = 1;
                else if(strcmp(line, "BSC2") == 0) version = 2;
                else if(strcmp(line, "BSC3") == 0) version = 3;
                else if(strcmp(line, "BSC4") == 0) version = 4;
                else if(strcmp(line, "BSC5") == 0) version = 5;
                else if(strcmp(line, "BSC6") == 0) version = 6;
                else valid = false;
            } else if(version >= 2 && line_no == 1) {
                unsigned b = 0, d = 0;
                int matched = version >= 4 ? sscanf(line, "%u %u %d %u %u %u %u", &b, &d, &h->fifths, &h->minor, &h->tempo, &h->combined, &h->separate) :
                    version == 3 ? sscanf(line, "%u %u %d %u %u", &b, &d, &h->fifths, &h->minor, &h->tempo) : sscanf(line, "%u %u", &b, &d);
                if(matched != (version >= 4 ? 7 : version == 3 ? 5 : 2) || !(((b == 2 || b == 3 || b == 4) && d == 4) || (b == 6 && d == 8)) || h->fifths < -7 || h->fifths > 7 || h->minor > 1 || h->tempo < 40 || h->tempo > 240 || h->combined > 1 || h->separate > 1) valid = false;
                else {h->beats = b; h->denom = d;}
            } else if(version >= 4 && strncmp(line, "VOICE ", 6) == 0) {
                unsigned v = 0, count = 0;
                if(sscanf(line, "VOICE %u %u", &v, &count) != 2 || v < 1 || v > 2 || count > MAX_NOTES) valid = false;
                else voice = (int)v - 1;
            } else if(line[0]) {
                unsigned p=0,d=0,r=0,sl=0,t=0,ac=0,st=0,fe=0,cc=0,c0=0,c1=0,c2=0;
                unsigned dot=0,rs=0,re=0,kc=0,km=0; int kf=0;
                unsigned tn=0,sg=0,mc=0,orn=0,am=0,bl=0;
                int matched = version == 6 ? sscanf(line, "%u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %d %u %u %u %u %u %u %u", &p,&d,&r,&sl,&t,&ac,&st,&fe,&cc,&c0,&c1,&c2,&dot,&rs,&re,&kc,&kf,&km,&tn,&sg,&mc,&orn,&am,&bl) : version == 5 ? sscanf(line, "%u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %d %u", &p,&d,&r,&sl,&t,&ac,&st,&fe,&cc,&c0,&c1,&c2,&dot,&rs,&re,&kc,&kf,&km) : version == 4 ? sscanf(line, "%u %u %u %u %u %u %u %u %u %u %u %u", &p,&d,&r,&sl,&t,&ac,&st,&fe,&cc,&c0,&c1,&c2) :
                    version >= 2 ? sscanf(line, "%u %u %u %u %u %u %u %u", &p,&d,&r,&sl,&t,&ac,&st,&fe) : sscanf(line, "%u %u", &p,&d);
                if(matched != (version == 6 ? 24 : version == 5 ? 18 : version == 4 ? 12 : version >= 2 ? 8 : 2) || p < 48 || p > 95 || d > 4 || r > 1 || sl > 2 || t > 2 || ac > 1 || st > 1 || fe > 1 || cc > 3 || (cc && (c0 < 48 || c0 > 95)) || (cc > 1 && (c1 < 48 || c1 > 95)) || (cc > 2 && (c2 < 48 || c2 > 95)) || dot > 1 || rs > 1 || re > 1 || kc > 1 || kf < -7 || kf > 7 || km > 1 || tn > 1 || sg > 1 || mc > 1 || orn > 4 || am > 2 || bl > 6 || h->counts[voice] >= MAX_NOTES) valid = false;
                else {
                    if(commit) {
                        Note n = {.pitch=p,.duration=d,.rest=r,.slur=sl,.tie=t,.accent=ac,.staccato=st,.fermata=fe,.chord_count=cc,.chord={c0,c1,c2},.dotted=dot,.repeat_start=rs,.repeat_end=re,.key_change=kc,.key_fifths=kf,.key_minor=km,.tenuto=tn,.staccatissimo=sg,.marcato=mc,.ornament=orn,.aftermark=am,.barline=bl};
                        Note* dst = voice ? a->other_notes : a->notes;
                        dst[h->counts[voice]] = n;
                    }
                    h->counts[voice]++;
                }
            }
            ++line_no; pos = 0;
            if(!got) break;
        } else if(pos < sizeof(line) - 1) line[pos++] = ch;
        else valid = false;
    }
    return valid && line_no >= (version >= 2 ? 2 : 1);
}

/* Two passes over the file: the first validates without storing anything,
 * the second writes notes directly into the live arrays. The current score
 * is untouched if the file is invalid, and no temporary copy is needed. */
static bool load_score(AppState* a) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* f = storage_file_alloc(storage);
    bool ok = storage_file_open(f, SCORE_FILE, FSAM_READ, FSOM_OPEN_EXISTING);
    if(ok) {
        ScoreHeader h;
        ok = parse_score(f, a, false, &h);
        if(ok) ok = storage_file_seek(f, 0, true);
        if(ok) {
            memset(a->notes, 0, sizeof(a->notes));
            memset(a->other_notes, 0, sizeof(a->other_notes));
            ok = parse_score(f, a, true, &h);
            if(!ok) {
                /* Read error mid-load: leave a consistent empty score. */
                memset(a->notes, 0, sizeof(a->notes));
                memset(a->other_notes, 0, sizeof(a->other_notes));
                a->count = 0; a->other_count = 0; a->selected = 0; a->other_selected = 0; a->voice = 0;
            }
        }
        storage_file_close(f);
        if(ok) {
            a->count = h.counts[0]; a->other_count = h.counts[1];
            a->selected = 0; a->other_selected = 0; a->voice = 0;
            a->beats = h.beats; a->beat_type = h.denom; a->fifths = h.fifths; a->minor = h.minor; a->tempo = h.tempo;
            a->combined = h.combined; a->separate_staves = h.separate;
            a->undo_valid = false;
            for(uint8_t i = 0; i < 4; ++i) if(time_beats[i] == h.beats && time_denoms[i] == h.denom) a->time_choice = i;
        }
    }
    storage_file_free(f); furi_record_close(RECORD_STORAGE);
    return ok;
}

static void switch_voice(AppState* a) {
    /* The editor always operates on notes[]; swap in the other voice. */
    for(uint16_t i = 0; i < MAX_NOTES; ++i) {
        Note t = a->notes[i]; a->notes[i] = a->other_notes[i]; a->other_notes[i] = t;
    }
    uint16_t t = a->count; a->count = a->other_count; a->other_count = t;
    t = a->selected; a->selected = a->other_selected; a->other_selected = t;
    a->voice ^= 1;
}

static void chord_add(AppState* a) {
    if(!a->count || a->notes[a->selected].rest) {
        snprintf(a->notice, sizeof(a->notice), "Select a note first"); return;
    }
    Note* n = &a->notes[a->selected];
    if(n->chord_count >= 3) {snprintf(a->notice, sizeof(a->notice), "Chord full (4)"); return;}
    for(uint8_t i = 0; i < n->chord_count; ++i)
        if(n->chord[i] == a->chord_pitch) {snprintf(a->notice, sizeof(a->notice), "Duplicate chord pitch"); return;}
    if(n->pitch == a->chord_pitch) {snprintf(a->notice, sizeof(a->notice), "Duplicate chord pitch"); return;}
    n->chord[n->chord_count++] = a->chord_pitch;
    snprintf(a->notice, sizeof(a->notice), "Chord tone added");
}

/* Moves the root and every chord tone together, staying within 48..95. */
static void shift_pitch(AppState* a, int delta) {
    if(!a->count) return;
    Note* n = &a->notes[a->selected];
    if(n->rest) return;
    int np = (int)n->pitch + delta;
    if(np < 48 || np > 95) return;
    for(uint8_t k = 0; k < n->chord_count && k < 3; ++k) {
        int cp = (int)n->chord[k] + delta;
        if(cp < 48 || cp > 95) return;
    }
    n->pitch = (uint8_t)np;
    for(uint8_t k = 0; k < n->chord_count && k < 3; ++k) n->chord[k] = (uint8_t)((int)n->chord[k] + delta);
}

/* Remove the selected event and keep the cursor on a valid note.
 * The removed note is kept so it can be restored with "Undo delete".
 * Barlines and beaming are computed from the remaining score on redraw. */
static void delete_selected_note(AppState* a) {
    if(a->count == 0) return;
    uint16_t at = a->selected;
    a->undo_note = a->notes[at];
    a->undo_at = at;
    a->undo_voice = a->voice;
    a->undo_valid = true;
    if(at + 1 < a->count) {
        memmove(&a->notes[at], &a->notes[at + 1],
            (a->count - at - 1) * sizeof(Note));
    }
    a->count--;
    if(a->count == 0) a->selected = 0;
    else if(a->selected >= a->count) a->selected = a->count - 1;
    snprintf(a->notice, sizeof(a->notice), "Note deleted");
}

static void undo_delete(AppState* a) {
    if(!a->undo_valid || a->undo_voice != a->voice) {
        snprintf(a->notice, sizeof(a->notice), "Nothing to undo"); return;
    }
    if(a->count >= MAX_NOTES) {snprintf(a->notice, sizeof(a->notice), "Score full"); return;}
    uint16_t at = a->undo_at > a->count ? a->count : a->undo_at;
    if(at < a->count) memmove(&a->notes[at + 1], &a->notes[at], (a->count - at) * sizeof(Note));
    a->notes[at] = a->undo_note;
    a->count++;
    a->selected = at;
    a->undo_valid = false;
    snprintf(a->notice, sizeof(a->notice), "Delete undone");
}

static void insert_note(AppState* a) {
    if(a->count >= MAX_NOTES) {snprintf(a->notice, sizeof(a->notice), "Score full"); return;}
    uint16_t at = a->count ? a->selected + 1 : 0;
    if(at < a->count) memmove(&a->notes[at + 1], &a->notes[at], (a->count - at) * sizeof(Note));
    a->notes[at] = (Note){.pitch = a->pitch, .duration = a->duration, .rest = a->adding_rest};
    a->count++; a->selected = at;
}

static float pitch_hz(uint8_t midi) {
    static const float c4[] = {261.626f, 277.183f, 293.665f, 311.127f, 329.628f, 349.228f, 369.994f, 391.995f, 415.305f, 440.000f, 466.164f, 493.883f};
    float hz = c4[midi % 12]; int octave = (int)midi / 12 - 5;
    while(octave > 0) {hz *= 2.0f; octave--;}
    while(octave < 0) {hz *= 0.5f; octave++;}
    return hz;
}
static float note_volume(const Note* n) {return n->marcato ? 0.9f : (n->accent ? 0.8f : 0.5f);}

/* Semitone offset applied at `ms` into a note that carries an ornament.
 * Approximate: whole-tone neighbours, fixed segment lengths. */
static int ornament_offset(uint8_t orn, uint32_t ms) {
    uint32_t seg = ms / 45;
    switch(orn) {
    case 1: return ((ms / 60) & 1) ? 2 : 0;
    case 2: return seg == 1 ? -2 : 0;
    case 3: return seg == 1 ? 2 : 0;
    case 4: return seg == 0 ? 2 : (seg == 2 ? -2 : 0);
    default: return 0;
    }
}

static uint32_t voice_total_ticks(const Note* notes, uint16_t count) {
    uint32_t total = 0;
    for(uint16_t i = 0; i < count; ++i) total += note_ticks(notes[i]);
    return total;
}

static bool voice_at(const Note* notes, uint16_t count, uint32_t tick, Note* found) {
    uint32_t offset = 0;
    for(uint16_t i = 0; i < count; ++i) {
        uint32_t len = note_ticks(notes[i]);
        if(tick < offset + len) { *found = notes[i]; return !found->rest; }
        offset += len;
    }
    return false;
}

static void playback_stop(AppState* a) {
    if(a->sounding) {furi_hal_speaker_stop(); a->sounding = false;}
    if(a->speaker_owned) {furi_hal_speaker_release(); a->speaker_owned = false;}
    a->playing = false; a->play_both = false; a->paused = false;
}
static bool time_reached(uint32_t now, uint32_t deadline) {return (int32_t)(now - deadline) >= 0;}

/* Pause silences the speaker (it stays acquired) and remembers when it
 * happened. Resume shifts every deadline by the paused time so the note
 * picks up where it left off. */
static void playback_pause(AppState* a) {
    if(!a->playing || a->paused) return;
    a->paused = true;
    a->pause_tick = furi_get_tick();
    a->resume_sound = a->sounding;
    if(a->sounding) {furi_hal_speaker_stop(); a->sounding = false;}
}

static void playback_resume(AppState* a) {
    if(!a->playing || !a->paused) return;
    uint32_t now = furi_get_tick();
    uint32_t paused_for = now - a->pause_tick;
    a->paused = false;
    if(a->play_both) {
        a->both_start += paused_for;
        a->both_last_sound = now;
        return;
    }
    a->note_end_tick += paused_for;
    a->sound_end_tick += paused_for;
    a->note_start_tick += paused_for;
    a->chord_last = now;
    if(a->resume_sound && a->play_index < a->play_end) {
        Note n = a->notes[a->play_index];
        if(!n.rest) {
            furi_hal_speaker_start(pitch_hz(n.pitch), note_volume(&n));
            a->sounding = true;
        }
    }
}
static void playback_start_note(AppState* a, uint32_t now) {
    if(a->play_index >= a->play_end) {playback_stop(a); return;}
    Note n = a->notes[a->play_index];
    uint32_t ms = (uint32_t)(60000UL / a->tempo) * note_ticks(n) / 8;
    if(n.fermata && !n.rest) ms += ms / 2;
    if(ms < 20) ms = 20;
    a->note_end_tick = now + furi_ms_to_ticks(ms);
    a->sound_end_tick = a->note_end_tick;
    if(!n.rest) {
        bool tied_forward = n.tie == 1 && a->play_index + 1 < a->play_end && a->notes[a->play_index + 1].tie == 2 && a->notes[a->play_index + 1].pitch == n.pitch;
        bool tied_back = n.tie == 2 && a->play_index > 0 && a->notes[a->play_index - 1].tie == 1 && a->notes[a->play_index - 1].pitch == n.pitch;
        uint32_t gap = n.staccato ? ms / 2 : (n.staccatissimo ? ms * 3 / 4 : (n.slur || tied_forward || tied_back ? 0 : (n.tenuto ? 5 : 35)));
        if(gap >= ms) gap = ms / 4;
        a->sound_end_tick = now + furi_ms_to_ticks(ms - gap);
        if(!tied_back || !a->sounding) {
            if(a->sounding) {furi_hal_speaker_stop(); a->sounding = false;}
            furi_hal_speaker_start(pitch_hz(n.pitch), note_volume(&n));
            a->sounding = true;
        }
        if(tied_forward) a->sound_end_tick = a->note_end_tick;
        a->chord_last = now;
        a->chord_cycle = 0;
        a->note_start_tick = now;
        a->orn_off = 0;
    } else if(a->sounding) {furi_hal_speaker_stop(); a->sounding = false;}
    a->selected = a->play_index;
}
static void playback_begin(AppState* a, uint16_t from, bool single) {
    playback_stop(a);
    if(from >= a->count) {snprintf(a->notice, sizeof(a->notice), "No notes to play"); return;}
    if(!furi_hal_speaker_acquire(100)) {snprintf(a->notice, sizeof(a->notice), "Speaker busy"); return;}
    memset(a->repeat_used, 0, sizeof(a->repeat_used));
    a->play_single = single;
    a->speaker_owned = true; a->playing = true; a->play_index = from; a->play_end = single ? from + 1 : a->count;
    playback_start_note(a, furi_get_tick());
}
static void playback_begin_both(AppState* a) {
    playback_stop(a);
    if(!a->count && !a->other_count) {snprintf(a->notice, sizeof(a->notice), "No notes to play"); return;}
    if(!furi_hal_speaker_acquire(100)) {snprintf(a->notice, sizeof(a->notice), "Speaker busy"); return;}
    a->speaker_owned = true; a->playing = true; a->play_both = true;
    a->both_start = furi_get_tick(); a->both_last_sound = a->both_start;
    a->both_cycle = 0;
}

static void playback_update_both(AppState* a) {
    uint32_t now = furi_get_tick();
    uint32_t elapsed_ms = ((uint32_t)(((uint64_t)(now - a->both_start) * 1000U) / furi_ms_to_ticks(1000)));
    uint32_t quarter_ms = 60000UL / a->tempo;
    uint32_t tick = elapsed_ms * 8 / quarter_ms;
    uint32_t total1 = voice_total_ticks(a->notes, a->count);
    uint32_t total2 = voice_total_ticks(a->other_notes, a->other_count);
    if(tick >= (total1 > total2 ? total1 : total2)) {playback_stop(a); return;}
    if((now - a->both_last_sound) < furi_ms_to_ticks(22)) return;
    a->both_last_sound = now;
    Note n1, n2;
    bool has1 = voice_at(a->notes, a->count, tick, &n1);
    bool has2 = voice_at(a->other_notes, a->other_count, tick, &n2);
    uint8_t pitches[8]; uint8_t count = 0;
    if(has1) {
        pitches[count++] = n1.pitch;
        for(uint8_t k = 0; k < n1.chord_count; ++k) pitches[count++] = n1.chord[k];
    }
    if(has2) {
        pitches[count++] = n2.pitch;
        for(uint8_t k = 0; k < n2.chord_count; ++k) pitches[count++] = n2.chord[k];
    }
    if(a->sounding) {furi_hal_speaker_stop(); a->sounding = false;}
    if(count) {
        furi_hal_speaker_start(pitch_hz(pitches[a->both_cycle % count]), 0.45f);
        a->sounding = true;
    }
    ++a->both_cycle;
}

/* Moves to the next note. A repeat-end jumps back to the nearest earlier
 * repeat-start (or the beginning) once; each repeat-end is taken only once
 * per playback. Single-note preview never repeats. */
static void playback_advance(AppState* a) {
    uint16_t next = a->play_index + 1;
    if(!a->play_single && a->play_index < MAX_NOTES) {
        Note n = a->notes[a->play_index];
        uint8_t mask = (uint8_t)(1U << (a->play_index & 7));
        if(n.repeat_end && !(a->repeat_used[a->play_index >> 3] & mask)) {
            a->repeat_used[a->play_index >> 3] |= mask;
            uint16_t start = 0;
            for(uint16_t i = a->play_index + 1; i-- > 0;) {
                if(a->notes[i].repeat_start) {start = i; break;}
            }
            next = start;
        }
    }
    a->play_index = next;
}

static void playback_update(AppState* a) {
    if(!a->playing || a->paused) return;
    if(a->play_both) {playback_update_both(a); return;}
    uint32_t now = furi_get_tick();
    if(a->sounding && time_reached(now, a->sound_end_tick)) {
        Note n = a->notes[a->play_index];
        bool tied_forward = n.tie == 1 && a->play_index + 1 < a->play_end && a->notes[a->play_index + 1].tie == 2 && a->notes[a->play_index + 1].pitch == n.pitch;
        if(!tied_forward) {furi_hal_speaker_stop(); a->sounding = false;}
    }
    /* The speaker is monophonic, so chords are arpeggiated quickly. */
    if(a->sounding && a->play_index < a->play_end) {
        Note cn = a->notes[a->play_index];
        if(!cn.rest && cn.ornament) {
            uint32_t ms_in = (uint32_t)(((uint64_t)(now - a->note_start_tick) * 1000U) / furi_ms_to_ticks(1000));
            int off = ornament_offset(cn.ornament, ms_in);
            if(off != a->orn_off) {
                a->orn_off = (int8_t)off;
                furi_hal_speaker_stop();
                furi_hal_speaker_start(pitch_hz((uint8_t)((int)cn.pitch + off)), note_volume(&cn));
            }
        } else if(!cn.rest && cn.chord_count && (now - a->chord_last) >= furi_ms_to_ticks(22)) {
            uint8_t pitches[4]; uint8_t pc = 0;
            pitches[pc++] = cn.pitch;
            for(uint8_t k = 0; k < cn.chord_count && k < 3; ++k) pitches[pc++] = cn.chord[k];
            a->chord_last = now;
            a->chord_cycle++;
            furi_hal_speaker_stop();
            furi_hal_speaker_start(pitch_hz(pitches[a->chord_cycle % pc]), note_volume(&cn));
        }
    }
    if(time_reached(now, a->note_end_tick)) {playback_advance(a); playback_start_note(a, now);}
}

/* Runs one menu entry. Toggles return early (staying in the submenu so the
 * checkbox updates); everything else falls through to the score screen. */
static void do_menu(AppState* a, uint8_t entry) {
    Note* n = a->count ? &a->notes[a->selected] : NULL;
    a->notice[0] = '\0';
    if(entry >= ActCount) {
        if(n && !n->rest) {mark_toggle(n, entry - ActCount); return;}
        snprintf(a->notice, sizeof(a->notice), "Select a note first");
        a->screen = ScreenScore;
        return;
    }
    switch(entry) {
    case ActAddRest: a->adding_rest = true; a->screen = ScreenDuration; return;
    case ActTime: a->screen = ScreenTime; return;
    case ActKey: a->inserting_key = false; a->screen = ScreenKey; return;
    case ActSlurStart: case ActSlurEnd: case ActTieStart: case ActTieEnd:
        if(n && !n->rest) {
            if(entry == ActSlurStart) n->slur = n->slur == 1 ? 0 : 1;
            else if(entry == ActSlurEnd) n->slur = n->slur == 2 ? 0 : 2;
            else if(entry == ActTieStart) n->tie = n->tie == 1 ? 0 : 1;
            else n->tie = n->tie == 2 ? 0 : 2;
            return;
        }
        snprintf(a->notice, sizeof(a->notice), "Select a note first");
        break;
    case ActSave: snprintf(a->notice, sizeof(a->notice), write_score(a, false) ? "Score saved" : "Save failed"); break;
    case ActLoad: snprintf(a->notice, sizeof(a->notice), load_score(a) ? "Score loaded" : "Load failed"); break;
    case ActExport: snprintf(a->notice, sizeof(a->notice), write_score(a, true) ? "MusicXML exported" : "Export failed"); break;
    case ActClear:
        a->count = 0; a->selected = 0; a->other_count = 0; a->other_selected = 0;
        a->undo_valid = false;
        snprintf(a->notice, sizeof(a->notice), "Both voices cleared");
        break;
    case ActPlay: playback_begin(a, 0, false); break;
    case ActPlayFrom: playback_begin(a, a->selected, false); break;
    case ActPreview: playback_begin(a, a->selected, true); break;
    case ActTempo: a->screen = ScreenTempo; return;
    case ActVoice: switch_voice(a); snprintf(a->notice, sizeof(a->notice), "Voice %u", (unsigned)a->voice + 1); break;
    case ActCombined: a->combined = !a->combined; return;
    case ActChordAdd:
        if(n && !n->rest) {
            a->chord_pitch = n->pitch < 83 ? n->pitch + 12 : n->pitch - 12;
            a->chord_edit = true; a->screen = ScreenPitch; return;
        }
        snprintf(a->notice, sizeof(a->notice), "Select a note first");
        break;
    case ActChordRemove:
        if(n && n->chord_count) {n->chord_count--; snprintf(a->notice, sizeof(a->notice), "Chord tone removed");}
        else snprintf(a->notice, sizeof(a->notice), "No chord tones");
        break;
    case ActXmlLayout: a->separate_staves = !a->separate_staves; return;
    case ActPlayBoth: playback_begin_both(a); break;
    case ActHelp: a->about_page = 0; a->screen = ScreenAbout; return;
    case ActDotted: if(n) {n->dotted ^= 1; return;} snprintf(a->notice, sizeof(a->notice), "No notes yet"); break;
    case ActRepeatStart: if(n) {n->repeat_start ^= 1; return;} snprintf(a->notice, sizeof(a->notice), "No notes yet"); break;
    case ActRepeatEnd: if(n) {n->repeat_end ^= 1; return;} snprintf(a->notice, sizeof(a->notice), "No notes yet"); break;
    case ActKeyChange:
        if(n) {
            a->inserting_key = true;
            active_key(a, a->notes, a->count, a->selected, &a->edit_fifths, &a->edit_minor);
            a->screen = ScreenKey; return;
        }
        snprintf(a->notice, sizeof(a->notice), "No notes yet");
        break;
    case ActKeyRemove: if(n) {n->key_change = 0; snprintf(a->notice, sizeof(a->notice), "Key change removed");} break;
    case ActUndo: undo_delete(a); break;
    case ActBarline:
        if(n) {
            a->barline_choice = a->notes[measure_last_index(a, a->selected)].barline;
            a->screen = ScreenBarline; return;
        }
        snprintf(a->notice, sizeof(a->notice), "No notes yet");
        break;
    default: break;
    }
    a->screen = ScreenScore;
}

int32_t bards_score_app(void* p) {
    UNUSED(p);
    AppState* a = malloc(sizeof(AppState));
    if(!a) return -1;
    memset(a, 0, sizeof(AppState));
    a->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    a->queue = furi_message_queue_alloc(8, sizeof(InputEvent));
    a->viewport = view_port_alloc();
    if(!a->mutex || !a->queue || !a->viewport) {
        if(a->viewport) view_port_free(a->viewport);
        if(a->queue) furi_message_queue_free(a->queue);
        if(a->mutex) furi_mutex_free(a->mutex);
        free(a); return -1;
    }
    a->running = true; a->pitch = 69; a->duration = 2;
    a->beats = 4; a->beat_type = 4; a->time_choice = 2;
    a->tempo = 120; a->fifths = 0; a->minor = false; a->separate_staves = true;
    view_port_draw_callback_set(a->viewport, draw_callback, a);
    view_port_input_callback_set(a->viewport, input_callback, a);
    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, a->viewport, GuiLayerFullscreen);
    view_port_update(a->viewport);
    InputEvent ev;
    while(a->running) {
        FuriStatus status = furi_message_queue_get(a->queue, &ev, (a->playing && !a->paused) ? furi_ms_to_ticks(10) : FuriWaitForever);
        furi_mutex_acquire(a->mutex, FuriWaitForever);
        if(status != FuriStatusOk) {
            playback_update(a);
            furi_mutex_release(a->mutex); view_port_update(a->viewport); continue;
        }
        a->notice[0] = '\0';
        if(a->playing) {
            if(ev.key == InputKeyBack && ev.type == InputTypeLong) {playback_stop(a); a->running = false;}
            else if(ev.key == InputKeyBack && ev.type == InputTypeShort) {playback_stop(a); snprintf(a->notice, sizeof(a->notice), "Playback stopped");}
            else if(ev.key == InputKeyOk && ev.type == InputTypeShort) {
                if(a->paused) playback_resume(a); else playback_pause(a);
            }
            furi_mutex_release(a->mutex); view_port_update(a->viewport); continue;
        }
        /* "nav" accepts auto-repeat for the arrow keys only, so holding
         * Up/Down scrolls pitch, tempo and menus. OK and Back never repeat. */
        bool nav = ev.type == InputTypeShort ||
                   (ev.type == InputTypeRepeat &&
                    (ev.key == InputKeyUp || ev.key == InputKeyDown ||
                     ev.key == InputKeyLeft || ev.key == InputKeyRight));
        if(a->screen == ScreenScore) {
            if(ev.key == InputKeyBack && ev.type == InputTypeLong) a->running = false;
            else if(ev.key == InputKeyBack && ev.type == InputTypeShort) delete_selected_note(a);
            else if(ev.key == InputKeyOk && ev.type == InputTypeLong) {a->menu = 0; a->menu_top = 0; a->screen = ScreenMenu;}
            else if(nav) {
                if(ev.key == InputKeyOk) {a->adding_rest = false; a->chord_edit = false; a->screen = ScreenPitch;}
                else if(ev.key == InputKeyLeft && a->selected > 0) a->selected--;
                else if(ev.key == InputKeyRight && a->selected + 1 < a->count) a->selected++;
                else if(ev.key == InputKeyUp) shift_pitch(a, 1);
                else if(ev.key == InputKeyDown) shift_pitch(a, -1);
            }
        } else if(a->screen == ScreenPitch) {
            if(ev.key == InputKeyBack && ev.type == InputTypeShort) {a->chord_edit = false; a->screen = ScreenScore;}
            else if(nav) {
                if(ev.key == InputKeyUp && (a->chord_edit ? a->chord_pitch : a->pitch) < 95) {if(a->chord_edit) a->chord_pitch++; else a->pitch++;}
                else if(ev.key == InputKeyDown && (a->chord_edit ? a->chord_pitch : a->pitch) > 48) {if(a->chord_edit) a->chord_pitch--; else a->pitch--;}
                else if(ev.key == InputKeyOk) {if(a->chord_edit) {chord_add(a); a->chord_edit = false; a->screen = ScreenScore;} else a->screen = ScreenDuration;}
            }
        } else if(a->screen == ScreenDuration) {
            if(ev.key == InputKeyBack && ev.type == InputTypeShort) a->screen = a->adding_rest ? ScreenScore : ScreenPitch;
            else if(nav) {
                if(ev.key == InputKeyUp && a->duration > 0) a->duration--;
                else if(ev.key == InputKeyDown && a->duration < 4) a->duration++;
                else if(ev.key == InputKeyOk) {insert_note(a); a->screen = ScreenScore;}
            }
        } else if(a->screen == ScreenTime) {
            if(ev.key == InputKeyBack && ev.type == InputTypeShort) a->screen = ScreenScore;
            else if(nav) {
                if(ev.key == InputKeyUp && a->time_choice > 0) a->time_choice--;
                else if(ev.key == InputKeyDown && a->time_choice < 3) a->time_choice++;
                else if(ev.key == InputKeyOk) {a->beats = time_beats[a->time_choice]; a->beat_type = time_denoms[a->time_choice]; a->screen = ScreenScore;}
            }
        } else if(a->screen == ScreenKey) {
            if(ev.key == InputKeyBack && ev.type == InputTypeShort) {a->inserting_key=false; a->screen = ScreenScore;}
            else if(nav) {
                if(ev.key == InputKeyUp && (a->inserting_key ? a->edit_fifths : a->fifths) < 7) {if(a->inserting_key) a->edit_fifths++; else a->fifths++;}
                else if(ev.key == InputKeyDown && (a->inserting_key ? a->edit_fifths : a->fifths) > -7) {if(a->inserting_key) a->edit_fifths--; else a->fifths--;}
                else if(ev.type == InputTypeShort && (ev.key == InputKeyLeft || ev.key == InputKeyRight)) {if(a->inserting_key) a->edit_minor=!a->edit_minor; else a->minor=!a->minor;}
                else if(ev.key == InputKeyOk) {if(a->inserting_key && a->count) {Note* n=&a->notes[a->selected]; n->key_change=1; n->key_fifths=a->edit_fifths; n->key_minor=a->edit_minor;} a->inserting_key=false; a->screen = ScreenScore;}
            }
        } else if(a->screen == ScreenTempo) {
            if(ev.key == InputKeyBack && ev.type == InputTypeShort) a->screen = ScreenScore;
            else if(nav) {
                if(ev.key == InputKeyUp && a->tempo < 240) a->tempo += 5;
                else if(ev.key == InputKeyDown && a->tempo > 40) a->tempo -= 5;
                else if(ev.key == InputKeyOk) a->screen = ScreenScore;
            }
        } else if(a->screen == ScreenAbout) {
            if(ev.key == InputKeyBack && ev.type == InputTypeShort) a->screen = ScreenMenu;
            else if(ev.type == InputTypeShort) {
                if(ev.key == InputKeyUp) a->about_page = a->about_page == 0 ? 8 : a->about_page - 1;
                else if(ev.key == InputKeyDown) a->about_page = a->about_page == 8 ? 0 : a->about_page + 1;
                else if(ev.key == InputKeyOk) a->about_page = (a->about_page + 1) % 9;
            }
        } else if(a->screen == ScreenBarline) {
            if(ev.key == InputKeyBack && ev.type == InputTypeShort) a->screen = ScreenScore;
            else if(nav) {
                if(ev.key == InputKeyUp) a->barline_choice = a->barline_choice == 0 ? BARLINE_COUNT - 1 : a->barline_choice - 1;
                else if(ev.key == InputKeyDown) a->barline_choice = a->barline_choice + 1 >= BARLINE_COUNT ? 0 : a->barline_choice + 1;
                else if(ev.key == InputKeyOk && a->count) {
                    a->notes[measure_last_index(a, a->selected)].barline = a->barline_choice;
                    snprintf(a->notice, sizeof(a->notice), "Barline set");
                    a->screen = ScreenScore;
                }
            }
        } else if(a->screen == ScreenMenu) {
            if(ev.key == InputKeyBack && ev.type == InputTypeShort) a->screen = ScreenScore;
            else if(nav) {
                if(ev.key == InputKeyUp) a->menu = a->menu == 0 ? MENU_CAT_COUNT - 1 : a->menu - 1;
                else if(ev.key == InputKeyDown) a->menu = a->menu + 1 >= MENU_CAT_COUNT ? 0 : a->menu + 1;
                else if(ev.key == InputKeyOk) {
                    const MenuCat* cat = &menu_cats[a->menu];
                    if(cat->count == 1) do_menu(a, cat->items[0]);
                    else {a->menu_cat = a->menu; a->sub = 0; a->sub_top = 0; a->screen = ScreenSubmenu;}
                }
                if(a->menu < a->menu_top) a->menu_top = a->menu;
                if(a->menu >= a->menu_top + 4) a->menu_top = a->menu - 3;
            }
        } else if(a->screen == ScreenSubmenu) {
            const MenuCat* cat = &menu_cats[a->menu_cat];
            if(ev.key == InputKeyBack && ev.type == InputTypeShort) a->screen = ScreenMenu;
            else if(nav) {
                if(ev.key == InputKeyUp) a->sub = a->sub == 0 ? cat->count - 1 : a->sub - 1;
                else if(ev.key == InputKeyDown) a->sub = a->sub + 1 >= cat->count ? 0 : a->sub + 1;
                else if(ev.key == InputKeyOk) do_menu(a, cat->items[a->sub]);
                if(a->sub < a->sub_top) a->sub_top = a->sub;
                if(a->sub >= a->sub_top + 4) a->sub_top = a->sub - 3;
            }
        }
        furi_mutex_release(a->mutex);
        view_port_update(a->viewport);
    }
    playback_stop(a);
    gui_remove_view_port(gui, a->viewport);
    view_port_free(a->viewport);
    furi_record_close(RECORD_GUI);
    furi_message_queue_free(a->queue);
    furi_mutex_free(a->mutex);
    free(a);
    return 0;
}
