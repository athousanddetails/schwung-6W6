/*
 * sd606_engine.h — 6W6 voice orchestration.
 *
 * The DSP itself is the vendored 606-Inspired-Synth-Drums headers (MIT,
 * Matthew Fecher / AudioKit Pro), untouched. This layer is everything the
 * upstream README hands back to the host: pot mapping, per-voice drive and
 * distortion, accent, hi-hat choke, per-lane mutes and the master stage.
 *
 * Realtime contract: every entry point here runs on the SPI callback. Nothing
 * below allocates, opens a file or takes a lock after sd606_create().
 *
 * GPL-3.0.
 */
#ifndef SD606_ENGINE_H
#define SD606_ENGINE_H

#include <stddef.h>

/* Lane order. This is the pad order, the state blob order and the mute-bit
 * order — it is TR-606 front-panel order with the clap appended, and changing
 * it breaks saved patches. */
typedef enum {
    SD606_BD = 0, SD606_SD, SD606_LT, SD606_HT,
    SD606_CH, SD606_OH, SD606_CY, SD606_CP,
    SD606_NUM_VOICES
} sd606_voice_t;

/* Velocity has no threshold: level follows it all the way up, reaching the
 * Accent gain at 127. See the comment in sd606_trigger. */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sd606_engine sd606_engine_t;

sd606_engine_t *sd606_create(float sample_rate);
void  sd606_destroy(sd606_engine_t *e);

/* velocity 0 is treated as a note-off and ignored (drums are one-shots). */
void  sd606_trigger(sd606_engine_t *e, int voice, int velocity);

/* Mono float render. The plugin wrapper does the int16 stereo interleave. */
void  sd606_render(sd606_engine_t *e, float *out, int frames);

/* Both return 1 on a recognised key, 0 otherwise. */
int   sd606_set_param(sd606_engine_t *e, const char *key, const char *val);
int   sd606_get_param(sd606_engine_t *e, const char *key, char *buf, int len);

/* Bit n = lane n muted. Muted lanes swallow triggers and stop ringing. */
void  sd606_set_mutes(sd606_engine_t *e, unsigned mask);
unsigned sd606_get_mutes(const sd606_engine_t *e);

/* State blob for the host's get_param("state") / set_param("state") cycle. */
int   sd606_serialize(const sd606_engine_t *e, char *buf, int len);
void  sd606_deserialize(sd606_engine_t *e, const char *json);

const char *sd606_voice_id(int voice);

#ifdef __cplusplus
}
#endif
#endif /* SD606_ENGINE_H */
