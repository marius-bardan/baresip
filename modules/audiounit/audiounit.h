/**
 * @file audiounit.h  AudioUnit sound driver -- Internal interface
 *
 * Copyright (C) 2010 Alfred E. Heggestad
 */

#if __MAC_OS_X_VERSION_MAX_ALLOWED < 120000
#define kAudioObjectPropertyElementMain (kAudioObjectPropertyElementMaster)
#endif

extern AudioComponent audiounit_comp_io;
extern AudioComponent audiounit_comp_conv;


struct audiosess;
struct audiosess_st;
struct conv_buf;


typedef void (audiosess_int_h)(bool start, void *arg);

int  audiosess_alloc(struct audiosess_st **stp,
		     audiosess_int_h *inth, void *arg);
void audiosess_interrupt(bool interrupted);


int audiounit_conv_buf_alloc(struct conv_buf **bufp, size_t framesz);
int  audiounit_get_nb_frames(struct conv_buf *buf, uint32_t *nb_frames);
OSStatus init_data_write(struct conv_buf *buf, void **data,
			 size_t framesz, uint32_t nb_frames);
OSStatus init_data_read(struct conv_buf *buf, void **data,
			size_t framesz, uint32_t nb_frames);


int audiounit_player_alloc(struct auplay_st **stp, const struct auplay *ap,
			   struct auplay_prm *prm, const char *device,
			   auplay_write_h *wh, void *arg);
int audiounit_recorder_alloc(struct ausrc_st **stp, const struct ausrc *as,
			     struct ausrc_prm *prm, const char *device,
			     ausrc_read_h *rh, ausrc_error_h *errh, void *arg);


uint32_t audiounit_aufmt_to_formatflags(enum aufmt fmt);


#if TARGET_OS_IPHONE

/*
 * Shared VoiceProcessingIO holder.
 *
 * iOS VPIO is effectively single-instance-per-process — two independent
 * AudioComponentInstanceNew(VPIO) calls fight over the same mic+speaker
 * hardware and produce `render err: -1` spam. Production iOS VoIP stacks
 * (Linphone's msiounit, WebRTC's voice_processing_audio_unit) therefore
 * share a single VPIO AU between the player (render) and recorder
 * (input) filters. The holder below implements that pattern for baresip:
 *
 *   player.c  -> audiounit_holder_acquire(&st->au)
 *             -> audiounit_holder_set_render_cb(cb)
 *             -> audiounit_holder_set_output_format(fmt)
 *             -> audiounit_holder_start()
 *
 *   recorder.c -> audiounit_holder_acquire(&st->au_in)
 *              -> audiounit_holder_set_input_cb(cb)
 *              -> audiounit_holder_set_input_format(fmt)
 *              -> audiounit_holder_start()
 *
 *   both sides -> audiounit_holder_release() on teardown. Last release
 *              stops, uninitializes and disposes the shared AU.
 *
 * All calls return 0 on success, errno on failure.
 */

int audiounit_holder_acquire(AudioComponentInstance *aup);
int audiounit_holder_set_render_cb(const AURenderCallbackStruct *cb);
int audiounit_holder_set_input_cb(const AURenderCallbackStruct *cb);
int audiounit_holder_set_output_format(const AudioStreamBasicDescription *fmt);
int audiounit_holder_set_input_format(const AudioStreamBasicDescription *fmt);
int audiounit_holder_start(void);
void audiounit_holder_release(void);

#endif /* TARGET_OS_IPHONE */
