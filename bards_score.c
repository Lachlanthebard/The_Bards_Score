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
#define XML_FILE SCORE_DIR "/melody.musicxml"
#define STAFF_BOTTOM 47
#define STAFF_SPACING 7
#define MENU_COUNT 18
#define VISIBLE_NOTES 5

typedef struct {
    uint8_t pitch, duration, rest, slur, tie, accent, staccato, fermata;
} Note;

typedef enum {ScreenScore, ScreenPitch, ScreenDuration, ScreenMenu, ScreenTime, ScreenTempo, ScreenKey} Screen;

typedef struct {
    FuriMutex* mutex;
    FuriMessageQueue* queue;
    ViewPort* viewport;
    Note notes[MAX_NOTES];
    uint16_t count, selected;
    uint8_t pitch, duration, menu, menu_top, beats, beat_type, time_choice;
    int8_t fifths;
    bool minor, adding_rest, running;
    Screen screen;
    char notice[22];
    uint16_t tempo;
    bool playing, speaker_owned, sounding;
    uint16_t play_index, play_end;
    uint32_t note_end_tick, sound_end_tick;
} AppState;

static const char* const durations[] = {"Whole", "Half", "Quarter", "Eighth", "16th"};
static const char* const pitch_names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
static const char* const xml_types[] = {"whole", "half", "quarter", "eighth", "16th"};
static const char* const menu_items[MENU_COUNT] = {
    "Add rest", "Time signature", "Key signature", "Slur start", "Slur end",
    "Tie start", "Tie end", "Staccato", "Accent", "Fermata",
    "Save score", "Load score", "Export XML", "Clear score",
    "Play score", "Play from here", "Preview note", "Set tempo"
};
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

static uint8_t duration_ticks(uint8_t d) {return (uint8_t)(16U >> d);}
static uint8_t measure_ticks(const AppState* a) {return (uint8_t)(a->beats * (16U / a->beat_type));}
static void pitch_label(uint8_t p, char* b, size_t n) {snprintf(b, n, "%s%d", pitch_names[p % 12], (int)p / 12 - 1);}
static int note_step(uint8_t p) {return ((int)p / 12 - 5) * 7 + degree_pc[p % 12] - 2;}
static int step_y(int step) {return STAFF_BOTTOM - (step * STAFF_SPACING + (step >= 0 ? 1 : -1)) / 2;}
static int note_y(uint8_t p) {return step_y(note_step(p));}

static void draw_staff(Canvas* c) {
    for(int i = 0; i < 5; ++i) canvas_draw_line(c, 2, STAFF_BOTTOM - i * STAFF_SPACING, 125, STAFF_BOTTOM - i * STAFF_SPACING);
    canvas_draw_line(c, 12, 11, 12, 49);
    canvas_draw_circle(c, 12, 32, 6);
    canvas_draw_circle(c, 12, 39, 3);
    canvas_draw_line(c, 12, 11, 16, 16);
    canvas_draw_line(c, 16, 16, 12, 24);
    canvas_draw_line(c, 12, 49, 9, 53);
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
static void spell_pitch(const AppState* a, uint8_t pitch, uint8_t* letter, int* alter) {
    int pc = pitch % 12;
    int best = 100;
    uint8_t best_letter = 0;
    int best_alter = 0;
    for(uint8_t l = 0; l < 7; ++l) {
        for(int al = -1; al <= 1; ++al) {
            if((natural_pc[l] + al + 12) % 12 != pc) continue;
            int cost = (al == key_alter(a->fifths, l)) ? 0 : (al == 0 ? 2 : 3);
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

static void draw_key_signature(Canvas* c, const AppState* a) {
    int count = a->fifths < 0 ? -a->fifths : a->fifths;
    for(int i = 0; i < count; ++i) {
        int step = a->fifths > 0 ? sharp_steps[i] : flat_steps[i];
        draw_accidental(c, 23 + i * 5, step_y(step), a->fifths > 0 ? 1 : -1);
    }
}

static void draw_rest(Canvas* c, int x, uint8_t d) {
    if(d <= 1) {
        /* v1.2 correction: whole hangs below line 4; half rests above line 3. */
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

static int stem_end(const AppState* a, Note n, bool down) {
    uint8_t letter; int alter;
    spell_pitch(a, n.pitch, &letter, &alter);
    int y = step_y(spelled_step(n.pitch, letter, alter));
    return y + (down ? 17 : -17);
}

static bool stem_down(const AppState* a, Note n) {
    uint8_t letter; int alter;
    spell_pitch(a, n.pitch, &letter, &alter);
    return spelled_step(n.pitch, letter, alter) >= 4;
}

static void draw_note(Canvas* c, const AppState* a, int x, Note n, bool selected, bool beamed) {
    if(n.rest) {
        draw_rest(c, x, n.duration);
        if(selected) canvas_draw_frame(c, x - 6, 20, 13, 26);
        return;
    }
    uint8_t letter; int alter;
    spell_pitch(a, n.pitch, &letter, &alter);
    int step = spelled_step(n.pitch, letter, alter);
    int y = step_y(step);
    for(int s = -2; s >= step && s >= -14; s -= 2) canvas_draw_line(c, x - 6, step_y(s), x + 6, step_y(s));
    for(int s = 10; s <= step && s <= 22; s += 2) canvas_draw_line(c, x - 6, step_y(s), x + 6, step_y(s));
    if(alter != key_alter(a->fifths, letter)) draw_accidental(c, x - 8, y, alter);
    if(n.duration <= 1) canvas_draw_circle(c, x, y, 3);
    else canvas_draw_disc(c, x, y, 3);
    if(n.duration != 0) {
        bool down = stem_down(a, n);
        int sx = x + (down ? -3 : 3);
        int end = stem_end(a, n, down);
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
    if(n.staccato) canvas_draw_disc(c, x, y < 28 ? y + 7 : y - 7, 1);
    if(n.accent) {
        canvas_draw_line(c, x - 4, y - 9, x + 2, y - 11);
        canvas_draw_line(c, x + 2, y - 11, x - 4, y - 13);
    }
    if(n.fermata) {canvas_draw_circle(c, x, y - 13, 4); canvas_draw_disc(c, x, y - 13, 1);}
    if(selected) canvas_draw_frame(c, x - 5, y - 5, 11, 11);
}

static uint8_t tick_before(const AppState* a, uint16_t index) {
    uint8_t cap = measure_ticks(a), used = 0;
    for(uint16_t i = 0; i < index; ++i) {
        uint8_t ticks = duration_ticks(a->notes[i].duration);
        if(used && used + ticks > cap) used = 0;
        used += ticks;
        if(used >= cap) used = 0;
    }
    return used;
}

static uint8_t group_ticks(const AppState* a) {return a->beat_type == 8 ? 6 : 4;}

static bool can_beam(const AppState* a, uint16_t left) {
    if(left + 1 >= a->count) return false;
    Note n = a->notes[left], next = a->notes[left + 1];
    if(n.rest || next.rest || n.duration < 3 || next.duration < 3) return false;
    uint8_t used = tick_before(a, left), ticks = duration_ticks(n.duration);
    uint8_t cap = measure_ticks(a), group = group_ticks(a);
    if(used + ticks >= cap || used + ticks + duration_ticks(next.duration) > cap) return false;
    if(used + ticks > group * ((used / group) + 1)) return false;
    return (used + ticks) / group == used / group;
}

static void draw_beams(Canvas* c, const AppState* a, uint16_t start, uint16_t end, int x0, int spacing) {
    for(uint16_t i = start; i + 1 < end; ++i) {
        if(!can_beam(a, i)) continue;
        Note n = a->notes[i], next = a->notes[i + 1];
        bool down = stem_down(a, n);
        if(stem_down(a, next) != down) continue;
        int x1 = x0 + (i - start) * spacing + (down ? -3 : 3);
        int x2 = x0 + (i + 1 - start) * spacing + (down ? -3 : 3);
        int y1 = stem_end(a, n, down), y2 = stem_end(a, next, down);
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

static void draw_callback(Canvas* c, void* ctx) {
    AppState* a = ctx;
    furi_mutex_acquire(a->mutex, FuriWaitForever);
    canvas_clear(c);
    canvas_set_font(c, FontPrimary);
    if(a->screen == ScreenScore) {
        char header[40];
        snprintf(header, sizeof(header), "%u/%u  %s %s  %u", a->beats, a->beat_type,
            a->minor ? minor_names[a->fifths + 7] : major_names[a->fifths + 7],
            a->minor ? "m" : "", (unsigned)a->tempo);
        canvas_set_font(c, FontSecondary);
        canvas_draw_str(c, 2, 9, header);
        draw_staff(c);
        draw_key_signature(c, a);
        uint16_t start = a->selected > 2 ? a->selected - 2 : 0;
        int sig_count = a->fifths < 0 ? -a->fifths : a->fifths;
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
            uint8_t ticks = duration_ticks(a->notes[i].duration);
            if(used && used + ticks > cap) {canvas_draw_line(c, x - 8, 19, x - 8, STAFF_BOTTOM); used = 0;}
            bool beamed = (i > 0 && can_beam(a, i - 1) && stem_down(a, a->notes[i - 1]) == stem_down(a, a->notes[i])) ||
                          (can_beam(a, i) && stem_down(a, a->notes[i]) == stem_down(a, a->notes[i + 1]));
            draw_note(c, a, x, a->notes[i], i == a->selected, beamed);
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
            if(used >= cap) {if(i + 1 < end) canvas_draw_line(c, x + 8, 19, x + 8, STAFF_BOTTOM); used = 0;}
        }
        draw_beams(c, a, start, end, x0, spacing);
        canvas_set_font(c, FontSecondary);
        char status[42];
        if(a->playing) snprintf(status, sizeof(status), "Playing %u/%u Back:Stop", (unsigned)(a->play_index + 1), (unsigned)a->count);
        else if(a->notice[0]) snprintf(status, sizeof(status), "%s", a->notice);
        else if(a->count) {
            Note n = a->notes[a->selected]; char label[8];
            if(n.rest) snprintf(label, sizeof(label), "Rest"); else pitch_label(n.pitch, label, sizeof(label));
            snprintf(status, sizeof(status), "%s %s %u/%u", label, durations[n.duration], (unsigned)(a->selected + 1), (unsigned)a->count);
        } else snprintf(status, sizeof(status), "OK:Add Hold OK:Tools");
        canvas_draw_str(c, 2, 63, status);
    } else if(a->screen == ScreenMenu) {
        canvas_draw_str(c, 2, 11, "Score tools");
        canvas_set_font(c, FontSecondary);
        for(uint8_t i = a->menu_top; i < MENU_COUNT && i < a->menu_top + 4; ++i) {
            char line[28]; snprintf(line, sizeof(line), "%c %s", a->menu == i ? '>' : ' ', menu_items[i]);
            canvas_draw_str(c, 3, 23 + (i - a->menu_top) * 12, line);
        }
    } else {
        const char* title = a->screen == ScreenPitch ? "Choose pitch" :
            a->screen == ScreenDuration ? (a->adding_rest ? "Rest length" : "Note length") :
            a->screen == ScreenTempo ? "Playback tempo" : a->screen == ScreenKey ? "Key signature" : "Time signature";
        canvas_draw_str(c, 2, 11, title);
        canvas_set_font(c, FontSecondary);
        char line[32];
        if(a->screen == ScreenPitch) {pitch_label(a->pitch, line, sizeof(line)); canvas_draw_str(c, 8, 33, line); canvas_draw_str(c, 2, 61, "Up/Down OK:Next");}
        else if(a->screen == ScreenDuration) {canvas_draw_str(c, 8, 33, durations[a->duration]); canvas_draw_str(c, 2, 61, "Up/Down OK:Insert");}
        else if(a->screen == ScreenTempo) {snprintf(line, sizeof(line), "%u BPM", (unsigned)a->tempo); canvas_draw_str(c, 8, 33, line); canvas_draw_str(c, 2, 61, "Up/Down +/-5 OK:Set");}
        else if(a->screen == ScreenKey) {
            snprintf(line, sizeof(line), "%s %s (%d)", a->minor ? minor_names[a->fifths + 7] : major_names[a->fifths + 7], a->minor ? "minor" : "major", (int)a->fifths);
            canvas_draw_str(c, 3, 33, line);
            canvas_draw_str(c, 2, 61, "Up/Dn:key L/R:mode OK");
        } else {snprintf(line, sizeof(line), "%u/%u", time_beats[a->time_choice], time_denoms[a->time_choice]); canvas_draw_str(c, 8, 33, line); canvas_draw_str(c, 2, 61, "Up/Down OK:Set");}
    }
    furi_mutex_release(a->mutex);
}

static void input_callback(InputEvent* e, void* ctx) {
    AppState* a = ctx;
    if(e->type == InputTypeShort || e->type == InputTypeLong) furi_message_queue_put(a->queue, e, 0);
}
static bool file_write_text(File* f, const char* s) {size_t n = strlen(s); return storage_file_write(f, s, n) == n;}

static bool write_score(AppState* a, bool xml) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_common_mkdir(storage, SCORE_DIR);
    File* f = storage_file_alloc(storage);
    bool ok = storage_file_open(f, xml ? XML_FILE : SCORE_FILE, FSAM_WRITE, FSOM_CREATE_ALWAYS);
    if(ok) {
        char buf[512];
        if(!xml) {
            ok = file_write_text(f, "BSC3\n");
            snprintf(buf, sizeof(buf), "%u %u %d %u %u\n", a->beats, a->beat_type, (int)a->fifths, (unsigned)a->minor, (unsigned)a->tempo);
            if(ok) ok = file_write_text(f, buf);
            for(uint16_t i = 0; i < a->count && ok; ++i) {
                Note n = a->notes[i];
                snprintf(buf, sizeof(buf), "%u %u %u %u %u %u %u %u\n", n.pitch, n.duration, n.rest, n.slur, n.tie, n.accent, n.staccato, n.fermata);
                ok = file_write_text(f, buf);
            }
        } else {
            ok = file_write_text(f, "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<score-partwise version=\"4.0\"><work><work-title>Bard's Melody</work-title></work><movement-title>Bard's Melody</movement-title><part-list><score-part id=\"P1\"><part-name>Hulusi</part-name></score-part></part-list><part id=\"P1\">\n");
            uint16_t measure = 1;
            uint8_t used = 0, cap = measure_ticks(a);
            bool open = false;
            for(uint16_t i = 0; i < a->count && ok; ++i) {
                Note n = a->notes[i]; uint8_t ticks = duration_ticks(n.duration);
                if(open && used && used + ticks > cap) {ok = file_write_text(f, "</measure>\n"); open = false; used = 0;}
                if(!open && ok) {
                    snprintf(buf, sizeof(buf), "<measure implicit=\"no\" number=\"%u\">", (unsigned)measure++);
                    ok = file_write_text(f, buf); open = true;
                    if(measure == 2 && ok) {
                        snprintf(buf, sizeof(buf), "<attributes><divisions>4</divisions><key><fifths>%d</fifths><mode>%s</mode></key><time><beats>%u</beats><beat-type>%u</beat-type></time><clef><sign>G</sign><line>2</line></clef></attributes>", (int)a->fifths, a->minor ? "minor" : "major", a->beats, a->beat_type);
                        ok = file_write_text(f, buf);
                    }
                }
                if(!ok) break;
                if(n.rest) snprintf(buf, sizeof(buf), "<note><rest/><duration>%u</duration><type>%s</type>", ticks, xml_types[n.duration]);
                else {
                    uint8_t letter; int alter; spell_pitch(a, n.pitch, &letter, &alter);
                    int octave = ((int)n.pitch - (int)natural_pc[letter] - alter) / 12 - 1;
                    snprintf(buf, sizeof(buf), "<note><pitch><step>%s</step><alter>%d</alter><octave>%d</octave></pitch><duration>%u</duration><type>%s</type>", step_names[letter], alter, octave, ticks, xml_types[n.duration]);
                }
                ok = file_write_text(f, buf);
                if(!n.rest && ok && n.tie == 1) ok = file_write_text(f, "<tie type=\"start\"/>");
                if(!n.rest && ok && n.tie == 2) ok = file_write_text(f, "<tie type=\"stop\"/>");
                if(!n.rest && (n.slur || n.tie || n.accent || n.staccato || n.fermata) && ok) {
                    ok = file_write_text(f, "<notations>");
                    if(n.slur == 1 && ok) ok = file_write_text(f, "<slur number=\"1\" type=\"start\"/>");
                    if(n.slur == 2 && ok) ok = file_write_text(f, "<slur number=\"1\" type=\"stop\"/>");
                    if(n.tie == 1 && ok) ok = file_write_text(f, "<tied type=\"start\"/>");
                    if(n.tie == 2 && ok) ok = file_write_text(f, "<tied type=\"stop\"/>");
                    if((n.accent || n.staccato) && ok) {
                        ok = file_write_text(f, "<articulations>");
                        if(n.accent && ok) ok = file_write_text(f, "<accent/>");
                        if(n.staccato && ok) ok = file_write_text(f, "<staccato/>");
                        if(ok) ok = file_write_text(f, "</articulations>");
                    }
                    if(n.fermata && ok) ok = file_write_text(f, "<fermata/>");
                    if(ok) ok = file_write_text(f, "</notations>");
                }
                if(ok) ok = file_write_text(f, "</note>");
                used += ticks;
                if(used >= cap && ok) {ok = file_write_text(f, "</measure>\n"); open = false; used = 0;}
            }
            if(open && ok) ok = file_write_text(f, "</measure>\n");
            if(ok) ok = file_write_text(f, "</part></score-partwise>\n");
        }
        storage_file_close(f);
    }
    storage_file_free(f); furi_record_close(RECORD_STORAGE);
    return ok;
}

static bool load_score(AppState* a) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* f = storage_file_alloc(storage);
    bool ok = storage_file_open(f, SCORE_FILE, FSAM_READ, FSOM_OPEN_EXISTING);
    if(ok) {
        Note temp[MAX_NOTES]; uint16_t count = 0, line_no = 0;
        uint8_t beats = 4, denom = 4; int fifths = 0; unsigned minor = 0, tempo = 120;
        int version = 0; bool valid = true;
        char line[96]; size_t pos = 0;
        while(valid) {
            char ch; size_t rd = storage_file_read(f, &ch, 1);
            if(rd != 1) {if(pos == 0) break; ch = '\n';}
            if(ch == '\r') continue;
            if(ch == '\n') {
                line[pos] = '\0';
                if(line_no == 0) {
                    if(strcmp(line, "BSC1") == 0) version = 1;
                    else if(strcmp(line, "BSC2") == 0) version = 2;
                    else if(strcmp(line, "BSC3") == 0) version = 3;
                    else valid = false;
                } else if(version >= 2 && line_no == 1) {
                    unsigned b = 0, d = 0;
                    int matched = version == 3 ? sscanf(line, "%u %u %d %u %u", &b, &d, &fifths, &minor, &tempo) : sscanf(line, "%u %u", &b, &d);
                    if(matched != (version == 3 ? 5 : 2) || !(((b == 2 || b == 3 || b == 4) && d == 4) || (b == 6 && d == 8)) || fifths < -7 || fifths > 7 || minor > 1 || tempo < 40 || tempo > 240) valid = false;
                    else {beats = b; denom = d;}
                } else if(line[0] && count < MAX_NOTES) {
                    unsigned p = 0, d = 0, r = 0, s = 0, t = 0, ac = 0, st = 0, fe = 0;
                    int matched = version >= 2 ? sscanf(line, "%u %u %u %u %u %u %u %u", &p, &d, &r, &s, &t, &ac, &st, &fe) : sscanf(line, "%u %u", &p, &d);
                    if(matched != (version >= 2 ? 8 : 2) || p < 48 || p > 95 || d > 4 || r > 1 || s > 2 || t > 2 || ac > 1 || st > 1 || fe > 1) valid = false;
                    else temp[count++] = (Note){(uint8_t)p, (uint8_t)d, (uint8_t)r, (uint8_t)s, (uint8_t)t, (uint8_t)ac, (uint8_t)st, (uint8_t)fe};
                } else if(line[0] && count >= MAX_NOTES) valid = false;
                line_no++; pos = 0;
                if(rd != 1) break;
            } else if(pos < sizeof(line) - 1) line[pos++] = ch;
            else valid = false;
        }
        storage_file_close(f);
        ok = valid && line_no >= (version >= 2 ? 2 : 1);
        if(ok) {
            memcpy(a->notes, temp, count * sizeof(Note));
            a->count = count; a->selected = 0; a->beats = beats; a->beat_type = denom;
            a->fifths = fifths; a->minor = minor; a->tempo = tempo;
            for(uint8_t i = 0; i < 4; ++i) if(time_beats[i] == beats && time_denoms[i] == denom) a->time_choice = i;
        }
    }
    storage_file_free(f); furi_record_close(RECORD_STORAGE);
    return ok;
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
static void playback_stop(AppState* a) {
    if(a->sounding) {furi_hal_speaker_stop(); a->sounding = false;}
    if(a->speaker_owned) {furi_hal_speaker_release(); a->speaker_owned = false;}
    a->playing = false;
}
static bool time_reached(uint32_t now, uint32_t deadline) {return (int32_t)(now - deadline) >= 0;}
static void playback_start_note(AppState* a, uint32_t now) {
    if(a->play_index >= a->play_end) {playback_stop(a); return;}
    Note n = a->notes[a->play_index];
    uint32_t ms = (uint32_t)(60000UL / a->tempo) * duration_ticks(n.duration) / 4;
    if(n.fermata && !n.rest) ms += ms / 2;
    if(ms < 20) ms = 20;
    a->note_end_tick = now + furi_ms_to_ticks(ms);
    a->sound_end_tick = a->note_end_tick;
    if(!n.rest) {
        bool tied_forward = n.tie == 1 && a->play_index + 1 < a->play_end && a->notes[a->play_index + 1].tie == 2 && a->notes[a->play_index + 1].pitch == n.pitch;
        bool tied_back = n.tie == 2 && a->play_index > 0 && a->notes[a->play_index - 1].tie == 1 && a->notes[a->play_index - 1].pitch == n.pitch;
        uint32_t gap = n.staccato ? ms / 2 : (n.slur || tied_forward || tied_back ? 0 : 35);
        if(gap >= ms) gap = ms / 4;
        a->sound_end_tick = now + furi_ms_to_ticks(ms - gap);
        if(!tied_back || !a->sounding) {
            if(a->sounding) {furi_hal_speaker_stop(); a->sounding = false;}
            furi_hal_speaker_start(pitch_hz(n.pitch), n.accent ? 0.8f : 0.5f);
            a->sounding = true;
        }
        if(tied_forward) a->sound_end_tick = a->note_end_tick;
    } else if(a->sounding) {furi_hal_speaker_stop(); a->sounding = false;}
    a->selected = a->play_index;
}
static void playback_begin(AppState* a, uint16_t from, bool single) {
    playback_stop(a);
    if(from >= a->count) {snprintf(a->notice, sizeof(a->notice), "No notes to play"); return;}
    if(!furi_hal_speaker_acquire(100)) {snprintf(a->notice, sizeof(a->notice), "Speaker busy"); return;}
    a->speaker_owned = true; a->playing = true; a->play_index = from; a->play_end = single ? from + 1 : a->count;
    playback_start_note(a, furi_get_tick());
}
static void playback_update(AppState* a) {
    if(!a->playing) return;
    uint32_t now = furi_get_tick();
    if(a->sounding && time_reached(now, a->sound_end_tick)) {
        Note n = a->notes[a->play_index];
        bool tied_forward = n.tie == 1 && a->play_index + 1 < a->play_end && a->notes[a->play_index + 1].tie == 2 && a->notes[a->play_index + 1].pitch == n.pitch;
        if(!tied_forward) {furi_hal_speaker_stop(); a->sounding = false;}
    }
    if(time_reached(now, a->note_end_tick)) {a->play_index++; playback_start_note(a, now);}
}

static void do_menu(AppState* a) {
    Note* n = a->count ? &a->notes[a->selected] : NULL;
    a->notice[0] = '\0';
    switch(a->menu) {
    case 0: a->adding_rest = true; a->screen = ScreenDuration; return;
    case 1: a->screen = ScreenTime; return;
    case 2: a->screen = ScreenKey; return;
    case 3: if(n && !n->rest) n->slur = n->slur == 1 ? 0 : 1; break;
    case 4: if(n && !n->rest) n->slur = n->slur == 2 ? 0 : 2; break;
    case 5: if(n && !n->rest) n->tie = n->tie == 1 ? 0 : 1; break;
    case 6: if(n && !n->rest) n->tie = n->tie == 2 ? 0 : 2; break;
    case 7: if(n && !n->rest) n->staccato ^= 1; break;
    case 8: if(n && !n->rest) n->accent ^= 1; break;
    case 9: if(n && !n->rest) n->fermata ^= 1; break;
    case 10: snprintf(a->notice, sizeof(a->notice), write_score(a, false) ? "Score saved" : "Save failed"); break;
    case 11: snprintf(a->notice, sizeof(a->notice), load_score(a) ? "Score loaded" : "Load failed"); break;
    case 12: snprintf(a->notice, sizeof(a->notice), write_score(a, true) ? "MusicXML exported" : "Export failed"); break;
    case 13: a->count = 0; a->selected = 0; snprintf(a->notice, sizeof(a->notice), "Score cleared"); break;
    case 14: playback_begin(a, 0, false); break;
    case 15: playback_begin(a, a->selected, false); break;
    case 16: playback_begin(a, a->selected, true); break;
    case 17: a->screen = ScreenTempo; return;
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
    a->tempo = 120; a->fifths = 0; a->minor = false;
    view_port_draw_callback_set(a->viewport, draw_callback, a);
    view_port_input_callback_set(a->viewport, input_callback, a);
    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, a->viewport, GuiLayerFullscreen);
    view_port_update(a->viewport);
    InputEvent ev;
    while(a->running) {
        FuriStatus status = furi_message_queue_get(a->queue, &ev, a->playing ? furi_ms_to_ticks(10) : FuriWaitForever);
        furi_mutex_acquire(a->mutex, FuriWaitForever);
        if(status != FuriStatusOk) {
            playback_update(a);
            furi_mutex_release(a->mutex); view_port_update(a->viewport); continue;
        }
        a->notice[0] = '\0';
        if(a->playing) {
            if(ev.key == InputKeyBack && ev.type == InputTypeShort) {playback_stop(a); snprintf(a->notice, sizeof(a->notice), "Playback stopped");}
            furi_mutex_release(a->mutex); view_port_update(a->viewport); continue;
        }
        if(a->screen == ScreenScore) {
            if(ev.key == InputKeyBack && ev.type == InputTypeShort) a->running = false;
            else if(ev.key == InputKeyOk && ev.type == InputTypeLong) {a->menu = 0; a->menu_top = 0; a->screen = ScreenMenu;}
            else if(ev.type == InputTypeShort) {
                if(ev.key == InputKeyOk) {a->adding_rest = false; a->screen = ScreenPitch;}
                else if(ev.key == InputKeyLeft && a->selected > 0) a->selected--;
                else if(ev.key == InputKeyRight && a->selected + 1 < a->count) a->selected++;
                else if(ev.key == InputKeyUp && a->count && !a->notes[a->selected].rest && a->notes[a->selected].pitch < 95) a->notes[a->selected].pitch++;
                else if(ev.key == InputKeyDown && a->count && !a->notes[a->selected].rest && a->notes[a->selected].pitch > 48) a->notes[a->selected].pitch--;
            }
        } else if(a->screen == ScreenPitch) {
            if(ev.key == InputKeyBack && ev.type == InputTypeShort) a->screen = ScreenScore;
            else if(ev.type == InputTypeShort) {
                if(ev.key == InputKeyUp && a->pitch < 95) a->pitch++;
                else if(ev.key == InputKeyDown && a->pitch > 48) a->pitch--;
                else if(ev.key == InputKeyOk) a->screen = ScreenDuration;
            }
        } else if(a->screen == ScreenDuration) {
            if(ev.key == InputKeyBack && ev.type == InputTypeShort) a->screen = a->adding_rest ? ScreenScore : ScreenPitch;
            else if(ev.type == InputTypeShort) {
                if(ev.key == InputKeyUp && a->duration > 0) a->duration--;
                else if(ev.key == InputKeyDown && a->duration < 4) a->duration++;
                else if(ev.key == InputKeyOk) {insert_note(a); a->screen = ScreenScore;}
            }
        } else if(a->screen == ScreenTime) {
            if(ev.key == InputKeyBack && ev.type == InputTypeShort) a->screen = ScreenScore;
            else if(ev.type == InputTypeShort) {
                if(ev.key == InputKeyUp && a->time_choice > 0) a->time_choice--;
                else if(ev.key == InputKeyDown && a->time_choice < 3) a->time_choice++;
                else if(ev.key == InputKeyOk) {a->beats = time_beats[a->time_choice]; a->beat_type = time_denoms[a->time_choice]; a->screen = ScreenScore;}
            }
        } else if(a->screen == ScreenKey) {
            if(ev.key == InputKeyBack && ev.type == InputTypeShort) a->screen = ScreenScore;
            else if(ev.type == InputTypeShort) {
                if(ev.key == InputKeyUp && a->fifths < 7) a->fifths++;
                else if(ev.key == InputKeyDown && a->fifths > -7) a->fifths--;
                else if(ev.key == InputKeyLeft || ev.key == InputKeyRight) a->minor = !a->minor;
                else if(ev.key == InputKeyOk) a->screen = ScreenScore;
            }
        } else if(a->screen == ScreenTempo) {
            if(ev.key == InputKeyBack && ev.type == InputTypeShort) a->screen = ScreenScore;
            else if(ev.type == InputTypeShort) {
                if(ev.key == InputKeyUp && a->tempo < 240) a->tempo += 5;
                else if(ev.key == InputKeyDown && a->tempo > 40) a->tempo -= 5;
                else if(ev.key == InputKeyOk) a->screen = ScreenScore;
            }
        } else if(a->screen == ScreenMenu) {
            if(ev.key == InputKeyBack && ev.type == InputTypeShort) a->screen = ScreenScore;
            else if(ev.type == InputTypeShort) {
                if(ev.key == InputKeyUp && a->menu > 0) a->menu--;
                else if(ev.key == InputKeyDown && a->menu + 1 < MENU_COUNT) a->menu++;
                else if(ev.key == InputKeyOk) do_menu(a);
                if(a->menu < a->menu_top) a->menu_top = a->menu;
                if(a->menu >= a->menu_top + 4) a->menu_top = a->menu - 3;
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
