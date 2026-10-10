#include <furi.h>
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
#define MENU_COUNT 13

typedef struct {
    uint8_t pitch;
    uint8_t duration;
    uint8_t rest;
    uint8_t slur;
    uint8_t tie;
    uint8_t accent;
    uint8_t staccato;
    uint8_t fermata;
} Note;

typedef enum {
    ScreenScore,
    ScreenPitch,
    ScreenDuration,
    ScreenMenu,
    ScreenTime
} Screen;

typedef struct {
    FuriMutex* mutex;
    FuriMessageQueue* queue;
    ViewPort* viewport;
    Note notes[MAX_NOTES];
    uint16_t count;
    uint16_t selected;
    uint8_t pitch;
    uint8_t duration;
    uint8_t menu;
    uint8_t menu_top;
    uint8_t beats;
    uint8_t beat_type;
    uint8_t time_choice;
    bool adding_rest;
    Screen screen;
    bool running;
    char notice[22];
} AppState;

static const char* const durations[] = {
    "Whole", "Half", "Quarter", "Eighth", "16th"
};

static const char* const pitch_names[] = {
    "C", "C#", "D", "D#", "E", "F",
    "F#", "G", "G#", "A", "A#", "B"
};

static const char* const xml_steps[] = {
    "C", "C", "D", "D", "E", "F",
    "F", "G", "G", "A", "A", "B"
};

static const char* const xml_types[] = {
    "whole", "half", "quarter", "eighth", "16th"
};

static const char* const menu_items[MENU_COUNT] = {
    "Add rest",
    "Time signature",
    "Slur start",
    "Slur end",
    "Tie start",
    "Tie end",
    "Staccato",
    "Accent",
    "Fermata",
    "Save score",
    "Load score",
    "Export XML",
    "Clear score"
};

static const uint8_t time_beats[] = {2, 3, 4, 6};
static const uint8_t time_denoms[] = {4, 4, 4, 8};

static uint8_t duration_ticks(uint8_t duration) {
    return (uint8_t)(16U >> duration);
}

static uint8_t measure_ticks(const AppState* a) {
    return (uint8_t)(
        a->beats * (16U / a->beat_type));
}

static void pitch_label(
    uint8_t pitch,
    char* buffer,
    size_t size) {

    snprintf(
        buffer,
        size,
        "%s%d",
        pitch_names[pitch % 12],
        (int)pitch / 12 - 1);
}

static int note_step(uint8_t midi) {
    static const uint8_t degree[] = {
        0, 0, 1, 1, 2, 3,
        3, 4, 4, 5, 5, 6
    };

    return
        ((int)midi / 12 - 5) * 7 +
        (int)degree[midi % 12] - 2;
}

static int note_y(uint8_t midi) {
    int step = note_step(midi);

    int pixels =
        (step * STAFF_SPACING +
        (step >= 0 ? 1 : -1)) / 2;

    return STAFF_BOTTOM - pixels;
}

static void draw_staff(Canvas* canvas) {
    for(int i = 0; i < 5; ++i) {
        int y = STAFF_BOTTOM - i * STAFF_SPACING;

        canvas_draw_line(
            canvas, 2, y, 125, y);
    }

    canvas_draw_line(canvas, 12, 11, 12, 49);
    canvas_draw_circle(canvas, 12, 32, 6);
    canvas_draw_circle(canvas, 12, 39, 3);
    canvas_draw_line(canvas, 12, 11, 16, 16);
    canvas_draw_line(canvas, 16, 16, 12, 24);
    canvas_draw_line(canvas, 12, 49, 9, 53);
}

static void draw_rest(
    Canvas* canvas,
    int x,
    uint8_t duration) {

    if(duration <= 1) {
        int y = duration == 0 ? 26 : 33;

        canvas_draw_box(
            canvas, x - 4, y, 8, 3);

    } else if(duration == 2) {
        canvas_draw_line(
            canvas, x + 1, 24, x - 2, 34);

        canvas_draw_line(
            canvas, x - 2, 34, x + 3, 38);

        canvas_draw_line(
            canvas, x + 3, 38, x - 2, 43);

    } else {
        canvas_draw_line(
            canvas, x, 23, x, 40);

        canvas_draw_circle(
            canvas, x + 3, 27, 2);

        if(duration == 4) {
            canvas_draw_circle(
                canvas, x + 3, 34, 2);
        }
    }
}

static void draw_note(
    Canvas* canvas,
    int x,
    Note note,
    bool selected) {

    if(note.rest) {
        draw_rest(
            canvas, x, note.duration);

        if(selected) {
            canvas_draw_frame(
                canvas, x - 6, 20, 13, 26);
        }

        return;
    }

    int y = note_y(note.pitch);
    int step = note_step(note.pitch);

    for(
        int s = -2;
        s >= step && s >= -14;
        s -= 2) {

        int ly =
            STAFF_BOTTOM -
            (s * STAFF_SPACING) / 2;

        canvas_draw_line(
            canvas, x - 6, ly, x + 6, ly);
    }

    for(
        int s = 10;
        s <= step && s <= 22;
        s += 2) {

        int ly =
            STAFF_BOTTOM -
            (s * STAFF_SPACING) / 2;

        canvas_draw_line(
            canvas, x - 6, ly, x + 6, ly);
    }

    uint8_t pc = note.pitch % 12;

    if(
        pc == 1 ||
        pc == 3 ||
        pc == 6 ||
        pc == 8 ||
        pc == 10) {

        canvas_draw_line(
            canvas, x - 8, y - 5,
            x - 8, y + 5);

        canvas_draw_line(
            canvas, x - 5, y - 5,
            x - 5, y + 5);

        canvas_draw_line(
            canvas, x - 10, y - 2,
            x - 3, y - 3);

        canvas_draw_line(
            canvas, x - 10, y + 2,
            x - 3, y + 1);
    }

    if(note.duration <= 1) {
        canvas_draw_circle(
            canvas, x, y, 3);
    } else {
        canvas_draw_disc(
            canvas, x, y, 3);
    }

    if(note.duration != 0) {
        bool down = step >= 4;

        int sx = down ? x - 3 : x + 3;
        int end = y + (down ? 17 : -17);

        canvas_draw_line(
            canvas, sx, y, sx, end);

        if(note.duration >= 3) {
            int d = down ? -1 : 1;

            canvas_draw_line(
                canvas,
                sx, end,
                sx + 5, end + d * 5);

            canvas_draw_line(
                canvas,
                sx + 5, end + d * 5,
                sx + 3, end + d * 9);

            if(note.duration == 4) {
                canvas_draw_line(
                    canvas,
                    sx, end - d * 5,
                    sx + 5, end);

                canvas_draw_line(
                    canvas,
                    sx + 5, end,
                    sx + 3, end + d * 4);
            }
        }
    }

    if(note.staccato) {
        canvas_draw_disc(
            canvas,
            x,
            y < 28 ? y + 7 : y - 7,
            1);
    }

    if(note.accent) {
        canvas_draw_line(
            canvas,
            x - 4, y - 9,
            x + 2, y - 11);

        canvas_draw_line(
            canvas,
            x + 2, y - 11,
            x - 4, y - 13);
    }

    if(note.fermata) {
        canvas_draw_circle(
            canvas, x, y - 13, 4);

        canvas_draw_disc(
            canvas, x, y - 13, 1);
    }

    if(selected) {
        canvas_draw_frame(
            canvas,
            x - 5, y - 5,
            11, 11);
    }
}

static uint8_t tick_before(
    const AppState* a,
    uint16_t index) {

    uint8_t cap = measure_ticks(a);
    uint8_t used = 0;

    for(uint16_t i = 0; i < index; ++i) {
        uint8_t ticks =
            duration_ticks(
                a->notes[i].duration);

        if(used && used + ticks > cap) {
            used = 0;
        }

        used += ticks;

        if(used >= cap) {
            used = 0;
        }
    }

    return used;
}

static void draw_callback(
    Canvas* canvas,
    void* context) {

    AppState* a = context;

    furi_mutex_acquire(
        a->mutex,
        FuriWaitForever);

    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);

    if(a->screen == ScreenScore) {
        canvas_draw_str(
            canvas, 2, 10, "Bard's Score");

        draw_staff(canvas);

        uint16_t start =
            a->selected > 3 ?
            a->selected - 3 : 0;

        uint8_t used =
            tick_before(a, start);

        uint8_t cap =
            measure_ticks(a);

        int previous_x = -1;
        int previous_y = 0;
        uint16_t previous_i = 0;

        for(
            uint16_t i = start;
            i < a->count && i < start + 6;
            ++i) {

            int x =
                35 + (i - start) * 17;

            uint8_t ticks =
                duration_ticks(
                    a->notes[i].duration);

            if(used && used + ticks > cap) {
                canvas_draw_line(
                    canvas,
                    x - 9, 19,
                    x - 9, STAFF_BOTTOM);

                used = 0;
            }

            draw_note(
                canvas,
                x,
                a->notes[i],
                i == a->selected);

            if(
                previous_x >= 0 &&
                !a->notes[i].rest &&
                !a->notes[previous_i].rest) {

                if(
                    a->notes[previous_i].slur == 1 ||
                    a->notes[i].slur == 2) {

                    canvas_draw_line(
                        canvas,
                        previous_x, 13,
                        (previous_x + x) / 2, 11);

                    canvas_draw_line(
                        canvas,
                        (previous_x + x) / 2, 11,
                        x, 13);
                }

                if(
                    a->notes[previous_i].tie == 1 &&
                    a->notes[i].tie == 2 &&
                    a->notes[previous_i].pitch ==
                    a->notes[i].pitch) {

                    int y = previous_y + 7;

                    canvas_draw_line(
                        canvas,
                        previous_x + 3, y,
                        (previous_x + x) / 2,
                        y + 2);

                    canvas_draw_line(
                        canvas,
                        (previous_x + x) / 2,
                        y + 2,
                        x - 3, y);
                }
            }

            previous_x = x;
            previous_y =
                note_y(a->notes[i].pitch);

            previous_i = i;
            used += ticks;

            if(used >= cap) {
                if(
                    i + 1 < a->count &&
                    i + 1 < start + 6) {

                    canvas_draw_line(
                        canvas,
                        x + 8, 19,
                        x + 8, STAFF_BOTTOM);
                }

                used = 0;
            }
        }

        canvas_set_font(
            canvas, FontSecondary);

        char status[42];

        if(a->notice[0]) {
            snprintf(
                status,
                sizeof(status),
                "%s",
                a->notice);

        } else if(a->count) {
            Note n =
                a->notes[a->selected];

            char pitch[8];

            if(n.rest) {
                snprintf(
                    pitch,
                    sizeof(pitch),
                    "Rest");
            } else {
                pitch_label(
                    n.pitch,
                    pitch,
                    sizeof(pitch));
            }

            snprintf(
                status,
                sizeof(status),
                "%s %s %u/%u",
                pitch,
                durations[n.duration],
                (unsigned)(a->selected + 1),
                (unsigned)a->count);

        } else {
            snprintf(
                status,
                sizeof(status),
                "OK:Add Hold OK:Tools");
        }

        canvas_draw_str(
            canvas, 2, 63, status);

    } else if(a->screen == ScreenMenu) {
        canvas_draw_str(
            canvas, 2, 11, "Score tools");

        canvas_set_font(
            canvas, FontSecondary);

        for(
            uint8_t i = a->menu_top;
            i < MENU_COUNT &&
            i < a->menu_top + 4;
            ++i) {

            char line[28];

            snprintf(
                line,
                sizeof(line),
                "%c %s",
                a->menu == i ? '>' : ' ',
                menu_items[i]);

            canvas_draw_str(
                canvas,
                3,
                23 + (i - a->menu_top) * 12,
                line);
        }

    } else {
        const char* title =
            a->screen == ScreenPitch ?
            "Choose pitch" :
            a->screen == ScreenDuration ?
            (a->adding_rest ?
             "Rest length" : "Note length") :
            "Time signature";

        canvas_draw_str(
            canvas, 2, 11, title);

        canvas_set_font(
            canvas, FontSecondary);

        char line[24];

        if(a->screen == ScreenPitch) {
            pitch_label(
                a->pitch,
                line,
                sizeof(line));

            canvas_draw_str(
                canvas, 8, 33, line);

            canvas_draw_str(
                canvas, 2, 61,
                "Up/Down OK:Next");

        } else if(a->screen == ScreenDuration) {
            canvas_draw_str(
                canvas,
                8, 33,
                durations[a->duration]);

            canvas_draw_str(
                canvas, 2, 61,
                "Up/Down OK:Insert");

        } else {
            snprintf(
                line,
                sizeof(line),
                "%u/%u",
                time_beats[a->time_choice],
                time_denoms[a->time_choice]);

            canvas_draw_str(
                canvas, 8, 33, line);

            canvas_draw_str(
                canvas, 2, 61,
                "Up/Down OK:Set");
        }
    }

    furi_mutex_release(a->mutex);
}

static void input_callback(
    InputEvent* event,
    void* context) {

    AppState* a = context;

    if(
        event->type == InputTypeShort ||
        event->type == InputTypeLong) {

        furi_message_queue_put(
            a->queue, event, 0);
    }
}

static bool file_write_text(
    File* file,
    const char* text) {

    size_t len = strlen(text);

    return storage_file_write(
        file, text, len) == len;
}

static bool write_score(
    AppState* a,
    bool xml) {

    Storage* storage =
        furi_record_open(RECORD_STORAGE);

    storage_common_mkdir(
        storage, SCORE_DIR);

    File* file =
        storage_file_alloc(storage);

    bool ok = storage_file_open(
        file,
        xml ? XML_FILE : SCORE_FILE,
        FSAM_WRITE,
        FSOM_CREATE_ALWAYS);

    if(ok) {
        char buf[512];

        if(!xml) {
            ok = file_write_text(
                file, "BSC2\n");

            snprintf(
                buf,
                sizeof(buf),
                "%u %u\n",
                a->beats,
                a->beat_type);

            if(ok) {
                ok = file_write_text(
                    file, buf);
            }

            for(
                uint16_t i = 0;
                i < a->count && ok;
                ++i) {

                Note n = a->notes[i];

                snprintf(
                    buf,
                    sizeof(buf),
                    "%u %u %u %u %u %u %u %u\n",
                    n.pitch,
                    n.duration,
                    n.rest,
                    n.slur,
                    n.tie,
                    n.accent,
                    n.staccato,
                    n.fermata);

                ok = file_write_text(
                    file, buf);
            }

        } else {
            ok = file_write_text(
                file,
                "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                "<score-partwise version=\"4.0\">"
                "<work><work-title>Bard's Melody</work-title></work>"
                "<movement-title>Bard's Melody</movement-title>"
                "<part-list><score-part id=\"P1\">"
                "<part-name>Hulusi</part-name>"
                "</score-part></part-list><part id=\"P1\">\n");

            uint16_t measure = 1;
            uint8_t used = 0;
            uint8_t cap = measure_ticks(a);
            bool open = false;

            for(
                uint16_t i = 0;
                i < a->count && ok;
                ++i) {

                Note n = a->notes[i];

                uint8_t ticks =
                    duration_ticks(n.duration);

                if(
                    open &&
                    used &&
                    used + ticks > cap) {

                    ok = file_write_text(
                        file, "</measure>\n");

                    open = false;
                    used = 0;
                }

                if(!open && ok) {
                    snprintf(
                        buf,
                        sizeof(buf),
                        "<measure implicit=\"no\" number=\"%u\">",
                        (unsigned)measure++);

                    ok = file_write_text(
                        file, buf);

                    open = true;

                    if(measure == 2 && ok) {
                        snprintf(
                            buf,
                            sizeof(buf),
                            "<attributes>"
                            "<divisions>4</divisions>"
                            "<key><fifths>0</fifths></key>"
                            "<time><beats>%u</beats>"
                            "<beat-type>%u</beat-type></time>"
                            "<clef><sign>G</sign>"
                            "<line>2</line></clef>"
                            "</attributes>",
                            a->beats,
                            a->beat_type);

                        ok = file_write_text(
                            file, buf);
                    }
                }

                if(!ok) break;

                if(n.rest) {
                    snprintf(
                        buf,
                        sizeof(buf),
                        "<note><rest/>"
                        "<duration>%u</duration>"
                        "<type>%s</type>",
                        ticks,
                        xml_types[n.duration]);

                } else {
                    uint8_t pc =
                        n.pitch % 12;

                    bool sharp =
                        pc == 1 ||
                        pc == 3 ||
                        pc == 6 ||
                        pc == 8 ||
                        pc == 10;

                    snprintf(
                        buf,
                        sizeof(buf),
                        "<note><pitch>"
                        "<step>%s</step>"
                        "%s"
                        "<octave>%d</octave>"
                        "</pitch>"
                        "<duration>%u</duration>"
                        "<type>%s</type>",
                        xml_steps[pc],
                        sharp ?
                        "<alter>1</alter>" : "",
                        (int)n.pitch / 12 - 1,
                        ticks,
                        xml_types[n.duration]);
                }

                ok = file_write_text(
                    file, buf);

                if(
                    !n.rest &&
                    ok &&
                    n.tie == 1) {

                    ok = file_write_text(
                        file,
                        "<tie type=\"start\"/>");
                }

                if(
                    !n.rest &&
                    ok &&
                    n.tie == 2) {

                    ok = file_write_text(
                        file,
                        "<tie type=\"stop\"/>");
                }

                if(
                    !n.rest &&
                    (n.slur ||
                     n.tie ||
                     n.accent ||
                     n.staccato ||
                     n.fermata) &&
                    ok) {

                    ok = file_write_text(
                        file, "<notations>");

                    if(n.slur == 1 && ok) {
                        ok = file_write_text(
                            file,
                            "<slur number=\"1\" "
                            "type=\"start\"/>");
                    }

                    if(n.slur == 2 && ok) {
                        ok = file_write_text(
                            file,
                            "<slur number=\"1\" "
                            "type=\"stop\"/>");
                    }

                    if(n.tie == 1 && ok) {
                        ok = file_write_text(
                            file,
                            "<tied type=\"start\"/>");
                    }

                    if(n.tie == 2 && ok) {
                        ok = file_write_text(
                            file,
                            "<tied type=\"stop\"/>");
                    }

                    if(
                        (n.accent || n.staccato) &&
                        ok) {

                        ok = file_write_text(
                            file,
                            "<articulations>");

                        if(n.accent && ok) {
                            ok = file_write_text(
                                file, "<accent/>");
                        }

                        if(n.staccato && ok) {
                            ok = file_write_text(
                                file, "<staccato/>");
                        }

                        if(ok) {
                            ok = file_write_text(
                                file,
                                "</articulations>");
                        }
                    }

                    if(n.fermata && ok) {
                        ok = file_write_text(
                            file, "<fermata/>");
                    }

                    if(ok) {
                        ok = file_write_text(
                            file, "</notations>");
                    }
                }

                if(ok) {
                    ok = file_write_text(
                        file, "</note>");
                }

                used += ticks;

                if(used >= cap && ok) {
                    ok = file_write_text(
                        file, "</measure>\n");

                    open = false;
                    used = 0;
                }
            }

            if(open && ok) {
                ok = file_write_text(
                    file, "</measure>\n");
            }

            if(ok) {
                ok = file_write_text(
                    file,
                    "</part></score-partwise>\n");
            }
        }

        storage_file_close(file);
    }

    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);

    return ok;
}

static bool load_score(AppState* a) {
    Storage* storage =
        furi_record_open(RECORD_STORAGE);

    File* file =
        storage_file_alloc(storage);

    bool ok = storage_file_open(
        file,
        SCORE_FILE,
        FSAM_READ,
        FSOM_OPEN_EXISTING);

    if(ok) {
        Note temp[MAX_NOTES];

        uint16_t count = 0;
        uint16_t line_no = 0;

        uint8_t beats = 4;
        uint8_t denom = 4;

        bool v2 = false;
        bool valid = true;

        char line[96];
        size_t pos = 0;

        while(valid) {
            char c;

            size_t read =
                storage_file_read(
                    file, &c, 1);

            if(read != 1) {
                if(pos == 0) break;
                c = '\n';
            }

            if(c == '\r') continue;

            if(c == '\n') {
                line[pos] = '\0';

                if(line_no == 0) {
                    if(strcmp(line, "BSC2") == 0) {
                        v2 = true;
                    } else if(
                        strcmp(line, "BSC1") != 0) {

                        valid = false;
                    }

                } else if(v2 && line_no == 1) {
                    unsigned b, d;

                    if(
                        sscanf(
                            line,
                            "%u %u",
                            &b, &d) != 2 ||
                        (!((b == 2 ||
                            b == 3 ||
                            b == 4) &&
                           d == 4) &&
                         !(b == 6 && d == 8))) {

                        valid = false;

                    } else {
                        beats = (uint8_t)b;
                        denom = (uint8_t)d;
                    }

                } else if(
                    line[0] &&
                    count < MAX_NOTES) {

                    unsigned p = 0;
                    unsigned d = 0;
                    unsigned r = 0;
                    unsigned s = 0;
                    unsigned t = 0;
                    unsigned ac = 0;
                    unsigned st = 0;
                    unsigned f = 0;

                    int matched = v2 ?
                        sscanf(
                            line,
                            "%u %u %u %u %u %u %u %u",
                            &p, &d, &r, &s,
                            &t, &ac, &st, &f) :
                        sscanf(
                            line,
                            "%u %u",
                            &p, &d);

                    if(
                        matched != (v2 ? 8 : 2) ||
                        p < 48 ||
                        p > 95 ||
                        d > 4 ||
                        r > 1 ||
                        s > 2 ||
                        t > 2 ||
                        ac > 1 ||
                        st > 1 ||
                        f > 1) {

                        valid = false;

                    } else {
                        temp[count++] = (Note){
                            (uint8_t)p,
                            (uint8_t)d,
                            (uint8_t)r,
                            (uint8_t)s,
                            (uint8_t)t,
                            (uint8_t)ac,
                            (uint8_t)st,
                            (uint8_t)f
                        };
                    }

                } else if(
                    line[0] &&
                    count >= MAX_NOTES) {

                    valid = false;
                }

                line_no++;
                pos = 0;

                if(read != 1) break;

            } else if(
                pos < sizeof(line) - 1) {

                line[pos++] = c;

            } else {
                valid = false;
            }
        }

        storage_file_close(file);

        ok = valid &&
            line_no >= (v2 ? 2 : 1);

        if(ok) {
            memcpy(
                a->notes,
                temp,
                count * sizeof(Note));

            a->count = count;
            a->selected = 0;
            a->beats = beats;
            a->beat_type = denom;

            for(uint8_t i = 0; i < 4; ++i) {
                if(
                    time_beats[i] == beats &&
                    time_denoms[i] == denom) {

                    a->time_choice = i;
                }
            }
        }
    }

    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);

    return ok;
}

static void insert_note(AppState* a) {
    if(a->count >= MAX_NOTES) {
        snprintf(
            a->notice,
            sizeof(a->notice),
            "Score full");

        return;
    }

    uint16_t at =
        a->count ?
        a->selected + 1 : 0;

    if(at < a->count) {
        memmove(
            &a->notes[at + 1],
            &a->notes[at],
            (a->count - at) * sizeof(Note));
    }

    a->notes[at] = (Note){
        .pitch = a->pitch,
        .duration = a->duration,
        .rest = a->adding_rest
    };

    a->count++;
    a->selected = at;
}

static void do_menu(AppState* a) {
    Note* n =
        a->count ?
        &a->notes[a->selected] : NULL;

    a->notice[0] = '\0';

    switch(a->menu) {
    case 0:
        a->adding_rest = true;
        a->screen = ScreenDuration;
        return;

    case 1:
        a->screen = ScreenTime;
        return;

    case 2:
        if(n && !n->rest) {
            n->slur =
                n->slur == 1 ? 0 : 1;
        }
        break;

    case 3:
        if(n && !n->rest) {
            n->slur =
                n->slur == 2 ? 0 : 2;
        }
        break;

    case 4:
        if(n && !n->rest) {
            n->tie =
                n->tie == 1 ? 0 : 1;
        }
        break;

    case 5:
        if(n && !n->rest) {
            n->tie =
                n->tie == 2 ? 0 : 2;
        }
        break;

    case 6:
        if(n && !n->rest) {
            n->staccato ^= 1;
        }
        break;

    case 7:
        if(n && !n->rest) {
            n->accent ^= 1;
        }
        break;

    case 8:
        if(n && !n->rest) {
            n->fermata ^= 1;
        }
        break;

    case 9:
        snprintf(
            a->notice,
            sizeof(a->notice),
            write_score(a, false) ?
            "Score saved" : "Save failed");
        break;

    case 10:
        snprintf(
            a->notice,
            sizeof(a->notice),
            load_score(a) ?
            "Score loaded" : "Load failed");
        break;

    case 11:
        snprintf(
            a->notice,
            sizeof(a->notice),
            write_score(a, true) ?
            "MusicXML exported" :
            "Export failed");
        break;

    case 12:
        a->count = 0;
        a->selected = 0;

        snprintf(
            a->notice,
            sizeof(a->notice),
            "Score cleared");
        break;
    }

    a->screen = ScreenScore;
}

int32_t bards_score_app(void* p) {
    UNUSED(p);

    AppState* a =
        malloc(sizeof(AppState));

    if(!a) return -1;

    memset(a, 0, sizeof(AppState));

    a->mutex =
        furi_mutex_alloc(FuriMutexTypeNormal);

    a->queue =
        furi_message_queue_alloc(
            8, sizeof(InputEvent));

    a->viewport =
        view_port_alloc();

    if(
        !a->mutex ||
        !a->queue ||
        !a->viewport) {

        if(a->viewport) {
            view_port_free(a->viewport);
        }

        if(a->queue) {
            furi_message_queue_free(a->queue);
        }

        if(a->mutex) {
            furi_mutex_free(a->mutex);
        }

        free(a);
        return -1;
    }

    a->running = true;
    a->pitch = 69;
    a->duration = 2;
    a->beats = 4;
    a->beat_type = 4;
    a->time_choice = 2;

    view_port_draw_callback_set(
        a->viewport,
        draw_callback,
        a);

    view_port_input_callback_set(
        a->viewport,
        input_callback,
        a);

    Gui* gui =
        furi_record_open(RECORD_GUI);

    gui_add_view_port(
        gui,
        a->viewport,
        GuiLayerFullscreen);

    view_port_update(a->viewport);

    InputEvent ev;

    while(a->running) {
        if(
            furi_message_queue_get(
                a->queue,
                &ev,
                FuriWaitForever) != FuriStatusOk) {

            continue;
        }

        furi_mutex_acquire(
            a->mutex,
            FuriWaitForever);

        a->notice[0] = '\0';

        if(a->screen == ScreenScore) {
            if(
                ev.key == InputKeyBack &&
                ev.type == InputTypeShort) {

                a->running = false;

            } else if(
                ev.key == InputKeyOk &&
                ev.type == InputTypeLong) {

                a->menu = 0;
                a->menu_top = 0;
                a->screen = ScreenMenu;

            } else if(ev.type == InputTypeShort) {
                if(ev.key == InputKeyOk) {
                    a->adding_rest = false;
                    a->screen = ScreenPitch;

                } else if(
                    ev.key == InputKeyLeft &&
                    a->selected > 0) {

                    a->selected--;

                } else if(
                    ev.key == InputKeyRight &&
                    a->selected + 1 < a->count) {

                    a->selected++;

                } else if(
                    ev.key == InputKeyUp &&
                    a->count &&
                    !a->notes[a->selected].rest &&
                    a->notes[a->selected].pitch < 95) {

                    a->notes[a->selected].pitch++;

                } else if(
                    ev.key == InputKeyDown &&
                    a->count &&
                    !a->notes[a->selected].rest &&
                    a->notes[a->selected].pitch > 48) {

                    a->notes[a->selected].pitch--;
                }
            }

        } else if(a->screen == ScreenPitch) {
            if(
                ev.key == InputKeyBack &&
                ev.type == InputTypeShort) {

                a->screen = ScreenScore;

            } else if(ev.type == InputTypeShort) {
                if(
                    ev.key == InputKeyUp &&
                    a->pitch < 95) {

                    a->pitch++;

                } else if(
                    ev.key == InputKeyDown &&
                    a->pitch > 48) {

                    a->pitch--;

                } else if(ev.key == InputKeyOk) {
                    a->screen = ScreenDuration;
                }
            }

        } else if(a->screen == ScreenDuration) {
            if(
                ev.key == InputKeyBack &&
                ev.type == InputTypeShort) {

                a->screen =
                    a->adding_rest ?
                    ScreenScore : ScreenPitch;

            } else if(ev.type == InputTypeShort) {
                if(
                    ev.key == InputKeyUp &&
                    a->duration > 0) {

                    a->duration--;

                } else if(
                    ev.key == InputKeyDown &&
                    a->duration < 4) {

                    a->duration++;

                } else if(ev.key == InputKeyOk) {
                    insert_note(a);
                    a->screen = ScreenScore;
                }
            }

        } else if(a->screen == ScreenTime) {
            if(
                ev.key == InputKeyBack &&
                ev.type == InputTypeShort) {

                a->screen = ScreenScore;

            } else if(ev.type == InputTypeShort) {
                if(
                    ev.key == InputKeyUp &&
                    a->time_choice > 0) {

                    a->time_choice--;

                } else if(
                    ev.key == InputKeyDown &&
                    a->time_choice < 3) {

                    a->time_choice++;

                } else if(ev.key == InputKeyOk) {
                    a->beats =
                        time_beats[a->time_choice];

                    a->beat_type =
                        time_denoms[a->time_choice];

                    a->screen = ScreenScore;
                }
            }

        } else if(a->screen == ScreenMenu) {
            if(
                ev.key == InputKeyBack &&
                ev.type == InputTypeShort) {

                a->screen = ScreenScore;

            } else if(ev.type == InputTypeShort) {
                if(
                    ev.key == InputKeyUp &&
                    a->menu > 0) {

                    a->menu--;

                } else if(
                    ev.key == InputKeyDown &&
                    a->menu + 1 < MENU_COUNT) {

                    a->menu++;

                } else if(ev.key == InputKeyOk) {
                    do_menu(a);
                }

                if(a->menu < a->menu_top) {
                    a->menu_top = a->menu;
                }

                if(a->menu >= a->menu_top + 4) {
                    a->menu_top = a->menu - 3;
                }
            }
        }

        furi_mutex_release(a->mutex);
        view_port_update(a->viewport);
    }

    gui_remove_view_port(
        gui, a->viewport);

    view_port_free(a->viewport);

    furi_record_close(RECORD_GUI);

    furi_message_queue_free(a->queue);
    furi_mutex_free(a->mutex);

    free(a);

    return 0;
}
