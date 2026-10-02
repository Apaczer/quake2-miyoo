/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307,
 * USA.
 *
 * =======================================================================
 *
 * This file implements an interface to libmpg123 for decoding MP3 files.
 * It converts MP3 streams into raw PCM samples which are injected into
 * the sound backends as if they were normal "raw" samples, providing
 * background music playback support.
 *
 * Usage: compile with -DMP3 and link with -lmpg123.
 *
 * =======================================================================
 */

#ifdef MP3

#ifndef _WIN32
#include <sys/time.h>
#endif

#include <errno.h>
#include <mpg123.h>

#include "../header/client.h"
#include "header/local.h"
#include "header/mp3.h"

/* ------------------------------------------------------------------ */
/* Statics                                                              */
/* ------------------------------------------------------------------ */

static cvar_t *mp3_shuffle;        /* Shuffle playback */
static cvar_t *mp3_ignoretrack0;   /* Toggle track 0 playing */
static cvar_t *mp3_volume;         /* Music volume */
static int     mp3_curfile;        /* Index of currently played file */
static int     mp3_numbufs;        /* Number of buffers for OpenAL */
static int     mp3_numsamples;     /* Number of samples read from the current file */
static mp3_status_t mp3_status;   /* Status indicator */
static mpg123_handle *mp3_handle;  /* libmpg123 handle */
static qboolean mp3_started;       /* Initialization flag */

enum { MAX_NUM_MP3TRACKS = 32 };
static char *mp3_tracks[MAX_NUM_MP3TRACKS];
static int   mp3_maxfileindex;

/* Saved state for save/restore across level transitions */
struct {
	qboolean saved;
	int curfile;
	int numsamples;
} mp3_saved_state;

/* Cached stream properties filled when a file is opened */
static long  mp3_rate;      /* sample rate reported by libmpg123 */
static int   mp3_channels;  /* channel count reported by libmpg123 */

/* ------------------------------------------------------------------ */
/* GOG track mapping (identical logic to the OGG subsystem)            */
/* ------------------------------------------------------------------ */

enum GameType_mp3 {
	mp3_other,   /* incl. baseq2 */
	mp3_xatrix,
	mp3_rogue
};

/*
 * The GOG version of Quake2 has the music tracks in music/TrackXX.mp3
 * Map CD track numbers to GOG file numbers.
 */
static int
getMappedGOGtrack_mp3(int track, enum GameType_mp3 gameType)
{
	if (track <= 0)
		return 0;

	if (track == 1)
		return 0; /* data track on CD */

	if (gameType == mp3_other)
		return track;
	if (gameType == mp3_rogue)
		return track + 10;

	/* xatrix */
	switch (track)
	{
		case  2: return 9;
		case  3: return 13;
		case  4: return 14;
		case  5: return 7;
		case  6: return 16;
		case  7: return 2;
		case  8: return 15;
		case  9: return 3;
		case 10: return 4;
		case 11: return 18;
		default: return track;
	}
}

/* ------------------------------------------------------------------ */
/* Track list                                                           */
/* ------------------------------------------------------------------ */

/*
 * Load the list of MP3 files found under the "music/" directory.
 * Mirrors OGG_InitTrackList() but searches for *.mp3 files.
 */
void
MP3_InitTrackList(void)
{
	for (int i = 0; i < MAX_NUM_MP3TRACKS; ++i)
	{
		if (mp3_tracks[i] != NULL)
		{
			free(mp3_tracks[i]);
			mp3_tracks[i] = NULL;
		}
	}

	mp3_maxfileindex = 0;

	const char *potMusicDirs[3] = {0};
	char gameMusicDir[MAX_QPATH] = {0};
	cvar_t *gameCvar = Cvar_Get("game", "", CVAR_LATCH | CVAR_SERVERINFO);

	if (gameCvar == NULL || gameCvar->string[0] == '\0' ||
	    strcmp(BASEDIRNAME, gameCvar->string) == 0)
	{
		potMusicDirs[0] = BASEDIRNAME "/music/";
		potMusicDirs[1] = "music/";
		potMusicDirs[2] = NULL;
	}
	else
	{
		snprintf(gameMusicDir, MAX_QPATH, "%s/music/", gameCvar->string);
		potMusicDirs[0] = gameMusicDir;
		potMusicDirs[1] = "music/";
		potMusicDirs[2] = BASEDIRNAME "/music/";
	}

	enum GameType_mp3 gameType = mp3_other;

	if (strcmp("xatrix", gameCvar->string) == 0)
		gameType = mp3_xatrix;
	else if (strcmp("rogue", gameCvar->string) == 0)
		gameType = mp3_rogue;

	for (int potIdx = 0;
	     potIdx < (int)(sizeof(potMusicDirs) / sizeof(potMusicDirs[0]));
	     ++potIdx)
	{
		const char *musicDir = potMusicDirs[potIdx];

		if (musicDir == NULL)
			break;

		for (const char *rawPath = FS_GetNextRawPath(NULL);
		     rawPath != NULL;
		     rawPath = FS_GetNextRawPath(rawPath))
		{
			char fullMusicPath[MAX_OSPATH] = {0};
			snprintf(fullMusicPath, MAX_OSPATH, "%s/%s", rawPath, musicDir);

			if (!Sys_IsDir(fullMusicPath))
				continue;

			char testFileName[MAX_OSPATH];

			/* Simple case: 02.mp3 … */
			snprintf(testFileName, MAX_OSPATH, "%s02.mp3", fullMusicPath);

			if (Sys_IsFile(testFileName))
			{
				mp3_tracks[2] = strdup(testFileName);

				for (int i = 3; i < MAX_NUM_MP3TRACKS; ++i)
				{
					snprintf(testFileName, MAX_OSPATH, "%s%02i.mp3",
					         fullMusicPath, i);

					if (Sys_IsFile(testFileName))
					{
						mp3_tracks[i] = strdup(testFileName);
						mp3_maxfileindex = i;
					}
				}

				return;
			}

			/* GOG case: Track02.mp3 … Track21.mp3 */
			int gogTrack = getMappedGOGtrack_mp3(8, gameType);
			snprintf(testFileName, MAX_OSPATH, "%sTrack%02i.mp3",
			         fullMusicPath, gogTrack);

			if (Sys_IsFile(testFileName))
			{
				for (int i = 2; i < MAX_NUM_MP3TRACKS; ++i)
				{
					int gt = getMappedGOGtrack_mp3(i, gameType);
					snprintf(testFileName, MAX_OSPATH, "%sTrack%02i.mp3",
					         fullMusicPath, gt);

					if (Sys_IsFile(testFileName))
					{
						mp3_tracks[i] = strdup(testFileName);
						mp3_maxfileindex = i;
					}
				}

				return;
			}
		}
	}

	Com_Printf("No MP3 music tracks have been found, so there will be no music.\n");
}

/* ------------------------------------------------------------------ */
/* Playback helpers                                                     */
/* ------------------------------------------------------------------ */

/*
 * Decode and stream a chunk of the current MP3 file.
 */
static void
MP3_Read(void)
{
	/* Output buffer: 4096 interleaved 16-bit samples */
	short samples[4096];
	size_t bytes_read = 0;

	int err = mpg123_read(mp3_handle, (unsigned char *)samples,
	                      sizeof(samples), &bytes_read);

	if (err == MPG123_OK || (err == MPG123_DONE && bytes_read > 0))
	{
		/* bytes_read / (2 bytes per sample * channels) = frames */
		int frames = (int)(bytes_read / (sizeof(short) * (size_t)mp3_channels));

		if (frames > 0)
		{
			mp3_numsamples += frames;
			S_RawSamples(frames, (int)mp3_rate, sizeof(short),
			             mp3_channels, (byte *)samples,
			             mp3_volume->value);
		}
	}

	if (err == MPG123_DONE || err == MPG123_ERR || bytes_read == 0)
	{
		/*
		 * End of file (or error): close the handle, reset state, and
		 * re-queue the same track so it loops.  We intentionally do NOT
		 * call MP3_Stop() here because that flushes the OpenAL sample
		 * queue and would cause an audible gap of ~12 seconds.
		 */
		mpg123_close(mp3_handle);
		mp3_status     = MP3_STOP;
		mp3_numbufs    = 0;
		mp3_numsamples = 0;

		MP3_PlayTrack(mp3_curfile);
	}
}

/* ------------------------------------------------------------------ */

/*
 * Called every frame to keep the audio buffers filled.
 */
void
MP3_Stream(void)
{
	if (!mp3_started)
		return;

	if (mp3_status == MP3_PLAY)
	{
#ifdef USE_OPENAL
		if (sound_started == SS_OAL)
		{
			if (mp3_numbufs == 0 || active_buffers < mp3_numbufs - 256)
				mp3_numbufs = active_buffers + 256;

			while (active_buffers <= mp3_numbufs)
				MP3_Read();
		}
		else /* SDL */
#endif
		{
			if (sound_started == SS_SDL)
			{
				while (paintedtime + MAX_RAW_SAMPLES - 2048 > s_rawend)
					MP3_Read();
			}
		}
	}
}

/* ------------------------------------------------------------------ */
/* Track control                                                        */
/* ------------------------------------------------------------------ */

/*
 * Start playing the MP3 track that corresponds to the given CD track number.
 */
void
MP3_PlayTrack(int trackNo)
{
	/* Track 0 means "stop music". */
	if (trackNo == 0)
	{
		if (mp3_ignoretrack0->value == 0)
			MP3_Stop();

		if (mp3_curfile > 0)
			return;
	}

	/* Shuffle mode or track 0 with ignoretrack0=1 → pick a random track. */
	if ((trackNo == 0) || mp3_shuffle->value)
	{
		if (mp3_maxfileindex >= 0)
		{
			trackNo = randk() % (mp3_maxfileindex + 1);
			int retries = 100;
			while (mp3_tracks[trackNo] == NULL && retries-- > 0)
				trackNo = randk() % (mp3_maxfileindex + 1);
		}
	}

	if (mp3_maxfileindex == -1)
		return; /* no mp3 files; stay silent */

	if ((trackNo < 2) || (trackNo > mp3_maxfileindex))
	{
		Com_Printf("MP3_PlayTrack: %d out of range.\n", trackNo);
		return;
	}

	if (mp3_tracks[trackNo] == NULL)
	{
		Com_Printf("MP3_PlayTrack: Don't have a .mp3 file for track %d\n",
		           trackNo);
		return;
	}

	/* If already playing this track, do nothing. */
	if (mp3_status == MP3_PLAY)
	{
		if (mp3_curfile == trackNo)
			return;
		else
			MP3_Stop();
	}

	/* Open the MP3 file with libmpg123. */
	int mErr = mpg123_open(mp3_handle, mp3_tracks[trackNo]);

	if (mErr != MPG123_OK)
	{
		Com_Printf("MP3_PlayTrack: mpg123_open('%s') failed: %s\n",
		           mp3_tracks[trackNo], mpg123_strerror(mp3_handle));
		return;
	}

	/* Read format info (sample rate, channels, encoding). */
	int encoding = 0;
	mErr = mpg123_getformat(mp3_handle, &mp3_rate, &mp3_channels, &encoding);

	if (mErr != MPG123_OK)
	{
		Com_Printf("MP3_PlayTrack: mpg123_getformat failed: %s\n",
		           mpg123_strerror(mp3_handle));
		mpg123_close(mp3_handle);
		return;
	}

	/* Force 16-bit signed output regardless of the source encoding. */
	mpg123_format_none(mp3_handle);
	mpg123_format(mp3_handle, mp3_rate, mp3_channels, MPG123_ENC_SIGNED_16);

	mp3_curfile    = trackNo;
	mp3_numsamples = 0;
	mp3_status     = MP3_PLAY;
}

/* ------------------------------------------------------------------ */
/* Stop / pause                                                         */
/* ------------------------------------------------------------------ */

/*
 * Stop MP3 playback.
 */
void
MP3_Stop(void)
{
	if (mp3_status == MP3_STOP)
		return;

#ifdef USE_OPENAL
	if (sound_started == SS_OAL)
		AL_UnqueueRawSamples();
#endif

	mpg123_close(mp3_handle);
	mp3_status  = MP3_STOP;
	mp3_numbufs = 0;
}

/*
 * Toggle pause / resume.
 */
static void
MP3_TogglePlayback(void)
{
	if (mp3_status == MP3_PLAY)
	{
		mp3_status  = MP3_PAUSE;
		mp3_numbufs = 0;

#ifdef USE_OPENAL
		if (sound_started == SS_OAL)
			AL_UnqueueRawSamples();
#endif
	}
	else if (mp3_status == MP3_PAUSE)
	{
		mp3_status = MP3_PLAY;
	}
}

/* ------------------------------------------------------------------ */
/* Console commands                                                     */
/* ------------------------------------------------------------------ */

/*
 * Print information about the MP3 subsystem state.
 */
static void
MP3_Info(void)
{
	Com_Printf("Tracks:\n");

	for (int i = 2; i <= mp3_maxfileindex; i++)
	{
		if (mp3_tracks[i])
			Com_Printf(" - %02d %s\n", i, mp3_tracks[i]);
		else
			Com_Printf(" - %02d <none>\n", i);
	}

	Com_Printf("Total: %d MP3 files.\n", mp3_maxfileindex + 1);

	switch (mp3_status)
	{
		case MP3_PLAY:
			Com_Printf("State: Playing track %d (%s) at %d samples.\n",
			           mp3_curfile, mp3_tracks[mp3_curfile], mp3_numsamples);
			break;

		case MP3_PAUSE:
			Com_Printf("State: Paused track %d (%s) at %d samples.\n",
			           mp3_curfile, mp3_tracks[mp3_curfile], mp3_numsamples);
			break;

		case MP3_STOP:
			if (mp3_curfile == -1)
				Com_Printf("State: Stopped.\n");
			else
				Com_Printf("State: Stopped track %d (%s).\n",
				           mp3_curfile, mp3_tracks[mp3_curfile]);
			break;
	}
}

/*
 * Print a short help message for the 'mp3' console command.
 */
static void
MP3_HelpMsg(void)
{
	Com_Printf("Unknown sub command %s\n\n", Cmd_Argv(1));
	Com_Printf("Commands:\n");
	Com_Printf(" - info: Print information about playback state and tracks\n");
	Com_Printf(" - play <track>: Play track number <track>\n");
	Com_Printf(" - stop: Stop playback\n");
	Com_Printf(" - toggle: Toggle pause\n");
}

/*
 * The 'mp3' console command.
 */
static void
MP3_Cmd(void)
{
	if (Cmd_Argc() < 2)
	{
		MP3_HelpMsg();
		return;
	}

	if (Q_stricmp(Cmd_Argv(1), "info") == 0)
	{
		MP3_Info();
	}
	else if (Q_stricmp(Cmd_Argv(1), "play") == 0)
	{
		if (Cmd_Argc() != 3)
		{
			Com_Printf("mp3 play <track> : Play <track>\n");
			return;
		}

		int track = (int)strtol(Cmd_Argv(2), NULL, 10);

		if (track < 2 || track > mp3_maxfileindex)
		{
			Com_Printf("invalid track %s, must be a number between 2 and %d\n",
			           Cmd_Argv(2), mp3_maxfileindex);
			return;
		}

		MP3_PlayTrack(track);
	}
	else if (Q_stricmp(Cmd_Argv(1), "stop") == 0)
	{
		MP3_Stop();
	}
	else if (Q_stricmp(Cmd_Argv(1), "toggle") == 0)
	{
		MP3_TogglePlayback();
	}
	else
	{
		MP3_HelpMsg();
	}
}

/* ------------------------------------------------------------------ */
/* State save / restore (for level transitions)                         */
/* ------------------------------------------------------------------ */

void
MP3_SaveState(void)
{
	if (mp3_status != MP3_PLAY)
	{
		mp3_saved_state.saved = false;
		return;
	}

	mp3_saved_state.saved      = true;
	mp3_saved_state.curfile    = mp3_curfile;
	mp3_saved_state.numsamples = mp3_numsamples;
}

void
MP3_RecoverState(void)
{
	if (!mp3_saved_state.saved)
		return;

	/* Disable shuffle temporarily so the exact track is restored. */
	int shuffle_state = (int)mp3_shuffle->value;
	Cvar_SetValue("mp3_shuffle", 0);

	MP3_PlayTrack(mp3_saved_state.curfile);

	/* Seek to the saved position (best-effort; mpg123 uses frame seek). */
	if (mp3_status == MP3_PLAY)
	{
		/* Convert sample position to frame position (approx). */
		off_t frame = (off_t)(mp3_saved_state.numsamples / 1152);
		mpg123_seek_frame(mp3_handle, frame, SEEK_SET);
		mp3_numsamples = mp3_saved_state.numsamples;
	}

	Cvar_SetValue("mp3_shuffle", shuffle_state);
}

/* ------------------------------------------------------------------ */
/* Init / Shutdown                                                      */
/* ------------------------------------------------------------------ */

/*
 * Initialize the MP3 music subsystem.
 */
void
MP3_Init(void)
{
	/* CVars */
	mp3_shuffle      = Cvar_Get("mp3_shuffle",      "0",   CVAR_ARCHIVE);
	mp3_ignoretrack0 = Cvar_Get("mp3_ignoretrack0", "0",   CVAR_ARCHIVE);
	mp3_volume       = Cvar_Get("mp3_volume",       "0.7", CVAR_ARCHIVE);

	cvar_t *mp3_enabled = Cvar_Get("mp3_enable", "1", CVAR_ARCHIVE);

	if (mp3_enabled->value != 1)
		return;

	/* Initialize libmpg123 library. */
	int mErr = mpg123_init();

	if (mErr != MPG123_OK)
	{
		Com_Printf("MP3_Init: mpg123_init() failed: %s\n",
		           mpg123_plain_strerror(mErr));
		return;
	}

	mp3_handle = mpg123_new(NULL, &mErr);

	if (mp3_handle == NULL)
	{
		Com_Printf("MP3_Init: mpg123_new() failed: %s\n",
		           mpg123_plain_strerror(mErr));
		mpg123_exit();
		return;
	}

	/* Console command */
	Cmd_AddCommand("mp3", MP3_Cmd);

	/* Global state */
	mp3_curfile    = -1;
	mp3_numsamples = 0;
	mp3_status     = MP3_STOP;
	mp3_rate       = 44100;
	mp3_channels   = 2;

	mp3_started = true;
}

/*
 * Shut down the MP3 music subsystem and free all resources.
 */
void
MP3_Shutdown(void)
{
	if (!mp3_started)
		return;

	MP3_Stop();

	/* Free track list. */
	for (int i = 0; i < MAX_NUM_MP3TRACKS; ++i)
	{
		if (mp3_tracks[i] != NULL)
		{
			free(mp3_tracks[i]);
			mp3_tracks[i] = NULL;
		}
	}
	mp3_maxfileindex = 0;

	/* Release libmpg123 resources. */
	mpg123_delete(mp3_handle);
	mp3_handle = NULL;
	mpg123_exit();

	/* Remove console command. */
	Cmd_RemoveCommand("mp3");

	mp3_started = false;
}

#endif /* MP3 */
