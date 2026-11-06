/* about — the About screen and its intro for the About screen.
 *
 * WHY A SEPARATE COMPONENT. None of this belongs to the bridge's actual job:
 * it is a demoscene-style intro, it only lives while the About screen is open,
 * and it changes often. Keeping it in its own component keeps that churn out of
 * the files that carry the clock, and makes it obvious where the engineering
 * ends and the fun begins.
 *
 * WHY IT DOES NOT GO THROUGH LVGL. An early version was built from LVGL
 * objects: thirty-seven rectangles and labels, moved twenty times per second.
 * It ran at about 5 frames per second with core 1 pegged, because every object
 * invalidates an area, and every area must be recomposed by blending what lies
 * beneath and re-rasterising glyphs. A retained-object toolkit is built for
 * interfaces that change rarely, not for pixel animation.
 *
 * A demo writes straight into a framebuffer and pushes it out once. That is the
 * whole difference.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>    /* uint16_t: used by about_set_texture */

#ifdef __cplusplus
extern "C" {
#endif

/* Called when the intro ends, from the About task.
 * NOTE: this does NOT run on the LVGL task, so an implementation must take
 * lvgl_port_lock() before touching LVGL objects. */
typedef void (*about_done_fn)(void);

/* Start the intro. Returns immediately: the work happens in a dedicated task
 * which holds the LVGL lock for the whole duration, so the LVGL task cannot
 * touch the panel while we draw.
 *
 * The intro ends on the first TOUCH, or after a safety timeout — if touch
 * input ever failed, the screen would not become a trap.
 *
 * Returns false if it is already running, or if the task fails to start. */
bool about_start(about_done_fn on_done);

/* The image the intro reveals: the author's portrait, uncovered by the scan
 * bar. RGB565, same encoding as the framebuffer.
 *
 * PASSED IN FROM OUTSIDE rather than included here: the image lives in
 * settings_ui/assets, and this component must not depend on settings_ui — it stays a
 * component you can read, or delete, without touching the rest. The caller
 * knows where the image is; the intro only knows what to do with it.
 *
 * Without a texture the intro has nothing to show and stays on its
 * background colour. */
void about_set_texture(const uint16_t *rgb565, int w, int h);


/* True while the intro is on screen. */
bool about_running(void);

/* Ask the intro to close (for instance when the screen is left by another
 * route). Does not block: the intro shuts down on the next frame. */
void about_stop(void);


#ifdef __cplusplus
}
#endif
