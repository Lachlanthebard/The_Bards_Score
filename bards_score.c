#include <furi.h>
#include <gui/gui.h>
#include <gui/view_port.h>
#include <input/input.h>
#include <storage/storage.h>
#include <stdio.h>
#include <string.h>

#define MAX_NOTES 256
#define SCORE_DIR EXT_PATH("apps_data/bards_score")
#define SCORE_FILE SCORE_DIR "/melody.bsc"
#define XML_FILE SCORE_DIR "/melody.musicxml"

typedef struct {
    uint8_t pitch; /* MIDI number */
    uint8_t duration; /* 0 whole, 1 half, 2 quarter, 3 eighth, 4 sixteenth */
} Note;
typedef enum {ScreenScore, ScreenPitch, ScreenDuration, ScreenMenu} Screen;
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
    Screen screen;
    bool running;
    char message[24];
} AppState;

static const char* durations[] = {"Whole", "Half", "Quarter", "Eighth", "Sixteenth"};
static const char* menu_items[] = {"Save score", "Load score", "Export MusicXML", "Clear score"};
static const char* pitch_names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
static const uint8_t duration_types[] = {1, 2, 4, 8, 16};

static void pitch_label(uint8_t pitch, char* buffer, size_t size) {
    snprintf(buffer, size, "%s%d", pitch_names[pitch % 12], (int)pitch / 12 - 1);
}
static void draw_staff(Canvas* canvas) {
    for(int i = 0; i < 5; i++) canvas_draw_line(canvas, 2, 20 + i * 7, 125, 20 + i * 7);
    /* Simplified clef marker; glyph font cannot reliably render U+1D11E */
    canvas_draw_str(canvas, 4, 17, "G");
}
static int note_y(uint8_t midi) {
    /* C4 = ledger line below staff, E4 = bottom line, F5 = top line */
    static const uint8_t semitone_to_step[] = {0, 0, 1, 1, 2, 3, 3, 4, 4, 5, 5, 6};
    int step = ((int)midi / 12 - 5) * 7 + semitone_to_step[midi % 12];
    return 54 - step * 3; /* E4 (MIDI 64) sits on bottom staff line */
}
static void draw_callback(Canvas* canvas, void* context) {
    AppState* a = context;
    furi_mutex_acquire(a->mutex, FuriWaitForever);
    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    if(a->screen == ScreenScore) {
        canvas_draw_str(canvas, 2, 10, "Bard's Score");
        draw_staff(canvas);
        uint16_t start = a->selected > 5 ? a->selected - 5 : 0;
        for(uint16_t i = start; i < a->count && i < start + 9; i++) {
            int x = 27 + (i - start) * 11;
            int y = note_y(a->notes[i].pitch) - 7;
            if(y < 16) y = 16;
            if(y > 56) y = 56;
            canvas_draw_disc(canvas, x, y, 3);
            if(a->notes[i].duration != 0) canvas_draw_line(canvas, x + 3, y, x + 3, y - 12);
            if(i == a->selected) canvas_draw_frame(canvas, x - 5, y - 5, 11, 11);
        }
        canvas_set_font(canvas, FontSecondary);
        char status[40];
        if(a->count) {
            char pitch[8]; pitch_label(a->notes[a->selected].pitch, pitch, sizeof(pitch));
            snprintf(status, sizeof(status), "%s %s %u/%u", pitch, durations[a->notes[a->selected].duration], a->selected + 1, a->count);
        } else snprintf(status, sizeof(status), "OK:Add  Hold OK:Menu");
        canvas_draw_str(canvas, 2, 63, status);
    } else {
        const char* title = a->screen == ScreenPitch ? "Choose pitch" : a->screen == ScreenDuration ? "Choose length" : "Score tools";
        canvas_draw_str(canvas, 2, 11, title);
        canvas_set_font(canvas, FontSecondary);
        char line[40];
        if(a->screen == ScreenPitch) {
            pitch_label(a->pitch, line, sizeof(line));
            snprintf(a->message, sizeof(a->message), "%s", line);
            canvas_draw_str(canvas, 8, 33, a->message);
            canvas_draw_str(canvas, 2, 61, "Up/Down: pitch  OK: next");
        } else if(a->screen == ScreenDuration) {
            canvas_draw_str(canvas, 8, 33, durations[a->duration]);
            canvas_draw_str(canvas, 2, 61, "Up/Down  OK: insert");
        } else {
            for(int i = 0; i < 4; i++) {
                snprintf(line, sizeof(line), "%c %s", a->menu == i ? '>' : ' ', menu_items[i]);
                canvas_draw_str(canvas, 3, 23 + i * 12, line);
            }
        }
    }
    furi_mutex_release(a->mutex);
}
static void input_callback(InputEvent* event, void* context) {
    AppState* a = context;
    if(event->type == InputTypeShort || event->type == InputTypeLong) furi_message_queue_put(a->queue, event, 0);
}
static bool write_score(AppState* a, bool xml) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_common_mkdir(storage, SCORE_DIR);
    File* file = storage_file_alloc(storage);
    bool ok = storage_file_open(file, xml ? XML_FILE : SCORE_FILE, FSAM_WRITE, FSOM_CREATE_ALWAYS);
    if(ok) {
        char buf[512];
        if(xml) {
            const char* head = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<!DOCTYPE score-partwise  PUBLIC \"-//Recordare//DTD MusicXML 4.0 Partwise//EN\" \"http://www.musicxml.org/dtds/partwise.dtd\">\n<score-partwise version=\"4.0\"><work><work-title>Bard's Melody</work-title></work><movement-title>Bard's Melody</movement-title><identification><creator type=\"composer\">Bard</creator></identification><defaults/><part-list><score-part id=\"P1\"><part-name>Hulusi</part-name><part-abbreviation>Hl</part-abbreviation><score-instrument id=\"I1\"><instrument-name>Hulusi</instrument-name></score-instrument></score-part></part-list><part id=\"P1\">\n";
            storage_file_write(file, head, strlen(head));
            uint16_t measure = 1;
            int used = 0;
            for(uint16_t i = 0; i < a->count; i++) {
                int ticks = 16 / duration_types[a->notes[i].duration];
                if(used == 0) {
                    snprintf(buf, sizeof(buf), "<measure implicit=\"no\" number=\"%u\">", measure++);
                    storage_file_write(file, buf, strlen(buf));
                    if(measure == 2) {
                        const char* attr = "<attributes><divisions>4</divisions><key><fifths>0</fifths></key><time><beats>4</beats><beat-type>4</beat-type></time><clef><sign>G</sign><line>2</line></clef></attributes>";
                        storage_file_write(file, attr, strlen(attr));
                    }
                }
                uint8_t pc = a->notes[i].pitch % 12;
                static const char* steps[] = {"C","C","D","D","E","F","F","G","G","A","A","B"};
                bool sharp = pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10;
                snprintf(buf, sizeof(buf), "<note><pitch><step>%s</step>%s<octave>%d</octave></pitch><duration>%d</duration><type>%s</type></note>", steps[pc], sharp ? "<alter>1</alter>" : "", (int)a->notes[i].pitch / 12 - 1, ticks, (const char*[]){"whole","half","quarter","eighth","16th"}[a->notes[i].duration]);
                storage_file_write(file, buf, strlen(buf));
                used += ticks;
                if(used >= 16) {
                    storage_file_write(file, "</measure>\n", 11);
                    used = 0;
                }
            }
            if(used) storage_file_write(file, "</measure>\n", 11);
            const char* tail = "</part></score-partwise>\n";
            storage_file_write(file, tail, strlen(tail));
        } else {
            const char* header = "BSC1\n";
            storage_file_write(file, header, strlen(header));
            for(uint16_t i = 0; i < a->count; i++) {
                snprintf(buf, sizeof(buf), "%u %u\n", a->notes[i].pitch, a->notes[i].duration);
                storage_file_write(file, buf, strlen(buf));
            }
        }
        storage_file_close(file);
    }
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
    return ok;
}
static bool load_score(AppState* a) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);
    bool ok = storage_file_open(file, SCORE_FILE, FSAM_READ, FSOM_OPEN_EXISTING);
    if(ok) {
        char line[32]; size_t pos = 0; uint16_t count = 0;
        while(true) {
            char c;
            if(storage_file_read(file, &c, 1) != 1) break;
            if(c == '\n') {
                line[pos] = 0;
                if(count == 0 && strcmp(line, "BSC1") != 0) { ok = false; break; }
                if(count > 0 && a->count < MAX_NOTES) {
                    unsigned p, d;
                    if(sscanf(line, "%u %u", &p, &d) == 2 && p >= 48 && p <= 95 && d <= 4) {
                        a->notes[a->count++] = (Note){.pitch=p, .duration=d};
                    }
                }
                count++; pos = 0;
            } else if(pos < sizeof(line) - 1) line[pos++] = c;
        }
        storage_file_close(file);
        a->selected = 0;
    }
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
    return ok;
}
int32_t bards_score_app(void* p) {
    UNUSED(p);
    AppState* a = malloc(sizeof(AppState));
    memset(a, 0, sizeof(AppState));
    a->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    a->queue = furi_message_queue_alloc(8, sizeof(InputEvent));
    a->viewport = view_port_alloc();
    a->running = true; a->pitch = 69; a->duration = 2;
    view_port_draw_callback_set(a->viewport, draw_callback, a);
    view_port_input_callback_set(a->viewport, input_callback, a);
    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, a->viewport, GuiLayerFullscreen);
    InputEvent ev;
    while(a->running) {
        if(furi_message_queue_get(a->queue, &ev, FuriWaitForever) != FuriStatusOk) continue;
        furi_mutex_acquire(a->mutex, FuriWaitForever);
        if(a->screen == ScreenScore) {
            if(ev.key == InputKeyBack) a->running = false;
            else if(ev.key == InputKeyOk && ev.type == InputTypeLong) { a->menu = 0; a->screen = ScreenMenu; }
            else if(ev.key == InputKeyOk) { a->screen = ScreenPitch; }
            else if(ev.key == InputKeyLeft && a->selected > 0) a->selected--;
            else if(ev.key == InputKeyRight && a->selected + 1 < a->count) a->selected++;
            else if(ev.key == InputKeyUp && a->count && a->notes[a->selected].pitch < 95) a->notes[a->selected].pitch++;
            else if(ev.key == InputKeyDown && a->count && a->notes[a->selected].pitch > 48) a->notes[a->selected].pitch--;
        } else if(a->screen == ScreenPitch) {
            if(ev.key == InputKeyBack) a->screen = ScreenScore;
            else if(ev.key == InputKeyUp && a->pitch < 95) a->pitch++;
            else if(ev.key == InputKeyDown && a->pitch > 48) a->pitch--;
            else if(ev.key == InputKeyOk) a->screen = ScreenDuration;
        } else if(a->screen == ScreenDuration) {
            if(ev.key == InputKeyBack) a->screen = ScreenPitch;
            else if(ev.key == InputKeyUp && a->duration > 0) a->duration--;
            else if(ev.key == InputKeyDown && a->duration < 4) a->duration++;
            else if(ev.key == InputKeyOk) {
                if(a->count < MAX_NOTES) {
                    uint16_t at = a->count ? a->selected + 1 : 0;
                    memmove(&a->notes[at + 1], &a->notes[at], (a->count - at) * sizeof(Note));
                    a->notes[at] = (Note){.pitch=a->pitch, .duration=a->duration};
                    a->count++; a->selected = at;
                }
                a->screen = ScreenScore;
            }
        } else {
            if(ev.key == InputKeyBack) a->screen = ScreenScore;
            else if(ev.key == InputKeyUp && a->menu > 0) a->menu--;
            else if(ev.key == InputKeyDown && a->menu < 3) a->menu++;
            else if(ev.key == InputKeyOk) {
                if(a->menu == 0) write_score(a, false);
                else if(a->menu == 1) { a->count = 0; load_score(a); }
                else if(a->menu == 2) write_score(a, true);
                else { a->count = 0; a->selected = 0; }
                a->screen = ScreenScore;
            }
        }
        furi_mutex_release(a->mutex);
        view_port_update(a->viewport);
    }
    gui_remove_view_port(gui, a->viewport);
    view_port_free(a->viewport);
    furi_record_close(RECORD_GUI);
    furi_message_queue_free(a->queue);
    furi_mutex_free(a->mutex);
    free(a);
    return 0;
}
