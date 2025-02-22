#include "hangman.h"

void hangman_draw_utf8_str(Canvas* canvas, uint8_t x, uint8_t y, const char* str) {
    FuriStringUTF8State state = FuriStringUTF8StateStarting;
    FuriStringUnicodeValue value = 0;

    for(; *str; str++) {
        furi_string_utf8_decode(*str, &state, &value);
        if(state == FuriStringUTF8StateError) furi_crash(NULL);

        if(state == FuriStringUTF8StateStarting) {
            canvas_draw_glyph(canvas, x, y, value);
            x += hangman_GetGlyphWidth(&canvas->fb, value);
        }
    }
}

void hangman_draw_gallows(Canvas* canvas, HangmanApp* app) {
    const Icon* gallows[HANGMAN_GALLOWS_MAX_STATE] = {&I_1, &I_2, &I_3, &I_4, &I_5, &I_6, &I_7};
    canvas_set_color(canvas, ColorBlack);
    canvas_draw_icon(canvas, 0, 30, gallows[app->gallows_state]);
    canvas_set_color(canvas, ColorWhite);
}

// This function was copied from Flipper Zero firmware
void hangman_ok_button(Canvas* canvas, uint8_t y, const char* str) {
    const Icon I_ButtonCenter_7x7 = {
        .width = 7,
        .height = 7,
        .frame_count = 1,
        .frame_rate = 0,
        .frames = (const uint8_t* const[]){
            (const uint8_t[]){0x00, 0x1c, 0x22, 0x5d, 0x5d, 0x5d, 0x22, 0x1c}}};

    const uint8_t button_height = 12;
    const uint8_t vertical_offset = 3;
    const uint8_t horizontal_offset = 1;
    const uint8_t string_width = hangman_GetGlyphWidth(&canvas->fb, ' ') * strlen(str) / 2;
    const Icon* icon = &I_ButtonCenter_7x7;
    const uint8_t icon_h_offset = 3;
    const uint8_t icon_width_with_offset = icon->width + icon_h_offset;
    const uint8_t icon_v_offset = icon->height + vertical_offset;
    const uint8_t button_width = string_width + horizontal_offset * 2 + icon_width_with_offset;

    const uint8_t x = (canvas_width(canvas) - button_width) / 2;

    canvas_draw_box(canvas, x, y - button_height, button_width, button_height);

    canvas_draw_line(canvas, x - 1, y, x - 1, y - button_height + 0);
    canvas_draw_line(canvas, x - 2, y, x - 2, y - button_height + 1);
    canvas_draw_line(canvas, x - 3, y, x - 3, y - button_height + 2);

    canvas_draw_line(canvas, x + button_width + 0, y, x + button_width + 0, y - button_height + 0);
    canvas_draw_line(canvas, x + button_width + 1, y, x + button_width + 1, y - button_height + 1);
    canvas_draw_line(canvas, x + button_width + 2, y, x + button_width + 2, y - button_height + 2);

    canvas_invert_color(canvas);
    canvas_draw_icon(canvas, x + horizontal_offset, y - icon_v_offset, &I_ButtonCenter_7x7);
    hangman_draw_utf8_str(
        canvas, x + horizontal_offset + icon_width_with_offset, y - vertical_offset, str);
    canvas_invert_color(canvas);
}

void hangman_text_window(Canvas* canvas, char* txt) {
    uint8_t x = 23, y = 18, w = 84, h = 34;

    canvas_set_color(canvas, ColorWhite);
    canvas_draw_box(canvas, x - 2, y - 2, w + 4, h + 4);
    canvas_set_color(canvas, ColorBlack);

    elements_frame(canvas, x, y, w, h);
    hangman_ok_button(canvas, y + h, "Ok");

    uint8_t txt_w = hangman_GetGlyphWidth(&canvas->fb, ' ') * strlen(txt) / 2;
    hangman_draw_utf8_str(
        canvas, (canvas_width(canvas) - txt_w) / 2, x + canvas_current_font_height(canvas), txt);
}
