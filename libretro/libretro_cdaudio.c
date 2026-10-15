
#if defined(HAVE_CDAUDIO)
#include <libretro_file.h>
#include <string/stdstring.h>
#include <audio/conversion/s16_to_float.h>
#include <audio/conversion/float_to_s16.h>

#include <core_audio_mixer.h>
#endif

#include "../game/q_shared.h"
#include "../client/cdaudio.h"

#if defined(HAVE_CDAUDIO)
extern char g_music_dir[1024];
extern bool cdaudio_enabled;

static core_audio_mixer_sound_t *cdaudio_sound = NULL;
static core_audio_mixer_voice_t *cdaudio_voice = NULL;
static bool cdaudio_playing                    = false;

static void cdaudio_reset(void)
{
   if (cdaudio_voice)
      core_audio_mixer_stop(cdaudio_voice);

   if (cdaudio_sound)
      core_audio_mixer_destroy(cdaudio_sound);

   cdaudio_sound   = NULL;
   cdaudio_voice   = NULL;
   cdaudio_playing = false;
}

void cdaudio_stop_cb(core_audio_mixer_sound_t *sound, unsigned reason)
{
   if ((sound != cdaudio_sound) ||
       (reason == CORE_AUDIO_MIXER_SOUND_REPEATED))
      return;

   cdaudio_playing = false;
}
#endif

int CDAudio_Init(void)
{
#if defined(HAVE_CDAUDIO)
   core_audio_mixer_init(AUDIO_SAMPLE_RATE);
   cdaudio_reset();
   return 1;
#else
   return 0;
#endif
}

void CDAudio_Shutdown(void)
{
#if defined(HAVE_CDAUDIO)
   cdaudio_reset();
   core_audio_mixer_done();
#endif
}

void CDAudio_Play(int track, qboolean looping)
{
#if defined(HAVE_CDAUDIO)
   char file_name[32];
   char file_path[1024];
   void *file_contents = NULL;
   int64_t file_len    = 0;

   file_name[0] = '\0';
   file_path[0] = '\0';

   /* Stop any existing playback */
   cdaudio_reset();

   /* Sanity check */
   if (!cdaudio_enabled ||
       string_is_empty(g_music_dir))
      goto error;

#if defined(HAVE_STB_VORBIS)
   /* Get track file path
    * > Try XX.ogg */
   snprintf(file_name, sizeof(file_name), "%02i.ogg", track);
   fill_pathname_join(file_path, g_music_dir, file_name, sizeof(file_path));

   if (!path_is_valid(file_path))
   {
      /* Try trackXX.ogg */
      snprintf(file_name, sizeof(file_name), "track%02i.ogg", track);
      fill_pathname_join(file_path, g_music_dir, file_name, sizeof(file_path));

      if (!path_is_valid(file_path))
         goto error;
   }
#elif defined(HAVE_RWAV)
   /* Get track file path
    * > Try XX.ogg */
   snprintf(file_name, sizeof(file_name), "%02i.wav", track);
   fill_pathname_join(file_path, g_music_dir, file_name, sizeof(file_path));

   if (!path_is_valid(file_path))
   {
      /* Try trackXX.ogg */
      snprintf(file_name, sizeof(file_name), "track%02i.wav", track);
      fill_pathname_join(file_path, g_music_dir, file_name, sizeof(file_path));

      if (!path_is_valid(file_path))
         goto error;
   }
#endif
   /* Load track file */
   if (filestream_read_file(file_path, &file_contents, &file_len))
   {
      if (file_len < 1)
         goto error;

#if defined(HAVE_STB_VORBIS)
      cdaudio_sound = core_audio_mixer_load_ogg(file_contents, file_len);
#elif defined(HAVE_RWAV)
#if defined (HAVE_NEAREST_RESAMPLER)
      cdaudio_sound = core_audio_mixer_load_wav(file_contents, file_len, nearest_resampler.ident, RESAMPLER_QUALITY_LOWEST);
#else
      cdaudio_sound = core_audio_mixer_load_wav(file_contents, file_len, sinc_resampler.ident, RESAMPLER_QUALITY_NORMAL);
#endif
#endif

      if (!cdaudio_sound)
         goto error;

      /* Start 'playing' track file */
      cdaudio_voice = core_audio_mixer_play(cdaudio_sound,
#if defined (HAVE_NEAREST_RESAMPLER)
            looping, 1.0f, nearest_resampler.ident, RESAMPLER_QUALITY_LOWEST,
#else
            looping, 1.0f, sinc_resampler.ident, RESAMPLER_QUALITY_NORMAL,
#endif
            cdaudio_stop_cb);

      if (!cdaudio_voice)
      {
         core_audio_mixer_destroy(cdaudio_sound);
         cdaudio_sound = NULL;
         /* core_audio_mixer_destroy() will free file_contents */
         file_contents = NULL;
      }

      cdaudio_playing = true;
   }

   return;

error:
   if (file_contents)
      free(file_contents);
#endif
}

void CDAudio_Stop(void)
{
#if defined(HAVE_CDAUDIO)
   cdaudio_reset();
#endif
}

void CDAudio_Update(void)
{
}

void CDAudio_Mix(int16_t *buffer, size_t num_frames, float volume)
{
#if defined(HAVE_CDAUDIO)
   static float fbuf[AUDIO_BUFFER_SIZE];

   if (!cdaudio_playing ||
       !buffer ||
       (num_frames > AUDIO_BUFFER_SIZE >> 1))
      return;

   /* Convert input audio buffer to float */
   convert_s16_to_float(fbuf, buffer, num_frames * 2, 1.0f);

   /* Mix in CD audio */
   core_audio_mixer_mix(fbuf, num_frames, volume, true);

   /* Convert back to signed integer */
   convert_float_to_s16(buffer, fbuf, num_frames * 2);
#endif
}

qboolean CDAudio_Playing(void)
{
#if defined(HAVE_CDAUDIO)
   return (qboolean)cdaudio_playing;
#else
   return false;
#endif
}
