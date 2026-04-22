/**
 * @file audiounit.c  AudioUnit sound driver
 *
 * Copyright (C) 2010 Alfred E. Heggestad
 */
#include <AudioUnit/AudioUnit.h>
#include <AudioToolbox/AudioToolbox.h>
#if TARGET_OS_IPHONE
#include <pthread.h>
#endif
#include <re.h>
#include <rem.h>
#include <baresip.h>
#include "audiounit.h"


/**
 * @defgroup audiounit audiounit
 *
 * Audio driver module for OSX/iOS AudioUnit
 */


#define MAX_NB_FRAMES 4096


struct conv_buf {
	void *mem[2];
	uint8_t mem_idx;
	uint32_t nb_frames;
};

AudioComponent audiounit_comp_io;
AudioComponent audiounit_comp_conv;

static struct auplay *auplay;
static struct ausrc *ausrc;


static void conv_buf_destructor(void *arg)
{
	struct conv_buf *buf = (struct conv_buf *)arg;

	mem_deref(buf->mem[0]);
	mem_deref(buf->mem[1]);
}


int audiounit_conv_buf_alloc(struct conv_buf **bufp, size_t framesz)
{
	struct conv_buf *buf;

	if (!bufp)
		return EINVAL;

	buf = mem_zalloc(sizeof(*buf), conv_buf_destructor);
	if (!buf)
		return ENOMEM;

	buf->mem_idx = 0;
	buf->nb_frames = 0;
	buf->mem[0] = mem_alloc(MAX_NB_FRAMES * framesz, NULL);
	buf->mem[1] = mem_alloc(MAX_NB_FRAMES * framesz, NULL);

	*bufp = buf;

	return 0;
}


int  audiounit_get_nb_frames(struct conv_buf *buf, uint32_t *nb_frames)
{
	if (!buf)
		return EINVAL;

	*nb_frames = buf->nb_frames;

	return 0;
}


OSStatus init_data_write(struct conv_buf *buf, void **data,
			 size_t framesz, uint32_t nb_frames)
{
	uint32_t mem_idx = buf->mem_idx;

	if (buf->nb_frames + nb_frames > MAX_NB_FRAMES) {
		return kAudioUnitErr_TooManyFramesToProcess;
	}

	*data = (uint8_t*)buf->mem[mem_idx] +
		buf->nb_frames * framesz;

	buf->nb_frames = buf->nb_frames + nb_frames;

	return noErr;
}


OSStatus init_data_read(struct conv_buf *buf, void **data,
			size_t framesz, uint32_t nb_frames)
{
	uint8_t *src;
	uint32_t delta = 0;
	uint32_t mem_idx = buf->mem_idx;

	if (buf->nb_frames < nb_frames) {
		return kAudioUnitErr_TooManyFramesToProcess;
	}

	*data = buf->mem[mem_idx];

	delta = buf->nb_frames - nb_frames;

	src = (uint8_t *)buf->mem[mem_idx] + nb_frames * framesz;

	memcpy(buf->mem[(mem_idx+1)%2],
	       (void *)src, delta * framesz);

	buf->mem_idx = (mem_idx + 1)%2;
	buf->nb_frames = delta;

	return noErr;
}


uint32_t audiounit_aufmt_to_formatflags(enum aufmt fmt)
{
	switch (fmt) {

	case AUFMT_S16LE:  return kLinearPCMFormatFlagIsSignedInteger;
	case AUFMT_S24_3LE:return kLinearPCMFormatFlagIsSignedInteger;
	case AUFMT_FLOAT:  return kLinearPCMFormatFlagIsFloat;
	default: return 0;
	}
}


static int module_init(void)
{
	AudioComponentDescription desc;
	CFStringRef name = NULL;
	int err;

	desc.componentType = kAudioUnitType_Output;
#if TARGET_OS_IPHONE
	/*
	 * VoiceProcessingIO is the canonical iOS VoIP AU and
	 * pairs with the `.voiceChat` AVAudioSession mode we set in the
	 * CallKit manager. Player and recorder both route through the
	 * shared instance managed by audiounit_holder_* (see below) so
	 * we never end up with two VPIO AUs fighting over the hardware.
	 */
	desc.componentSubType = kAudioUnitSubType_VoiceProcessingIO;
#else
	desc.componentSubType = kAudioUnitSubType_HALOutput;
#endif
	desc.componentManufacturer = kAudioUnitManufacturer_Apple;
	desc.componentFlags = 0;
	desc.componentFlagsMask = 0;

	audiounit_comp_io = AudioComponentFindNext(NULL, &desc);
	if (!audiounit_comp_io) {
#if TARGET_OS_IPHONE
		warning("audiounit: Voice Processing I/O not found\n");
#else
		warning("audiounit: AUHAL not found\n");
#endif
		return ENOENT;
	}

	if (0 == AudioComponentCopyName(audiounit_comp_io, &name)) {
		debug("audiounit: using component '%s'\n",
		      CFStringGetCStringPtr(name, kCFStringEncodingUTF8));
	}

	desc.componentType = kAudioUnitType_FormatConverter;
	desc.componentSubType = kAudioUnitSubType_AUConverter;
	desc.componentManufacturer = kAudioUnitManufacturer_Apple;
	desc.componentFlags = 0;
	desc.componentFlagsMask = 0;

	audiounit_comp_conv = AudioComponentFindNext(NULL, &desc);
	if (!audiounit_comp_conv) {
		warning("audiounit: AU Converter not found\n");
		return ENOENT;
	}

	if (0 == AudioComponentCopyName(audiounit_comp_conv, &name)) {
		debug("audiounit: using component '%s'\n",
		      CFStringGetCStringPtr(name, kCFStringEncodingUTF8));
	}

	err  = auplay_register(&auplay, baresip_auplayl(),
			       "audiounit", audiounit_player_alloc);
	err |= ausrc_register(&ausrc, baresip_ausrcl(),
			      "audiounit", audiounit_recorder_alloc);

	return err;
}


static int module_close(void)
{
	ausrc  = mem_deref(ausrc);
	auplay = mem_deref(auplay);

	return 0;
}


EXPORT_SYM const struct mod_export DECL_EXPORTS(audiounit) = {
	"audiounit",
	"audio",
	module_init,
	module_close,
};


#if TARGET_OS_IPHONE

/* Shared VPIO holder — see audiounit.h for design notes. */

struct au_holder {
	AudioComponentInstance au;
	int refcount;
	bool started;
	pthread_mutex_t lock;
};

static struct au_holder s_holder = {
	.au       = NULL,
	.refcount = 0,
	.started  = false,
	.lock     = PTHREAD_MUTEX_INITIALIZER,
};


/*
 * Suspend the AU while held — Stop + Uninitialize so property sets that
 * are illegal on a running+initialized AU (most critically
 * kAudioOutputUnitProperty_SetInputCallback, which iOS silently ignores
 * if applied post-Initialize) become legal again. Caller must hold
 * s_holder.lock and must check s_holder.started == true beforehand.
 *
 * Mirrors the stop/reset/restart cycle Linphone's msiounit.mm runs when
 * a format or callback changes after AudioUnitInitialize.
 */
static void holder_suspend_locked(void)
{
	if (!s_holder.au || !s_holder.started)
		return;
	AudioOutputUnitStop(s_holder.au);
	AudioUnitUninitialize(s_holder.au);
	s_holder.started = false;
}


/*
 * Re-Initialize + Start after a suspend. On failure leaves
 * s_holder.started = false and s_holder.au intact so a later
 * audiounit_holder_start() can retry from a clean state.
 */
static int holder_resume_locked(void)
{
	OSStatus ret;

	if (!s_holder.au)
		return EINVAL;

	ret = AudioUnitInitialize(s_holder.au);
	if (ret) {
		warning("audiounit: holder: resume Initialize failed (%d)\n",
			ret);
		return ENODEV;
	}

	ret = AudioOutputUnitStart(s_holder.au);
	if (ret) {
		warning("audiounit: holder: resume Start failed (%d)\n", ret);
		AudioUnitUninitialize(s_holder.au);
		return ENODEV;
	}

	s_holder.started = true;
	return 0;
}


static int holder_create_locked(void)
{
	const AudioUnitElement outputBus = 0;
	const AudioUnitElement inputBus  = 1;
	const UInt32 enable = 1;
	OSStatus ret;

	ret = AudioComponentInstanceNew(audiounit_comp_io, &s_holder.au);
	if (ret) {
		warning("audiounit: holder: InstanceNew failed (%d)\n", ret);
		s_holder.au = NULL;
		return ENODEV;
	}

	/*
	 * Enable both IO scopes up front. VPIO is full-duplex — enabling
	 * only one scope here and then letting the other filter enable
	 * its scope later works too, but enabling both eagerly keeps the
	 * configure-before-initialize contract simple: by the time the
	 * first filter calls _start(), whichever scope the second filter
	 * will populate later is already enabled.
	 */
	ret = AudioUnitSetProperty(s_holder.au,
				   kAudioOutputUnitProperty_EnableIO,
				   kAudioUnitScope_Output, outputBus,
				   &enable, sizeof(enable));
	if (ret) {
		warning("audiounit: holder: EnableIO(output) failed (%d)\n",
			ret);
		goto fail;
	}

	ret = AudioUnitSetProperty(s_holder.au,
				   kAudioOutputUnitProperty_EnableIO,
				   kAudioUnitScope_Input, inputBus,
				   &enable, sizeof(enable));
	if (ret) {
		warning("audiounit: holder: EnableIO(input) failed (%d)\n",
			ret);
		goto fail;
	}

	info("audiounit: holder: shared VPIO instance created\n");
	return 0;

fail:
	AudioComponentInstanceDispose(s_holder.au);
	s_holder.au = NULL;
	return ENODEV;
}


int audiounit_holder_acquire(AudioComponentInstance *aup)
{
	int err = 0;

	if (!aup)
		return EINVAL;

	pthread_mutex_lock(&s_holder.lock);

	if (s_holder.refcount == 0) {
		err = holder_create_locked();
		if (err)
			goto out;
	}

	s_holder.refcount++;
	*aup = s_holder.au;

out:
	pthread_mutex_unlock(&s_holder.lock);
	return err;
}


int audiounit_holder_set_render_cb(const AURenderCallbackStruct *cb)
{
	const AudioUnitElement outputBus = 0;
	OSStatus ret;
	bool was_started;
	int err = 0;

	if (!cb)
		return EINVAL;

	pthread_mutex_lock(&s_holder.lock);

	if (!s_holder.au) {
		err = EINVAL;
		goto out;
	}

	was_started = s_holder.started;
	if (was_started)
		holder_suspend_locked();

	ret = AudioUnitSetProperty(s_holder.au,
				   kAudioUnitProperty_SetRenderCallback,
				   kAudioUnitScope_Input, outputBus,
				   cb, sizeof(*cb));
	if (ret) {
		warning("audiounit: holder: SetRenderCallback failed (%d)\n",
			ret);
		err = ENODEV;
	}

	if (was_started) {
		int rerr = holder_resume_locked();
		if (!err)
			err = rerr;
	}

out:
	pthread_mutex_unlock(&s_holder.lock);
	return err;
}


int audiounit_holder_set_input_cb(const AURenderCallbackStruct *cb)
{
	const AudioUnitElement inputBus = 1;
	OSStatus ret;
	bool was_started;
	int err = 0;

	if (!cb)
		return EINVAL;

	pthread_mutex_lock(&s_holder.lock);

	if (!s_holder.au) {
		err = EINVAL;
		goto out;
	}

	was_started = s_holder.started;
	if (was_started)
		holder_suspend_locked();

	ret = AudioUnitSetProperty(s_holder.au,
				   kAudioOutputUnitProperty_SetInputCallback,
				   kAudioUnitScope_Global, inputBus,
				   cb, sizeof(*cb));
	if (ret) {
		warning("audiounit: holder: SetInputCallback failed (%d)\n",
			ret);
		err = ENODEV;
	}

	if (was_started) {
		int rerr = holder_resume_locked();
		if (!err)
			err = rerr;
	}

out:
	pthread_mutex_unlock(&s_holder.lock);
	return err;
}


int audiounit_holder_set_output_format(const AudioStreamBasicDescription *fmt)
{
	const AudioUnitElement outputBus = 0;
	OSStatus ret;
	bool was_started;
	int err = 0;

	if (!fmt)
		return EINVAL;

	pthread_mutex_lock(&s_holder.lock);

	if (!s_holder.au) {
		err = EINVAL;
		goto out;
	}

	was_started = s_holder.started;
	if (was_started)
		holder_suspend_locked();

	/*
	 * Player-side format: what baresip feeds in on the AU's input
	 * scope of the output bus, to be rendered to the speaker.
	 */
	ret = AudioUnitSetProperty(s_holder.au,
				   kAudioUnitProperty_StreamFormat,
				   kAudioUnitScope_Input, outputBus,
				   fmt, sizeof(*fmt));
	if (ret) {
		warning("audiounit: holder: set output format failed (%d)\n",
			ret);
		err = ENODEV;
	}

	if (was_started) {
		int rerr = holder_resume_locked();
		if (!err)
			err = rerr;
	}

out:
	pthread_mutex_unlock(&s_holder.lock);
	return err;
}


int audiounit_holder_set_input_format(const AudioStreamBasicDescription *fmt)
{
	const AudioUnitElement inputBus = 1;
	OSStatus ret;
	bool was_started;
	int err = 0;

	if (!fmt)
		return EINVAL;

	pthread_mutex_lock(&s_holder.lock);

	if (!s_holder.au) {
		err = EINVAL;
		goto out;
	}

	was_started = s_holder.started;
	if (was_started)
		holder_suspend_locked();

	/*
	 * Recorder-side format: what the AU delivers on the output
	 * scope of the input bus, i.e. the mic samples baresip reads.
	 */
	ret = AudioUnitSetProperty(s_holder.au,
				   kAudioUnitProperty_StreamFormat,
				   kAudioUnitScope_Output, inputBus,
				   fmt, sizeof(*fmt));
	if (ret) {
		warning("audiounit: holder: set input format failed (%d)\n",
			ret);
		err = ENODEV;
	}

	if (was_started) {
		int rerr = holder_resume_locked();
		if (!err)
			err = rerr;
	}

out:
	pthread_mutex_unlock(&s_holder.lock);
	return err;
}


int audiounit_holder_start(void)
{
	OSStatus ret;
	int err = 0;

	pthread_mutex_lock(&s_holder.lock);

	if (!s_holder.au) {
		err = EINVAL;
		goto out;
	}

	if (s_holder.started)
		goto out;

	ret = AudioUnitInitialize(s_holder.au);
	if (ret) {
		warning("audiounit: holder: Initialize failed (%d)\n", ret);
		err = ENODEV;
		goto out;
	}

	ret = AudioOutputUnitStart(s_holder.au);
	if (ret) {
		warning("audiounit: holder: Start failed (%d)\n", ret);
		AudioUnitUninitialize(s_holder.au);
		err = ENODEV;
		goto out;
	}

	s_holder.started = true;
	info("audiounit: holder: shared VPIO instance started\n");

out:
	pthread_mutex_unlock(&s_holder.lock);
	return err;
}


void audiounit_holder_release(void)
{
	pthread_mutex_lock(&s_holder.lock);

	if (s_holder.refcount == 0) {
		pthread_mutex_unlock(&s_holder.lock);
		return;
	}

	s_holder.refcount--;

	if (s_holder.refcount > 0) {
		pthread_mutex_unlock(&s_holder.lock);
		return;
	}

	if (s_holder.au) {
		if (s_holder.started) {
			AudioOutputUnitStop(s_holder.au);
			AudioUnitUninitialize(s_holder.au);
		}
		AudioComponentInstanceDispose(s_holder.au);
		s_holder.au = NULL;
	}
	s_holder.started = false;

	info("audiounit: holder: shared VPIO instance disposed\n");

	pthread_mutex_unlock(&s_holder.lock);
}

#endif /* TARGET_OS_IPHONE */
