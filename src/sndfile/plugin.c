/*  Audacious - Cross-platform multimedia player
 *  Copyright (C) 2005,2009 Audacious development team.
 *
 *  Based on the xmms_sndfile input plugin:
 *  Copyright (C) 2000, 2002 Erik de Castro Lopo
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

/*
 * Rewritten 17-Feb-2007 (nenolod):
 *   - now uses conditional variables to ensure that sndfile mutex is
 *     entirely protected.
 *   - pausing works now
 *   - fixed some potential race conditions when dealing with NFS.
 *   - TITLE_LEN removed
 *
 * Re-cleaned up 09-Aug-2009 (ccr):
 *   - removed threading madness.
 *   - improved locking.
 *   - misc. cleanups.
 */

#include "config.h"

#include <glib.h>
#include <math.h>
#include <audacious/plugin.h>
#include <audacious/i18n.h>
#include <sndfile.h>

#define BUFFER_SIZE 8192
static GMutex *seek_mutex;
static GCond *seek_cond;
static glong seek_value = -1;


/* Virtual file access wrappers for libsndfile
 */
static sf_count_t
sf_get_filelen (void *user_data)
{
    return aud_vfs_fsize (user_data);
}

static sf_count_t
sf_vseek (sf_count_t offset, int whence, void *user_data)
{
    return aud_vfs_fseek(user_data, offset, whence);
}

static sf_count_t
sf_vread (void *ptr, sf_count_t count, void *user_data)
{
    return aud_vfs_fread(ptr, 1, count, user_data);
}

static sf_count_t
sf_vwrite (const void *ptr, sf_count_t count, void *user_data)
{
    return aud_vfs_fwrite(ptr, 1, count, user_data);
}

static sf_count_t
sf_tell (void *user_data)
{
    return aud_vfs_ftell(user_data);
}

static SF_VIRTUAL_IO sf_virtual_io =
{
    sf_get_filelen,
    sf_vseek,
    sf_vread,
    sf_vwrite,
    sf_tell
};


static SNDFILE *
open_sndfile_from_uri(const gchar *filename, VFSFile **vfsfile, SF_INFO *sfinfo)
{
    SNDFILE *snd_file = NULL;
    *vfsfile = aud_vfs_fopen(filename, "rb");

    if (*vfsfile == NULL)
        return NULL;

    snd_file = sf_open_virtual (&sf_virtual_io, SFM_READ, sfinfo, *vfsfile);
    if (snd_file == NULL)
        aud_vfs_fclose(*vfsfile);

    return snd_file;
}

static void
close_sndfile(SNDFILE *snd_file, VFSFile *vfsfile)
{
    if (snd_file != NULL)
        sf_close(snd_file);
    if (vfsfile != NULL)
        aud_vfs_fclose(vfsfile);
}


/* Plugin initialization
 */
static void
plugin_init (void)
{
    seek_value = -1;
    seek_mutex = g_mutex_new();
    seek_cond = g_cond_new();
}

static void
plugin_cleanup (void)
{
    g_cond_free(seek_cond);
    g_mutex_free(seek_mutex);
}

static void
fill_song_tuple (const gchar *filename, Tuple *ti)
{
    VFSFile *vfsfile = NULL;
    SNDFILE *sndfile;
    SF_INFO sfinfo;
    gboolean lossy = FALSE;
    gchar *codec, *format, *subformat;

    sndfile = open_sndfile_from_uri(filename, &vfsfile, &sfinfo);
    if (sf_get_string(sndfile, SF_STR_TITLE) != NULL)
        aud_tuple_associate_string(ti, FIELD_TITLE, NULL, sf_get_string(sndfile, SF_STR_TITLE));

    aud_tuple_associate_string(ti, FIELD_ARTIST, NULL, sf_get_string(sndfile, SF_STR_ARTIST));
    aud_tuple_associate_string(ti, FIELD_COMMENT, NULL, sf_get_string(sndfile, SF_STR_COMMENT));
    aud_tuple_associate_string(ti, FIELD_DATE, NULL, sf_get_string(sndfile, SF_STR_DATE));
    aud_tuple_associate_string(ti, -1, "software", sf_get_string(sndfile, SF_STR_SOFTWARE));

    if (sndfile != NULL)
        close_sndfile (sndfile, vfsfile);

    if (sfinfo.samplerate > 0)
    {
        aud_tuple_associate_int(ti, FIELD_LENGTH, NULL,
        (gint) ceil (1000.0 * sfinfo.frames / sfinfo.samplerate));
    }

    switch (sfinfo.format & SF_FORMAT_TYPEMASK)
    {
        case SF_FORMAT_WAV:
        case SF_FORMAT_WAVEX:
            format = "Microsoft WAV";
            break;
        case SF_FORMAT_AIFF:
            format = "Apple/SGI AIFF";
            break;
        case SF_FORMAT_AU:
            format = "Sun/NeXT AU";
            break;
        case SF_FORMAT_RAW:
            format = "Raw PCM data";
            break;
        case SF_FORMAT_PAF:
            format = "Ensoniq PARIS";
            break;
        case SF_FORMAT_SVX:
            format = "Amiga IFF / SVX8 / SV16";
            break;
        case SF_FORMAT_NIST:
            format = "Sphere NIST";
            break;
        case SF_FORMAT_VOC:
            format = "Creative VOC";
            break;
        case SF_FORMAT_IRCAM:
            format = "Berkeley/IRCAM/CARL";
            break;
        case SF_FORMAT_W64:
            format = "Sonic Foundry's 64 bit RIFF/WAV";
            break;
        case SF_FORMAT_MAT4:
            format = "Matlab (tm) V4.2 / GNU Octave 2.0";
            break;
        case SF_FORMAT_MAT5:
            format = "Matlab (tm) V5.0 / GNU Octave 2.1";
            break;
        case SF_FORMAT_PVF:
            format = "Portable Voice Format";
            break;
        case SF_FORMAT_XI:
            format = "Fasttracker 2 Extended Instrument";
            break;
        case SF_FORMAT_HTK:
            format = "HMM Tool Kit";
            break;
        case SF_FORMAT_SDS:
            format = "Midi Sample Dump Standard";
            break;
        case SF_FORMAT_AVR:
            format = "Audio Visual Research";
            break;
        case SF_FORMAT_SD2:
            format = "Sound Designer 2";
            break;
        case SF_FORMAT_FLAC:
            format = "Free Lossless Audio Codec";
            break;
        case SF_FORMAT_CAF:
            format = "Core Audio File";
            break;
        default:
            format = "Unknown sndfile";
    }

    switch (sfinfo.format & SF_FORMAT_SUBMASK)
    {
        case SF_FORMAT_PCM_S8:
            subformat = "signed 8 bit";
            break;
        case SF_FORMAT_PCM_16:
            subformat = "signed 16 bit";
            break;
        case SF_FORMAT_PCM_24:
            subformat = "signed 24 bit";
            break;
        case SF_FORMAT_PCM_32:
            subformat = "signed 32 bit";
            break;
        case SF_FORMAT_PCM_U8:
            subformat = "unsigned 8 bit";
            break;
        case SF_FORMAT_FLOAT:
            subformat = "32 bit float";
            break;
        case SF_FORMAT_DOUBLE:
            subformat = "64 bit float";
            break;
        case SF_FORMAT_ULAW:
            subformat = "U-Law";
            lossy = TRUE;
            break;
        case SF_FORMAT_ALAW:
            subformat = "A-Law";
            lossy = TRUE;
            break;
        case SF_FORMAT_IMA_ADPCM:
            subformat = "IMA ADPCM";
            lossy = TRUE;
            break;
        case SF_FORMAT_MS_ADPCM:
            subformat = "MS ADPCM";
            lossy = TRUE;
            break;
        case SF_FORMAT_GSM610:
            subformat = "GSM 6.10";
            lossy = TRUE;
            break;
        case SF_FORMAT_VOX_ADPCM:
            subformat = "Oki Dialogic ADPCM";
            lossy = TRUE;
            break;
        case SF_FORMAT_G721_32:
            subformat = "32kbs G721 ADPCM";
            lossy = TRUE;
            break;
        case SF_FORMAT_G723_24:
            subformat = "24kbs G723 ADPCM";
            lossy = TRUE;
            break;
        case SF_FORMAT_G723_40:
            subformat = "40kbs G723 ADPCM";
            lossy = TRUE;
            break;
        case SF_FORMAT_DWVW_12:
            subformat = "12 bit Delta Width Variable Word";
            lossy = TRUE;
            break;
        case SF_FORMAT_DWVW_16:
            subformat = "16 bit Delta Width Variable Word";
            lossy = TRUE;
            break;
        case SF_FORMAT_DWVW_24:
            subformat = "24 bit Delta Width Variable Word";
            lossy = TRUE;
            break;
        case SF_FORMAT_DWVW_N:
            subformat = "N bit Delta Width Variable Word";
            lossy = TRUE;
            break;
        case SF_FORMAT_DPCM_8:
            subformat = "8 bit differential PCM";
            break;
        case SF_FORMAT_DPCM_16:
            subformat = "16 bit differential PCM";
            break;
        default:
            subformat = NULL;
    }

    if (subformat != NULL)
        codec = g_strdup_printf("%s (%s)", format, subformat);
    else
        codec = g_strdup_printf("%s", format);

    aud_tuple_associate_string(ti, FIELD_CODEC, NULL, codec);
    g_free(codec);

    aud_tuple_associate_string(ti, FIELD_QUALITY, NULL, lossy ? "lossy" : "lossless");
}

static gint
is_our_file (const gchar *filename)
{
    VFSFile *vfsfile = NULL;
    SNDFILE *tmp_sndfile;
    SF_INFO tmp_sfinfo;

    /* Have to open the file to see if libsndfile can handle it. */
    tmp_sndfile = open_sndfile_from_uri(filename, &vfsfile, &tmp_sfinfo);

    if (!tmp_sndfile)
        return FALSE;

    /* It can so close file and return TRUE. */
    close_sndfile (tmp_sndfile, vfsfile);
    tmp_sndfile = NULL;

    return TRUE;
}

static void
play_start (InputPlayback *playback)
{
    SNDFILE *sndfile = NULL;
    SF_INFO sfinfo;
    VFSFile *vfsfile = NULL;
    gshort buffer[BUFFER_SIZE], *buffer_p;
    gint samples;

    sndfile = open_sndfile_from_uri(playback->filename, &vfsfile, &sfinfo);
    if (sndfile == NULL)
        return;

    if (!playback->output->open_audio(FMT_S16_NE, sfinfo.samplerate, sfinfo.channels))
    {
        close_sndfile (sndfile, vfsfile);
        playback->error = TRUE;
        return;
    }

    playback->set_params (playback, NULL, 0, 32 * sfinfo.samplerate,
     sfinfo.samplerate, sfinfo.channels);

    playback->playing = TRUE;
    playback->set_pb_ready(playback);

    while (playback->playing)
    {
        /* sf_read_short will return 0 for all reads at EOF. */
        samples = sf_read_short (sndfile, buffer, BUFFER_SIZE);

        if (samples > 0 && playback->playing) {
            buffer_p = &buffer[0];

            /* Output audio in small blocks */
            while (samples > 0 && playback->playing)
            {
                gint writeoff = samples >= 512 ? 512 : samples;

                playback->pass_audio(playback, FMT_S16_NE, sfinfo.channels,
                    writeoff * sizeof(buffer[0]), buffer_p, NULL);

                buffer_p += writeoff;
                samples -= writeoff;

                /* Check for seek request and bail out if we have one */
                g_mutex_lock(seek_mutex);
                if (seek_value != -1) {
                    g_mutex_unlock(seek_mutex);
                    break;
                }
                g_mutex_unlock(seek_mutex);
            }
        }
        else
        {
            playback->eof = TRUE;
            playback->playing = FALSE;
        }

        /* Perform seek, if requested */
        g_mutex_lock(seek_mutex);
        if (seek_value >= 0)
        {
            playback->output->flush(seek_value);
            sf_seek(sndfile, (sf_count_t) seek_value * sfinfo.samplerate / 1000, SEEK_SET);
            seek_value = -1;
            g_cond_signal(seek_cond);
        }
        g_mutex_unlock(seek_mutex);
    }

    playback->playing = FALSE;
    sf_close(sndfile);
    playback->output->close_audio();
}

static void
play_pause (InputPlayback *playback, gshort p)
{
    playback->output->pause(p);
}

static void
play_stop (InputPlayback *playback)
{
    playback->playing = FALSE;
}

static void
file_mseek (InputPlayback *playback, gulong ms)
{
    g_mutex_lock(seek_mutex);
    seek_value = ms;
    g_cond_wait(seek_cond, seek_mutex);
    g_mutex_unlock(seek_mutex);
}

static void
file_seek (InputPlayback *playback, gint time)
{
    gulong millisecond = time * 1000;
    file_mseek(playback, millisecond);
}

static Tuple*
get_song_tuple (const gchar *filename)
{
    Tuple *ti = aud_tuple_new_from_filename(filename);
    fill_song_tuple(filename, ti);
    return ti;
}

static gint
is_our_file_from_vfs(const gchar *filename, VFSFile *fin)
{
    SNDFILE *tmp_sndfile;
    SF_INFO tmp_sfinfo;

    /* Have to open the file to see if libsndfile can handle it. */
    tmp_sndfile = sf_open_virtual (&sf_virtual_io, SFM_READ, &tmp_sfinfo, fin);

    if (!tmp_sndfile)
        return FALSE;

    /* It can so close file and return TRUE. */
    sf_close (tmp_sndfile);
    tmp_sndfile = NULL;

    return TRUE;
}


static void
plugin_about(void)
{
    static GtkWidget *aboutbox = NULL;
    if (aboutbox == NULL)
    {
        aboutbox = audacious_info_dialog(
        _("About sndfile plugin"),
        _("Adapted for Audacious usage by Tony Vroon <chainsaw@gentoo.org>\n"
        "from the xmms_sndfile plugin which is:\n"
        "Copyright (C) 2000, 2002 Erik de Castro Lopo\n\n"
        "This program is free software ; you can redistribute it and/or modify \n"
        "it under the terms of the GNU General Public License as published by \n"
        "the Free Software Foundation ; either version 2 of the License, or \n"
        "(at your option) any later version. \n \n"
        "This program is distributed in the hope that it will be useful, \n"
        "but WITHOUT ANY WARRANTY ; without even the implied warranty of \n"
        "MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  \n"
        "See the GNU General Public License for more details. \n\n"
        "You should have received a copy of the GNU General Public \n"
        "License along with this program ; if not, write to \n"
        "the Free Software Foundation, Inc., \n"
        "51 Franklin Street, Fifth Floor, \n"
        "Boston, MA  02110-1301  USA"),
        _("Ok"), FALSE, NULL, NULL);
        g_signal_connect(G_OBJECT(aboutbox), "destroy",
            (GCallback)gtk_widget_destroyed, &aboutbox);
    }
}


static gchar *sndfile_fmts[] = { "aiff", "au", "raw", "wav", NULL };

static InputPlugin sndfile_ip = {
    .description = "sndfile plugin",
    .init = plugin_init,
    .about = plugin_about,
    .is_our_file = is_our_file,
    .play_file = play_start,
    .stop = play_stop,
    .pause = play_pause,
    .seek = file_seek,
    .cleanup = plugin_cleanup,
    .get_song_tuple = get_song_tuple,
    .is_our_file_from_vfs = is_our_file_from_vfs,
    .vfs_extensions = sndfile_fmts,
    .mseek = file_mseek,
};

static InputPlugin *sndfile_iplist[] = { &sndfile_ip, NULL };

SIMPLE_INPUT_PLUGIN(sndfile, sndfile_iplist)
