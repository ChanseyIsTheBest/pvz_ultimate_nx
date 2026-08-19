#ifndef PVZU_INPUT_H
#define PVZU_INPUT_H

/* Call once, before the engine spawns threads -- nx_pointer reads cursor.png
 * off the SD card during init. */
void input_init(int screen_w, int screen_h, const char *data_dir);

/* Once per frame, before lawn_do_frame. Reads the pad, touchscreen, mouse and
 * gyro, then emits the MotionEvent sequence for whatever changed. */
void input_update(void);

/* On dock/undock. Cheap when the size is unchanged. */
void input_set_screen(int w, int h);

/* Pass padGetButtonsDown/Up straight through. */
void input_handle_buttons(unsigned long long down, unsigned long long up);

/* Inside the eglSwapBuffers wrapper, after the game's draw and before the real
 * swap, so the cursor lands on the finished frame. */
void input_draw_overlay(void);

void input_shutdown(void);

#endif
