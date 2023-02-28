/*
 * Copyright (C) 2006, Jamie McCracken <jamiemcc@gnome.org>
 * Copyright (C) 2008-2009, Nokia <ivan.frade@nokia.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301, USA.
 */

#include "config-miners.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>

#include <glib.h>
#include <glib/gstdio.h>

#ifndef G_OS_WIN32
#include <sys/mman.h>
#endif /* G_OS_WIN32 */

#include <libtracker-miners-common/tracker-common.h>

#include <libtracker-extract/tracker-extract.h>

#ifdef FRAME_ENABLE_TRACE
#warning Frame traces enabled
#endif /* FRAME_ENABLE_TRACE */

/* We mmap the beginning of the file and read separately the last 128
 * bytes for id3v1 tags. While these are probably cornercases the
 * rationale is that we don't want to fault a whole page for the last
 * 128 bytes and on the other we don't want to mmap the whole file
 * with unlimited size (might need to create private copy in some
 * special cases, finding continuous space etc). We now take 5 first
 * MB of the file and assume that this is enough. In theory there is
 * no maximum size as someone could embed 50 gigabytes of album art
 * there.
 */

#define MAX_FILE_READ     1024 * 1024 * 5
#define MAX_MP3_SCAN_DEEP 16768

#define MAX_FRAMES_SCAN   512
#define VBR_THRESHOLD     16

#define ID3V1_SIZE        128

typedef struct {
	gchar *title;
	gchar *artist;
	gchar *album;
	gchar *recording_time;
	gchar *comment;
	gchar *genre;
	gchar *encoding;
	gint   track_number;
} id3tag;

typedef struct {
	gchar *album;
	gchar *comment;
	gchar *content_type;
	gchar *copyright;
	gchar *encoded_by;
	guint32 length;
	gchar *artist1;
	gchar *artist2;
	gchar **performers;
	gchar *composer;
	gchar *publisher;
	gchar *recording_time;
	gchar *release_time;
	gchar *text, *toly;
	gchar *title1;
	gchar *title2;
	gchar *title3;
	gint track_number;
	gint track_count;
	gint set_number;
	gint set_count;
	gchar *acoustid_fingerprint;
	gchar *mb_recording_id;
	gchar *mb_track_id;
	gchar *mb_release_id;
	gchar *mb_artist_id;
	gchar *mb_release_group_id;
} id3v2tag;

typedef enum {
	ID3V2_UNKNOWN,
	ID3V2_COM,
	ID3V2_PIC,
	ID3V2_TAL,
	ID3V2_TCO,
	ID3V2_TCR,
	ID3V2_TEN,
	ID3V2_TLE,
	ID3V2_TPB,
	ID3V2_TP1,
	ID3V2_TP2,
	ID3V2_TRK,
	ID3V2_TT1,
	ID3V2_TT2,
	ID3V2_TT3,
	ID3V2_TXT,
	ID3V2_TYE,
} id3v2frame;

typedef enum {
	ID3V24_UNKNOWN,
	ID3V24_APIC,
	ID3V24_COMM,
	ID3V24_IPLS,
	ID3V24_TALB,
	ID3V24_TCOM,
	ID3V24_TCON,
	ID3V24_TCOP,
	ID3V24_TDRC,
	ID3V24_TDRL,
	ID3V24_TENC,
	ID3V24_TEXT,
	ID3V24_TIT1,
	ID3V24_TIT2,
	ID3V24_TIT3,
	ID3V24_TLEN,
	ID3V24_TMCL,
	ID3V24_TOLY,
	ID3V24_TPE1,
	ID3V24_TPE2,
	ID3V24_TPUB,
	ID3V24_TRCK,
	ID3V24_TPOS,
	ID3V24_TXXX,
	ID3V24_TYER,
	ID3V24_UFID,
} id3v24frame;

typedef enum {
	ACOUSTID_FINGERPRINT,
	MB_TRACK_ID,
	MB_RELEASE_ID,
	MB_ARTIST_ID,
	MB_RELEASE_GROUP_ID,
	TXXX_UNKNOWN,
} id3txxxtype;

typedef enum {
	MB_RECORDING_ID,
	UFID_UNKNOWN,
} id3ufidtype;

typedef struct {
	size_t size;
	size_t id3v2_size;

	const gchar *title;
	const gchar *artist_name;
	TrackerResource *artist;
	gchar **performers_names;
	TrackerResource *performer;
	const gchar *album_artist_name;
	const gchar *lyricist_name;
	TrackerResource *lyricist;
	const gchar *album_name;
	TrackerResource *album;
	const gchar *genre;
	const gchar *text;
	const gchar *recording_time;
	const gchar *encoded_by;
	const gchar *copyright;
	const gchar *publisher;
	const gchar *comment;
	const gchar *composer_name;
	TrackerResource *composer;
	gint track_number;
	gint track_count;
	gint set_number;
	gint set_count;
	const gchar *acoustid_fingerprint;
	const gchar *mb_recording_id;
	const gchar *mb_track_id;
	const gchar *mb_release_id;
	const gchar *mb_artist_id;
	const gchar *mb_release_group_id;

	const unsigned char *media_art_data;
	size_t media_art_size;
	const gchar *media_art_mime;

	id3tag id3v1;
	id3v2tag id3v22;
	id3v2tag id3v23;
	id3v2tag id3v24;
} MP3Data;

enum {
	MPEG_ERR,
	MPEG_V1,
	MPEG_V2,
	MPEG_V25
};

enum {
	LAYER_ERR,
	LAYER_1,
	LAYER_2,
	LAYER_3
};

/* sorted array */
static const struct {
	const char *name;
	id3v24frame frame;
} id3v24_frames[] = {
	{ "APIC", ID3V24_APIC },
	{ "COMM", ID3V24_COMM },
	{ "IPLS", ID3V24_IPLS },
	{ "TALB", ID3V24_TALB },
	{ "TCOM", ID3V24_TCOM },
	{ "TCON", ID3V24_TCON },
	{ "TCOP", ID3V24_TCOP },
	{ "TDRC", ID3V24_TDRC },
	{ "TDRL", ID3V24_TDRL },
	{ "TENC", ID3V24_TENC },
	{ "TEXT", ID3V24_TEXT },
	{ "TIT1", ID3V24_TIT1 },
	{ "TIT2", ID3V24_TIT2 },
	{ "TIT3", ID3V24_TIT3 },
	{ "TLEN", ID3V24_TLEN },
	{ "TMCL", ID3V24_TMCL },
	{ "TOLY", ID3V24_TOLY },
	{ "TPE1", ID3V24_TPE1 },
	{ "TPE2", ID3V24_TPE2 },
	{ "TPOS", ID3V24_TPOS },
	{ "TPUB", ID3V24_TPUB },
	{ "TRCK", ID3V24_TRCK },
	{ "TXXX", ID3V24_TXXX },
	{ "TYER", ID3V24_TYER },
	{ "UFID", ID3V24_UFID },
};

/* sorted array */
static const struct {
	const char *name;
	id3v2frame frame;
} id3v2_frames[] = {
	{ "COM", ID3V2_COM },
	{ "PIC", ID3V2_PIC },
	{ "TAL", ID3V2_TAL },
	{ "TCO", ID3V2_TCO },
	{ "TCR", ID3V2_TCR },
	{ "TEN", ID3V2_TEN },
	{ "TLE", ID3V2_TLE },
	{ "TP1", ID3V2_TP1 },
	{ "TP2", ID3V2_TP2 },
	{ "TPB", ID3V2_TPB },
	{ "TRK", ID3V2_TRK },
	{ "TT1", ID3V2_TT1 },
	{ "TT2", ID3V2_TT2 },
	{ "TT3", ID3V2_TT3 },
	{ "TXT", ID3V2_TXT },
	{ "TYE", ID3V2_TYE },
};

static const struct {
	const char *name;
	id3txxxtype txxxtype;
} id3_txxxtypes[] = {
	{ "Acoustid Fingerprint", ACOUSTID_FINGERPRINT },
	{ "MusicBrainz Release Track Id", MB_TRACK_ID },
	{ "MusicBrainz Album Id", MB_RELEASE_ID },
	{ "MusicBrainz Artist Id", MB_ARTIST_ID },
	{ "MusicBrainz Release Group Id", MB_RELEASE_GROUP_ID },
};

static const struct {
	const char *name;
	id3ufidtype ufidtype;
} id3_ufidtypes[] = {
	{ "http://musicbrainz.org", MB_RECORDING_ID},
};

static const char *const genre_names[] = {
	"Blues",
	"Classic Rock",
	"Country",
	"Dance",
	"Disco",
	"Funk",
	"Grunge",
	"Hip-Hop",
	"Jazz",
	"Metal",
	"New Age",
	"Oldies",
	"Other",
	"Pop",
	"R&B",
	"Rap",
	"Reggae",
	"Rock",
	"Techno",
	"Industrial",
	"Alternative",
	"Ska",
	"Death Metal",
	"Pranks",
	"Soundtrack",
	"Euro-Techno",
	"Ambient",
	"Trip-Hop",
	"Vocal",
	"Jazz+Funk",
	"Fusion",
	"Trance",
	"Classical",
	"Instrumental",
	"Acid",
	"House",
	"Game",
	"Sound Clip",
	"Gospel",
	"Noise",
	"Alt. Rock",
	"Bass",
	"Soul",
	"Punk",
	"Space",
	"Meditative",
	"Instrumental Pop",
	"Instrumental Rock",
	"Ethnic",
	"Gothic",
	"Darkwave",
	"Techno-Industrial",
	"Electronic",
	"Pop-Folk",
	"Eurodance",
	"Dream",
	"Southern Rock",
	"Comedy",
	"Cult",
	"Gangsta Rap",
	"Top 40",
	"Christian Rap",
	"Pop/Funk",
	"Jungle",
	"Native American",
	"Cabaret",
	"New Wave",
	"Psychedelic",
	"Rave",
	"Showtunes",
	"Trailer",
	"Lo-Fi",
	"Tribal",
	"Acid Punk",
	"Acid Jazz",
	"Polka",
	"Retro",
	"Musical",
	"Rock & Roll",
	"Hard Rock",
	"Folk",
	"Folk/Rock",
	"National Folk",
	"Swing",
	"Fast-Fusion",
	"Bebob",
	"Latin",
	"Revival",
	"Celtic",
	"Bluegrass",
	"Avantgarde",
	"Gothic Rock",
	"Progressive Rock",
	"Psychedelic Rock",
	"Symphonic Rock",
	"Slow Rock",
	"Big Band",
	"Chorus",
	"Easy Listening",
	"Acoustic",
	"Humour",
	"Speech",
	"Chanson",
	"Opera",
	"Chamber Music",
	"Sonata",
	"Symphony",
	"Booty Bass",
	"Primus",
	"Porn Groove",
	"Satire",
	"Slow Jam",
	"Club",
	"Tango",
	"Samba",
	"Folklore",
	"Ballad",
	"Power Ballad",
	"Rhythmic Soul",
	"Freestyle",
	"Duet",
	"Punk Rock",
	"Drum Solo",
	"A Cappella",
	"Euro-House",
	"Dance Hall",
	"Goa",
	"Drum & Bass",
	"Club-House",
	"Hardcore",
	"Terror",
	"Indie",
	"BritPop",
	"Negerpunk",
	"Polsk Punk",
	"Beat",
	"Christian Gangsta Rap",
	"Heavy Metal",
	"Black Metal",
	"Crossover",
	"Contemporary Christian",
	"Christian Rock",
	"Merengue",
	"Salsa",
	"Thrash Metal",
	"Anime",
	"JPop",
	"Synthpop"
};

static const guint sync_mask = 0xE0FF;
static const guint mpeg_ver_mask = 0x1800;
static const guint mpeg_layer_mask = 0x600;
static const guint bitrate_mask = 0xF00000;
static const guint freq_mask = 0xC0000;
static const guint ch_mask = 0xC0000000;
static const guint pad_mask = 0x20000;

static gint bitrate_table[16][6] = {
	{   0,   0,   0,   0,   0,   0 },
	{  32,  32,  32,  32,   8,   8 },
	{  64,  48,  40,  48,  16,  16 },
	{  96,  56,  48,  56,  24,  24 },
	{ 128,  64,  56,  64,  32,  32 },
	{ 160,  80,  64,  80,  40,  40 },
	{ 192,  96,  80,  96,  48,  48 },
	{ 224, 112,  96, 112,  56,  56 },
	{ 256, 128, 112, 128,  64,  64 },
	{ 288, 160, 128, 144,  80,  80 },
	{ 320, 192, 160, 160,  96,  96 },
	{ 352, 224, 192, 176, 112, 112 },
	{ 384, 256, 224, 192, 128, 128 },
	{ 416, 320, 256, 224, 144, 144 },
	{ 448, 384, 320, 256, 160, 160 },
	{  -1,  -1,  -1,  -1,  -1,  -1 }
};

static gint freq_table[4][3] = {
	{ 44100, 22050, 11025 },
	{ 48000, 24000, 12000 },
	{ 32000, 16000,  8000 },
	{    -1,     -1,   -1 }
};

static gint spf_table[6] = {
	48, 144, 144, 48, 144,  72
};

#ifndef HAVE_STRNLEN

size_t
strnlen (const char *str, size_t max)
{
	const char *end = memchr (str, 0, max);
	return end ? (size_t)(end - str) : max;
}

#endif /* HAVE_STRNLEN */

/* Helpers to get data from BE */
inline static guint32
extract_uint32 (gconstpointer data)
{
	const guint32 *ptr = data;
	return GUINT32_FROM_BE (*ptr);
}

inline static guint16
extract_uint16 (gconstpointer data)
{
	const guint16 *ptr = data;
	return GUINT16_FROM_BE (*ptr);
}

inline static guint32
extract_uint32_7bit (gconstpointer data)
{
	const guchar *ptr = data;
#if (G_BYTE_ORDER == G_LITTLE_ENDIAN)
	return (((ptr[0] & 0x7F) << 21) |
	        ((ptr[1] & 0x7F) << 14) |
	        ((ptr[2] & 0x7F) << 7) |
	        ((ptr[3] & 0x7F) << 0));
#elif (G_BYTE_ORDER == G_BIG_ENDIAN)
	return (((ptr[0] & 0x7F) << 0) |
	        ((ptr[1] & 0x7F) << 7) |
	        ((ptr[2] & 0x7F) << 14) |
	        ((ptr[3] & 0x7F) << 21));
#else
	#error "Can’t figure endianness"
	return 0;
#endif
}

/* id3v20 is odd... */
inline static guint32
extract_uint32_3byte (gconstpointer data)
{
	const guchar *ptr = data;
#if (G_BYTE_ORDER == G_LITTLE_ENDIAN)
	return ((ptr[0] << 16) |
	        (ptr[1] << 8) |
	        (ptr[2] << 0));
#elif (G_BYTE_ORDER == G_BIG_ENDIAN)
	return ((ptr[0] << 0) |
	        (ptr[1] << 8) |
	        (ptr[2] << 16));
#else
	#error "Can’t figure endianness"
	return 0;
#endif
}

static void
id3tag_free (id3tag *tags)
{
	g_free (tags->title);
	g_free (tags->artist);
	g_free (tags->album);
	g_free (tags->recording_time);
	g_free (tags->comment);
	g_free (tags->genre);
	g_free (tags->encoding);
}

static void
id3v2tag_free (id3v2tag *tags)
{
	g_free (tags->album);
	g_free (tags->comment);
	g_free (tags->content_type);
	g_free (tags->copyright);
	g_free (tags->artist1);
	g_free (tags->artist2);
	g_strfreev (tags->performers);
	g_free (tags->composer);
	g_free (tags->publisher);
	g_free (tags->recording_time);
	g_free (tags->release_time);
	g_free (tags->encoded_by);
	g_free (tags->text);
	g_free (tags->toly);
	g_free (tags->title1);
	g_free (tags->title2);
	g_free (tags->title3);
	g_free (tags->acoustid_fingerprint);
	g_free (tags->mb_recording_id);
	g_free (tags->mb_track_id);
	g_free (tags->mb_release_id);
	g_free (tags->mb_artist_id);
	g_free (tags->mb_release_group_id);
}

static gboolean
guess_dlna_profile (gint          bitrate,
                    gint          frequency,
                    gint          mpeg_version,
                    gint          layer_version,
                    gint          n_channels,
                    const gchar **dlna_profile,
                    const gchar **dlna_mimetype)
{
	if (mpeg_version == MPEG_V1 &&
	    layer_version == LAYER_3 &&
	    (bitrate >= 32000 && bitrate <= 320000) &&
	    (n_channels == 1 || n_channels == 2) &&
	    (frequency == freq_table[0][0] ||
	     frequency == freq_table[1][0] ||
	     frequency == freq_table[2][0])) {
		*dlna_profile = "MP3";
		*dlna_mimetype = "audio/mpeg";
		return TRUE;
	}

	if ((bitrate >= 8000 && bitrate <= 320000) &&
	    (mpeg_version == MPEG_V1 || mpeg_version == MPEG_V2) &&
	    (frequency == freq_table[0][0] || frequency == freq_table[0][1] ||
	     frequency == freq_table[1][0] || frequency == freq_table[1][1] ||
	     frequency == freq_table[2][0] || frequency == freq_table[2][1])) {
		*dlna_profile = "MP3X";
		*dlna_mimetype = "audio/mpeg";
		return TRUE;
	}

	return FALSE;
}

static char *
read_id3v1_buffer (int     fd,
                   goffset size)
{
	char *buffer;
	guint bytes_read;
	guint rc;

	if (size < 128) {
		return NULL;
	}

	if (lseek (fd, size - ID3V1_SIZE, SEEK_SET) < 0) {
		return NULL;
	}

	buffer = g_malloc (ID3V1_SIZE);

	if (!buffer) {
		return NULL;
	}

	bytes_read = 0;

	while (bytes_read < ID3V1_SIZE) {
		rc = read (fd,
		           buffer + bytes_read,
		           ID3V1_SIZE - bytes_read);
		if (rc == -1) {
			if (errno != EINTR) {
				g_free (buffer);
				return NULL;
			}
		} else if (rc == 0) {
			break;
		} else {
			bytes_read += rc;
		}
	}

	return buffer;
}

/* Convert from UCS-2 to UTF-8 checking the BOM.*/
static gchar *
ucs2_to_utf8(const gchar *data, guint len)
{
	const gchar *encoding = NULL;
	guint16  c;
	gboolean be;
	gchar *utf8 = NULL;

	memcpy (&c, data, 2);

	switch (c) {
	case 0xfeff:
	case 0xfffe:
		be = (G_BYTE_ORDER == G_BIG_ENDIAN);
		be = (c == 0xfeff) ? be : !be;
		encoding = be ? "UCS-2BE" : "UCS-2LE";
		data += 2;
		len -= 2;
		break;
	default:
		encoding = "UCS-2";
		break;
	}

	utf8 = g_convert (data, len, "UTF-8", encoding, NULL, NULL, NULL);

	return utf8;
}

/* Get the genre codes from regular expressions */
static gboolean
get_genre_number (const char *str, guint *genre)
{
	static GRegex *regex1 = NULL;
	static GRegex *regex2 = NULL;
	GMatchInfo *info = NULL;
	gchar *result = NULL;

	if (!regex1) {
		regex1 = g_regex_new ("\\(([0-9]+)\\)", 0, 0, NULL);
	}

	if (!regex2) {
		regex2 = g_regex_new ("([0-9]+)\\z", 0, 0, NULL);
	}

	if (g_regex_match (regex1, str, 0, &info)) {
		result = g_match_info_fetch (info, 1);
		if (result) {
			*genre = atoi (result);
			g_free (result);
			g_match_info_free (info);
			return TRUE;
		}
	}

	g_match_info_free (info);

	if (g_regex_match (regex2, str, 0, &info)) {
		result = g_match_info_fetch (info, 1);
		if (result) {
			*genre = atoi (result);
			g_free (result);
			g_match_info_free (info);
			return TRUE;
		}
	}

	g_match_info_free (info);

	return FALSE;
}

static const gchar *
get_genre_name (guint number)
{
	if (number >= G_N_ELEMENTS (genre_names)) {
		return NULL;
	}

	return genre_names[number];
}

static void
un_unsync (const unsigned char *source,
           size_t               size,
           unsigned char      **destination,
           size_t              *dest_size)
{
	size_t offset;
	gchar *dest;
	size_t new_size;

	offset       = 0;
	*destination = g_malloc0 (size);
	dest         = *destination;
	new_size     = size;

	while (offset < size) {
		*dest = source[offset];

		if ((source[offset] == 0xFF) &&
		    (source[offset + 1] == 0x00)) {
			offset++;
			new_size--;
		}
		dest++;
		offset++;
	}

	*dest_size = new_size;
}

static gchar *
get_encoding (const gchar *data,
              gsize        size,
              gboolean    *encoding_found)
{
	gdouble confidence = 1;
	gchar *encoding;

	/* Try to guess encoding */
	encoding = (data && size ?
	            tracker_encoding_guess (data, size, &confidence) :
	            NULL);

	if (confidence < 0.5) {
		/* Confidence on the results was too low, bail out and
		 * fallback to the default ISO-8859-1/Windows-1252 encoding.
		 */
		g_free (encoding);
		encoding = NULL;
	}

	/* Notify if a proper detection was done */
	if (encoding_found) {
		*encoding_found = (encoding ? TRUE : FALSE);;
	}

	/* If no proper detection was done, return default */
	if (!encoding) {
		/* Use Windows-1252 instead of ISO-8859-1 as the former is a
		   superset in terms of printable characters and some
		   applications use it to encode characters in ID3 tags */
		encoding = g_strdup ("Windows-1252");
	}

	return encoding;
}

static gchar *
convert_to_encoding (const gchar  *str,
                     gssize        len,
                     const gchar  *to_codeset,
                     const gchar  *from_codeset,
                     gsize        *bytes_read,
                     gsize        *bytes_written,
                     GError      **error_out)
{
	GError *error = NULL;
	gchar *word;

	/* g_print ("%s for %s\n", from_codeset, str); */

	word = g_convert (str,
	                  len,
	                  to_codeset,
	                  from_codeset,
	                  bytes_read,
	                  bytes_written,
	                  &error);

	if (error) {
		gchar *encoding;

		encoding = get_encoding (str, len, NULL);
		g_free (word);

		word = g_convert (str,
		                  len,
		                  to_codeset,
		                  encoding,
		                  bytes_read,
		                  bytes_written,
		                  error_out);

		g_free (encoding);
		g_error_free (error);
	}

	return word;
}

static gboolean
get_id3 (const gchar *data,
         size_t       size,
         id3tag      *id3)
{
	gchar *encoding, *year;
	const gchar *pos;

	if (!data) {
		return FALSE;
	}

	if (size < 128) {
		return FALSE;
	}

	pos = &data[size - 128];

	if (strncmp ("TAG", pos, 3) != 0) {
		return FALSE;
	}

	/* Now convert all the data separately */
	pos += 3;

	/* We don't use our magic convert_to_encoding here because we
	 * have a better way to collect a bit more data before we let
	 * enca loose on it for v1.
	 */
	if (tracker_encoding_can_guess ()) {
		GString *s;
		gboolean encoding_was_found;

		/* Get the encoding for ALL the data we are extracting here */
		/* This wont work with encodings where a NUL byte may be actually valid,
		 * like UTF-16 */
		s = g_string_new_len (pos, strnlen (pos, 30));
		g_string_append_len (s, pos + 30, strnlen (pos+30, 30));
		g_string_append_len (s, pos + 60, strnlen (pos+60, 30));
		g_string_append_len (s, pos + 94, strnlen (pos+94, ((pos+94)[28] != 0) ? 30 : 28));

		encoding = get_encoding (s->str, s->len, &encoding_was_found);

		if (encoding_was_found) {
			id3->encoding = g_strdup (encoding);
		}

		g_string_free (s, TRUE);
	} else {
		/* If we cannot guess encoding, don't even try it, just
		 * use the default one */
		encoding = get_encoding (NULL, 0, NULL);
	}

	id3->title = g_convert (pos, 30, "UTF-8", encoding, NULL, NULL, NULL);

	pos += 30;
	id3->artist = g_convert (pos, 30, "UTF-8", encoding, NULL, NULL, NULL);

	pos += 30;
	id3->album = g_convert (pos, 30, "UTF-8", encoding, NULL, NULL, NULL);

	pos += 30;
	year = g_convert (pos, 4, "UTF-8", encoding, NULL, NULL, NULL);
	if (year && atoi (year) > 0) {
		id3->recording_time = tracker_date_guess (year);
	}
	g_free (year);

	pos += 4;

	if (pos[28] != 0) {
		id3->comment = g_convert (pos, 30, "UTF-8", encoding, NULL, NULL, NULL);
		id3->track_number = 0;
	} else {
		gchar buf[5];

		id3->comment = g_convert (pos, 28, "UTF-8", encoding, NULL, NULL, NULL);

		snprintf (buf, 5, "%d", pos[29]);
		id3->track_number = atoi (buf);
	}

	pos += 30;
	id3->genre = g_strdup (get_genre_name ((guint) pos[0]));

	if (!id3->genre) {
		id3->genre = g_strdup ("");
	}

	g_free (encoding);

	return TRUE;
}

static gboolean
mp3_parse_xing_header (const gchar          *data,
                       size_t                frame_pos,
                       gchar                 mpeg_version,
                       gint                  n_channels,
                       guint32              *nr_frames)
{
	guint32 field_flags;
	size_t pos;
	guint xing_header_offset;

	if (mpeg_version == MPEG_V1) {
		xing_header_offset = (n_channels == 1) ? 21: 36;
	} else {
		xing_header_offset = (n_channels == 1) ? 13: 21;
	}

	pos = frame_pos + xing_header_offset;

	/* header starts with "Xing" or "Info" */
	if ((data[pos] == 0x58 && data[pos+1] == 0x69 && data[pos+2] == 0x6E && data[pos+3] == 0x67) ||
	    (data[pos] == 0x49 && data[pos+1] == 0x6E && data[pos+2] == 0x46 && data[pos+3] == 0x6F)) {
		g_debug ("XING header found");
	} else {
		return FALSE;
	}

	/* Try to extract the number of frames if the frames field flag is set */
	pos += 4;
	field_flags = extract_uint32 (&data[pos]);
	if ((field_flags & 0x0001) > 0) {
		*nr_frames = extract_uint32 (&data[pos+4]);
	}

	return TRUE;
}

/*
 * For the MP3 frame header description, see
 * http://www.mp3-tech.org/programmer/frame_header.html
 */
static gboolean
mp3_parse_header (const gchar          *data,
                  size_t                size,
                  size_t                seek_pos,
                  const gchar          *uri,
                  TrackerResource      *resource,
                  MP3Data              *filedata)
{
	const gchar *dlna_profile, *dlna_mimetype;
	guint header;
	gchar mpeg_ver = 0;
	gchar layer_ver = 0;
	gint spfp8 = 0;
	guint padsize = 0;
	gint idx_num = 0;
	gint bitrate = 0;
	guint avg_bps = 0;
	gint vbr_flag = 0;
	guint length = 0;
	gint sample_rate = 0;
	guint frame_size;
	guint frames = 0;
	size_t pos = 0;
	gint n_channels;
	guint32 xing_nr_frames = 0;

	pos = seek_pos;

	memcpy (&header, &data[pos], sizeof (header));

	switch (header & mpeg_ver_mask) {
	case 0x1000:
		mpeg_ver = MPEG_V2;
		break;
	case 0x1800:
		mpeg_ver = MPEG_V1;
		break;
	case 0:
		mpeg_ver = MPEG_V25;
		break;
	default:
		/* unknown version */
		return FALSE;
	}

	switch (header & mpeg_layer_mask) {
	case 0x400:
		layer_ver = LAYER_2;
		padsize = 1;
		break;
	case 0x200:
		layer_ver = LAYER_3;
		padsize = 1;
		break;
	case 0x600:
		layer_ver = LAYER_1;
		padsize = 4;
		break;
	default:
		/* unknown layer */
		return FALSE;
	}

	if (mpeg_ver < 3) {
		idx_num = (mpeg_ver - 1) * 3 + layer_ver - 1;
	} else {
		idx_num = 2 + layer_ver;
	}

	spfp8 = spf_table[idx_num];

	/* We assume mpeg version, layer and channels are constant in frames */
	do {
		frames++;

		bitrate = 1000 * bitrate_table[(header & bitrate_mask) >> 20][idx_num];

		/* Skip frame headers with bitrate index '0000' (free) or '1111' (bad) */
		if (bitrate <= 0) {
			frames--;
			return FALSE;
		}

		sample_rate = freq_table[(header & freq_mask) >> 18][mpeg_ver - 1];

		/* Skip frame headers with frequency index '11' (reserved) */
		if (sample_rate <= 0) {
			frames--;
			return FALSE;
		}

		frame_size = spfp8 * bitrate / sample_rate + padsize*((header & pad_mask) >> 17);
		avg_bps += bitrate / 1000;

		pos += frame_size;

		if (frames > MAX_FRAMES_SCAN) {
			/* Optimization */
			break;
		}

		if (avg_bps / frames != bitrate / 1000) {
			vbr_flag = 1;
		}

		if (pos + sizeof (header) > size) {
			/* EOF */
			break;
		}

		if ((!vbr_flag) && (frames > VBR_THRESHOLD)) {
			break;
		}

		memcpy(&header, &data[pos], sizeof (header));
	} while ((header & sync_mask) == sync_mask);

	/* At least 2 frames to check the right position */
	if (frames < 2) {
		/* No valid frames */
		return FALSE;
	}

	n_channels = ((header & ch_mask) == ch_mask) ? 1 : 2;

	/* If the file is encoded in variable bit mode (VBR),
	   try to get the number of frames from the xing header
	   to compute the file duration.  */
	if (vbr_flag) {
		mp3_parse_xing_header (data, seek_pos, mpeg_ver, n_channels, &xing_nr_frames);
	}

	tracker_resource_set_string (resource, "nfo:codec", "MPEG");

	tracker_resource_set_int (resource, "nfo:channels", n_channels);

	avg_bps /= frames;

	if (vbr_flag && xing_nr_frames > 0) {
		/* If the file is encoded with variable bitrate mode (VBR)
		   and the number of frame is known */
		length = spfp8 * 8 * xing_nr_frames / sample_rate;
	} else if ((!vbr_flag && frames > VBR_THRESHOLD) || (frames > MAX_FRAMES_SCAN)) {
		/* If not all frames scanned
		 * Note that bitrate is always > 0, checked before */
		length = (filedata->size - filedata->id3v2_size) / (avg_bps ? avg_bps : (bitrate / 1000)) / 125;
	} else {
		/* Note that sample_rate is always > 0, checked before */
		length = spfp8 * 8 * frames / sample_rate;
	}

	tracker_resource_set_int64 (resource, "nfo:duration", length);
	tracker_resource_set_int64 (resource, "nfo:sampleRate", sample_rate);
	tracker_resource_set_int64 (resource, "nfo:averageBitrate", avg_bps*1000);

	if (guess_dlna_profile (bitrate, sample_rate,
	                        mpeg_ver, layer_ver, n_channels,
	                        &dlna_profile, &dlna_mimetype)) {
		tracker_resource_set_string (resource, "nmm:dlnaProfile", dlna_profile);
		tracker_resource_set_string (resource, "nmm:dlnaMime", dlna_mimetype);
	}

	return TRUE;
}

static gboolean
mp3_parse (const gchar          *data,
           size_t                size,
           size_t                offset,
           const gchar          *uri,
           TrackerResource      *resource,
           MP3Data              *filedata)
{
	guint header;
	guint counter = 0;
	guint pos = offset;

	do {
		/* Seek for frame start */
		if (pos + sizeof (header) > size) {
			return FALSE;
		}

		memcpy (&header, &data[pos], sizeof (header));

		if ((header & sync_mask) == sync_mask) {
			/* Found header sync */
			if (mp3_parse_header (data, size, pos, uri, resource, filedata)) {
				return TRUE;
			}
		}

		pos++;
		counter++;
	} while (counter < MAX_MP3_SCAN_DEEP);

	return FALSE;
}

static gssize
id3v2_nul_size (const gchar encoding)
{
	switch (encoding) {
	case 0x01:
	case 0x02:
		/* UTF-16, string terminated by two NUL bytes */
		return 2;
	default:
		return 1;
	}
}

static gssize
id3v2_strlen (const gchar  encoding,
              const gchar *text,
              gssize       len)
{
	const gchar *pos;

	switch (encoding) {
	case 0x01:
	case 0x02:

		/* UTF-16, string terminated by two NUL bytes */
		pos = memmem (text, len, "\0\0\0", 3);

		if (pos == NULL) {
			pos = memmem (text, len, "\0\0", 2);
		} else {
			pos++;
		}

		if (pos != NULL) {
			return pos - text;
		} else {
			return len;
		}
	default:
		return strnlen (text, len);
	}
}

static gchar *
id3v24_text_to_utf8 (const gchar  encoding,
                     const gchar *text,
                     gssize       len,
                     id3tag      *info)
{
	/* This byte describes the encoding
	 * try to convert strings to UTF-8
	 * if it fails, then forget it.
	 * For UTF-16 if size odd assume invalid 00 term.
	 */

	switch (encoding) {
	case 0x00:
		/* Use Windows-1252 instead of ISO-8859-1 as the former is a
		   superset in terms of printable characters and some
		   applications use it to encode characters in ID3 tags */
		return convert_to_encoding (text,
		                            len,
		                            "UTF-8",
		                            info->encoding ? info->encoding : "Windows-1252",
		                            NULL, NULL, NULL);
	case 0x01 :
		return convert_to_encoding (text,
		                            len - len%2,
		                            "UTF-8",
		                            "UTF-16",
		                            NULL, NULL, NULL);
	case 0x02 :
		return convert_to_encoding (text,
		                            len - len%2,
		                            "UTF-8",
		                            "UTF-16BE",
		                            NULL, NULL, NULL);
	case 0x03 :
		return strndup (text, len);

	default:
		/* Bad encoding byte,
		 * try to convert from
		 * Windows-1252
		 */
		return convert_to_encoding (text,
		                            len,
		                            "UTF-8",
		                            info->encoding ? info->encoding : "Windows-1252",
		                            NULL, NULL, NULL);
	}
}

static gchar *
id3v2_text_to_utf8 (const gchar  encoding,
                    const gchar *text,
                    gssize       len,
                    id3tag      *info)
{
	/* This byte describes the encoding
	 * try to convert strings to UTF-8
	 * if it fails, then forget it
	 * For UCS2 if size odd assume invalid 00 term.
	 */

	switch (encoding) {
	case 0x00:
		/* Use Windows-1252 instead of ISO-8859-1 as the former is a
		   superset in terms of printable characters and some
		   applications use it to encode characters in ID3 tags */
		return convert_to_encoding (text,
		                            len,
		                            "UTF-8",
		                            info->encoding ? info->encoding : "Windows-1252",
		                            NULL, NULL, NULL);
	case 0x01 :
		/* return g_convert (text, */
		/*                   len, */
		/*                   "UTF-8", */
		/*                   "UCS-2", */
		/*                   NULL, NULL, NULL); */
		return ucs2_to_utf8 (text, len - len%2);

	default:
		/* Bad encoding byte,
		 * try to convert from
		 * Windows-1252
		 */
		return convert_to_encoding (text,
		                            len,
		                            "UTF-8",
		                            info->encoding ? info->encoding : "Windows-1252",
		                            NULL, NULL, NULL);
	}
}

static id3v24frame
id3v24_get_frame (const gchar *name)
{
	gint l, r, m;

	/* use binary search */

	l = 0;
	r = G_N_ELEMENTS (id3v24_frames) - 1;
	m = 0;

	do {
		m = (l + r) / 2;
		if (strncmp (name, id3v24_frames[m].name, 4) < 0) {
			/* left half */
			r = m - 1;
		} else {
			/* right half */
			l = m + 1;
		}
	} while (l <= r && strncmp (id3v24_frames[m].name, name, 4) != 0);

	if (strncmp (id3v24_frames[m].name, name, 4) == 0) {
		return id3v24_frames[m].frame;
	} else {
		return ID3V24_UNKNOWN;
	}
}

static id3v2frame
id3v2_get_frame (const gchar *name)
{
	gint l, r, m;

	/* use binary search */

	l = 0;
	r = G_N_ELEMENTS (id3v2_frames) - 1;
	m = 0;

	do {
		m = (l + r) / 2;
		if (strncmp (name, id3v2_frames[m].name, 3) < 0) {
			/* left half */
			r = m - 1;
		} else {
			/* right half */
			l = m + 1;
		}
	} while (l <= r && strncmp (id3v2_frames[m].name, name, 3) != 0);

	if (strncmp (id3v2_frames[m].name, name, 3) == 0) {
		return id3v2_frames[m].frame;
	} else {
		return ID3V2_UNKNOWN;
	}
}

static id3txxxtype
id3_get_txxx_type (const gchar *name)
{
	gint i;
	for (i = 0; i < G_N_ELEMENTS (id3_txxxtypes); i++) {
		if (strcmp (id3_txxxtypes[i].name, name) == 0) {
			return id3_txxxtypes[i].txxxtype;
		}
	}
	return TXXX_UNKNOWN;
}

static id3ufidtype
id3_get_ufid_type (const gchar *name)
{
	gint i;
	for (i = 0; i < G_N_ELEMENTS (id3_ufidtypes); i++) {
		if (strcmp (id3_ufidtypes[i].name, name) == 0) {
			return id3_ufidtypes[i].ufidtype;
		}
	}
	return UFID_UNKNOWN;
}

static void
extract_performers_tags (id3v2tag *tag, const gchar *data, guint pos, size_t csize, id3tag *info, gfloat version)
{
	gchar text_encode;
	guint offset = 0;
	GSList *performers;
	gint n_performers = 0;

	text_encode = data[pos];
	pos += 1;
	performers = NULL;

	while (pos + offset < csize) {
		const gchar *text_instrument;
		const gchar *text_performer;
		gint text_instrument_len;
		gint text_performer_len;
		gchar *performer = NULL;

		text_instrument = &data[pos];
		text_instrument_len = id3v2_strlen (text_encode, text_instrument, csize - 1);
		offset = text_instrument_len + id3v2_nul_size (text_encode);
		text_performer = &data[pos + offset];

		if (version == 2.4f) {
			performer = id3v24_text_to_utf8 (text_encode, text_performer, csize - offset, info);
		} else {
			performer = id3v2_text_to_utf8 (text_encode, text_performer, csize - offset, info);
		}

		performers = g_slist_prepend (performers, g_strstrip (g_strdup (performer)));
		n_performers += 1;

		text_performer_len = id3v2_strlen (text_encode, text_performer, csize - offset);
		pos += text_instrument_len + text_performer_len + 2*id3v2_nul_size (text_encode);
	}

	if (performers) {
		GSList *list;

		tag->performers = g_new (gchar *, n_performers + 1);
		tag->performers[n_performers] = NULL;
		for (list = performers; list != NULL; list = list->next) {
			tag->performers[--n_performers] = list->data;
		}

		g_slist_free (performers);
	}
}

static void
extract_txxx_tags (id3v2tag *tag, const gchar *data, guint pos, size_t csize, id3tag *info, gfloat version)
{
	gchar *description = NULL;
	gchar *value = NULL;
	gchar text_encode;
	const gchar *text_desc;
	const gchar *text;
	guint offset;
	gint text_desc_len;
	id3txxxtype txxxtype;

	text_encode   =  data[pos + 0]; /* $xx */
	text_desc     = &data[pos + 4]; /* <text string according to encoding> $00 (00) */
	text_desc_len = id3v2_strlen (text_encode, text_desc, csize - 4);

	offset        = 4 + text_desc_len + id3v2_nul_size (text_encode);
	text          = &data[pos + offset]; /* <full text string according to encoding> */

	if (version == 2.3f) {
		description = id3v2_text_to_utf8 (data[pos], &data[pos + 1], csize - 1, info);
		value = id3v2_text_to_utf8 (text_encode, text, csize - offset, info);
	} else if (version == 2.4f) {
		description = id3v24_text_to_utf8 (data[pos], &data[pos + 1], csize - 1, info);
		value = id3v24_text_to_utf8 (text_encode, text, csize - offset, info);
	}

	if (!tracker_is_empty_string (description)) {
		g_strstrip (description);
		txxxtype = id3_get_txxx_type (description);
	} else {
		/* Can't do anything without mb tag. */
		g_free (description);
		return;
	}

	if (!tracker_is_empty_string (value)) {
		g_strstrip (value);
	} else {
		/* Can't do anything without value. */
		g_free (value);
		return;
	}

	switch (txxxtype) {
	case ACOUSTID_FINGERPRINT:
		tag->acoustid_fingerprint = value;
		break;
	case MB_TRACK_ID:
		tag->mb_track_id = value;
		break;
	case MB_RELEASE_ID:
		tag->mb_release_id = value;
		break;
	case MB_ARTIST_ID:
		tag->mb_artist_id = value;
		break;
	case MB_RELEASE_GROUP_ID:
		tag->mb_release_group_id = value;
		break;
	default:
		g_free (value);
		break;
	}
}

static void
extract_ufid_tags (id3v2tag *tag, const gchar *data, guint pos, size_t csize)
{
	id3ufidtype ufid_type;
	const gchar *owner;
	gint owner_len;
	gchar *identifier;

	owner      = &data[pos + 0];
	owner_len  = strnlen (owner, csize);
	if (tracker_is_empty_string (owner) ||
		(ufid_type = id3_get_ufid_type (owner)) == UFID_UNKNOWN) {
		return;
	}

	identifier = g_strndup (&data[pos + owner_len + 1], csize - owner_len - 1);
	if (tracker_is_empty_string (identifier)) {
		g_free (identifier);
		return;
	}

	tag->mb_recording_id = identifier;
}

static void
get_id3v24_tags (id3v24frame           frame,
                 const gchar          *data,
                 size_t                csize,
                 id3tag               *info,
                 const gchar          *uri,
                 TrackerResource      *resource,
                 MP3Data              *filedata)
{
	id3v2tag *tag = &filedata->id3v24;
	guint pos = 0;

	switch (frame) {
	case ID3V24_APIC: {
		/* embedded image */
		gchar text_type;
		const gchar *mime;
		gchar pic_type;
		const gchar *desc;
		guint offset;
		gint mime_len;

		text_type =  data[pos + 0];
		mime      = &data[pos + 1];
		mime_len  = strnlen (mime, csize - 1);
		pic_type  =  data[pos + 1 + mime_len + 1];
		desc      = &data[pos + 1 + mime_len + 1 + 1];

		if (pic_type == 3 || (pic_type == 0 && filedata->media_art_size == 0)) {
			offset = pos + 1 + mime_len + 2;
			offset += id3v2_strlen (text_type, desc, csize - offset) + id3v2_nul_size (text_type);

			filedata->media_art_data = &data[offset];
			filedata->media_art_size = csize - offset;
			filedata->media_art_mime = mime;
		}
		break;
	}

	case ID3V24_COMM: {
		gchar *word;
		gchar text_encode;
		const gchar *text_desc;
		const gchar *text;
		guint offset;
		gint text_desc_len;

		text_encode   =  data[pos + 0]; /* $xx */
		text_desc     = &data[pos + 4]; /* <text string according to encoding> $00 (00) */
		text_desc_len = id3v2_strlen (text_encode, text_desc, csize - 4);

		offset        = 4 + text_desc_len + id3v2_nul_size (text_encode);
		text          = &data[pos + offset]; /* <full text string according to encoding> */

		if (offset >= csize)
			break;

		word = id3v24_text_to_utf8 (text_encode, text, csize - offset, info);

		if (!tracker_is_empty_string (word)) {
			g_strstrip (word);
			g_free (tag->comment);
			tag->comment = word;
		} else {
			g_free (word);
		}
		break;
	}

	case ID3V24_TMCL: {
		extract_performers_tags (tag, data, pos, csize, info, 2.4f);
		break;
	}

	case ID3V24_TXXX: {
		extract_txxx_tags (tag, data, pos, csize, info, 2.4f);
		break;
	}

	case ID3V24_UFID: {
		extract_ufid_tags (tag, data, pos, csize);
		break;
	}

	default: {
		gchar *word;

		/* text frames */
		word = id3v24_text_to_utf8 (data[pos], &data[pos + 1], csize - 1, info);
		if (!tracker_is_empty_string (word)) {
			g_strstrip (word);
		} else {
			/* Can't do anything without word. */
			g_free (word);
			break;
		}

#ifdef FRAME_ENABLE_TRACE
		g_debug ("ID3v2.4: Frame is %d, word is %s", frame, word);
#endif /* FRAME_ENABLE_TRACE */

		switch (frame) {
		case ID3V24_TALB:
			tag->album = word;
			break;
		case ID3V24_TCON: {
			gint genre;

			if (get_genre_number (word, &genre)) {
				g_free (word);
				word = g_strdup (get_genre_name (genre));
			}
			if (word && strcasecmp (word, "unknown") != 0) {
				tag->content_type = word;
			} else {
				g_free (word);
			}
			break;
		}
		case ID3V24_TCOP:
			tag->copyright = word;
			break;
		case ID3V24_TDRC:
			tag->recording_time = tracker_date_guess (word);
			g_free (word);
			break;
		case ID3V24_TDRL:
			tag->release_time = tracker_date_guess (word);
			g_free (word);
			break;
		case ID3V24_TENC:
			tag->encoded_by = word;
			break;
		case ID3V24_TEXT:
			tag->text = word;
			break;
		case ID3V24_TOLY:
			tag->toly = word;
			break;
		case ID3V24_TCOM:
			tag->composer = word;
			break;
		case ID3V24_TIT1:
			tag->title1 = word;
			break;
		case ID3V24_TIT2:
			tag->title2 = word;
			break;
		case ID3V24_TIT3:
			tag->title3 = word;
			break;
		case ID3V24_TLEN:
			tag->length = atoi (word) / 1000;
			g_free (word);
			break;
		case ID3V24_TPE1:
			tag->artist1 = word;
			break;
		case ID3V24_TPE2:
			tag->artist2 = word;
			break;
		case ID3V24_TPUB:
			tag->publisher = word;
			break;
		case ID3V24_TRCK: {
			gchar **parts;

			parts = g_strsplit (word, "/", 2);
			if (parts[0]) {
				tag->track_number = atoi (parts[0]);
				if (parts[1]) {
					tag->track_count = atoi (parts[1]);
				}
			}
			g_strfreev (parts);
			g_free (word);

			break;
		}
		case ID3V24_TPOS: {
			gchar **parts;

			parts = g_strsplit (word, "/", 2);
			if (parts[0]) {
				tag->set_number = atoi (parts[0]);
				if (parts[1]) {
					tag->set_count = atoi (parts[1]);
				}
			}
			g_strfreev (parts);
			g_free (word);

			break;
		}
		case ID3V24_TYER:
			if (atoi (word) > 0) {
				tag->recording_time = tracker_date_guess (word);
			}
			g_free (word);
			break;
		default:
			g_free (word);
			break;
		}
	}
	}
}

static void
get_id3v23_tags (id3v24frame           frame,
                 const gchar          *data,
                 size_t                csize,
                 id3tag               *info,
                 const gchar          *uri,
                 TrackerResource      *resource,
                 MP3Data              *filedata)
{
	id3v2tag *tag = &filedata->id3v23;
	guint pos = 0;

	switch (frame) {
	case ID3V24_APIC: {
		/* embedded image */
		gchar text_type;
		const gchar *mime;
		gchar pic_type;
		const gchar *desc;
		guint offset;
		gint  mime_len;

		text_type =  data[pos + 0];
		mime      = &data[pos + 1];
		mime_len  = strnlen (mime, csize - 1);
		pic_type  =  data[pos + 1 + mime_len + 1];
		desc      = &data[pos + 1 + mime_len + 1 + 1];

		if (pic_type == 3 || (pic_type == 0 && filedata->media_art_size == 0)) {
			offset = pos + 1 + mime_len + 2;
			offset += id3v2_strlen (text_type, desc, csize - offset) + id3v2_nul_size (text_type);

			filedata->media_art_data = &data[offset];
			filedata->media_art_size = csize - offset;
			filedata->media_art_mime = mime;
		}
		break;
	}

	case ID3V24_COMM: {
		gchar *word;
		gchar text_encode;
		const gchar *text_desc;
		const gchar *text;
		guint offset;
		gint text_desc_len;

		text_encode   =  data[pos + 0]; /* $xx */
		text_desc     = &data[pos + 4]; /* <text string according to encoding> $00 (00) */
		text_desc_len = id3v2_strlen (text_encode, text_desc, csize - 4);

		offset        = 4 + text_desc_len + id3v2_nul_size (text_encode);
		text          = &data[pos + offset]; /* <full text string according to encoding> */

		word = id3v2_text_to_utf8 (text_encode, text, csize - offset, info);

		if (!tracker_is_empty_string (word)) {
			g_strstrip (word);
			g_free (tag->comment);
			tag->comment = word;
		} else {
			g_free (word);
		}

		break;
	}

	case ID3V24_IPLS: {
		extract_performers_tags (tag, data, pos, csize, info, 2.3f);
		break;
	}

	case ID3V24_TXXX: {
		extract_txxx_tags (tag, data, pos, csize, info, 2.3f);
		break;
	}

	case ID3V24_UFID: {
		extract_ufid_tags (tag, data, pos, csize);
		break;
	}

	default: {
		gchar *word;

		/* text frames */
		word = id3v2_text_to_utf8 (data[pos], &data[pos + 1], csize - 1, info);

		if (!tracker_is_empty_string (word)) {
			g_strstrip (word);
		} else {
			/* Can't do anything without word. */
			g_free (word);
			break;
		}


#ifdef FRAME_ENABLE_TRACE
		g_debug ("ID3v2.3: Frame is %d, word is %s", frame, word);
#endif /* FRAME_ENABLE_TRACE */

		switch (frame) {
		case ID3V24_TALB:
			tag->album = word;
			break;
		case ID3V24_TCON: {
			gint genre;

			if (get_genre_number (word, &genre)) {
				g_free (word);
				word = g_strdup (get_genre_name (genre));
			}
			if (word && strcasecmp (word, "unknown") != 0) {
				tag->content_type = word;
			} else {
				g_free (word);
			}
			break;
		}
		case ID3V24_TCOP:
			tag->copyright = word;
			break;
		case ID3V24_TENC:
			tag->encoded_by = word;
			break;
		case ID3V24_TEXT:
			tag->text = word;
			break;
		case ID3V24_TOLY:
			tag->toly = word;
			break;
		case ID3V24_TCOM:
			tag->composer = word;
			break;
		case ID3V24_TIT1:
			tag->title1 = word;
			break;
		case ID3V24_TIT2:
			tag->title2 = word;
			break;
		case ID3V24_TIT3:
			tag->title3 = word;
			break;
		case ID3V24_TLEN:
			tag->length = atoi (word) / 1000;
			g_free (word);
			break;
		case ID3V24_TPE1:
			tag->artist1 = word;
			break;
		case ID3V24_TPE2:
			tag->artist2 = word;
			break;
		case ID3V24_TPUB:
			tag->publisher = word;
			break;
		case ID3V24_TRCK: {
			gchar **parts;

			parts = g_strsplit (word, "/", 2);
			if (parts[0]) {
				tag->track_number = atoi (parts[0]);
				if (parts[1]) {
					tag->track_count = atoi (parts[1]);
				}
			}
			g_strfreev (parts);
			g_free (word);

			break;
		}
		case ID3V24_TPOS: {
			gchar **parts;

			parts = g_strsplit (word, "/", 2);
			if (parts[0]) {
				tag->set_number = atoi (parts[0]);
				if (parts[1]) {
					tag->set_count = atoi (parts[1]);
				}
			}
			g_strfreev (parts);
			g_free (word);

			break;
		}
		case ID3V24_TYER:
			if (atoi (word) > 0) {
				tag->recording_time = tracker_date_guess (word);
			}
			g_free (word);
			break;
		default:
			g_free (word);
			break;
		}
	}
	}
}

static void
get_id3v20_tags (id3v2frame            frame,
                 const gchar          *data,
                 size_t                csize,
                 id3tag               *info,
                 const gchar          *uri,
                 TrackerResource      *resource,
                 MP3Data              *filedata)
{
	id3v2tag *tag = &filedata->id3v22;
	guint pos = 0;

	if (frame == ID3V2_PIC) {
		/* embedded image */
		gchar          text_type;
		gchar          pic_type;
		const gchar   *desc;
		guint          offset;
		const gchar   *mime;

		text_type =  data[pos + 0];
		mime      = &data[pos + 1];
		pic_type  =  data[pos + 1 + 3];
		desc      = &data[pos + 1 + 3 + 1];

		if (pic_type == 3 || (pic_type == 0 && filedata->media_art_size == 0)) {
			offset = pos + 1 + 3 + 1;
			offset += id3v2_strlen (text_type, desc, csize - offset) + id3v2_nul_size (text_type);

			filedata->media_art_data = &data[offset];
			filedata->media_art_size = csize - offset;
			filedata->media_art_mime = mime;
		}
	} else {
		/* text frames */
		gchar *word;

		word = id3v2_text_to_utf8 (data[pos], &data[pos + 1], csize - 1, info);
		if (!tracker_is_empty_string (word)) {
			g_strstrip (word);
		} else {
			/* Can't do anything without word. */
			return;
		}

#ifdef FRAME_ENABLE_TRACE
		g_debug ("ID3v2.2: Frame is %d, word is %s", frame, word);
#endif /* FRAME_ENABLE_TRACE */

		switch (frame) {
		case ID3V2_COM:
			tag->comment = word;
			break;
		case ID3V2_TAL:
			tag->album = word;
			break;
		case ID3V2_TCO: {
			gint genre;

			if (get_genre_number (word, &genre)) {
				g_free (word);
				word = g_strdup (get_genre_name (genre));
			}

			if (word && strcasecmp (word, "unknown") != 0) {
				tag->content_type = word;
			} else {
				g_free (word);
			}

			break;
		}
		case ID3V2_TCR:
			tag->copyright = word;
			break;
		case ID3V2_TEN:
			tag->encoded_by = word;
			break;
		case ID3V2_TLE:
			tag->length = atoi (word) / 1000;
			g_free (word);
			break;
		case ID3V2_TPB:
			tag->publisher = word;
			break;
		case ID3V2_TP1:
			tag->artist1 = word;
			break;
		case ID3V2_TP2:
			tag->artist2 = word;
			break;
		case ID3V2_TRK: {
			gchar **parts;

			parts = g_strsplit (word, "/", 2);
			if (parts[0]) {
				tag->track_number = atoi (parts[0]);
				if (parts[1]) {
					tag->track_count = atoi (parts[1]);
				}
			}
			g_strfreev (parts);
			g_free (word);

			break;
		}
		case ID3V2_TT1:
			tag->title1 = word;
			break;
		case ID3V2_TT2:
			tag->title2 = word;
			break;
		case ID3V2_TT3:
			tag->title3 = word;
			break;
		case ID3V2_TXT:
			tag->text = word;
			break;
		case ID3V2_TYE:
			if (atoi (word) > 0) {
				tag->recording_time = tracker_date_guess (word);
			}
			g_free (word);
			break;
		default:
			g_free (word);
			break;
		}
	}
}

static void
parse_id3v24 (const gchar           *data,
              size_t                 size,
              id3tag                *info,
              const gchar           *uri,
              TrackerResource       *resource,
              MP3Data               *filedata,
              size_t                *offset_delta)
{
	const gint header_size = 10;
	const gint frame_size = 10;
	gint unsync;
	gint ext_header;
	gint experimental;
	guint tsize;
	guint pos;
	guint ext_header_size;

	/* Check header, expecting (in hex), 10 bytes long:
	 *
	 *   $ 49 44 33 yy yy xx zz zz zz zz
	 *
	 * Where yy is less than $FF, xx is the 'flags' byte and zz is
	 * less than $80.
	 *
	 * Here yy is the version, so v24 == 04 00.
	 *
	 * MP3's look like this:
	 *
	 *   [Header][?External Header?][Tags][Content]
	 */
	if ((size < 16) ||
	    (data[0] != 0x49) ||
	    (data[1] != 0x44) ||
	    (data[2] != 0x33) ||
	    (data[3] != 0x04) ||
	    (data[4] != 0x00)) {
		/* It's not an error, we might try another function
		 * if we have the wrong version header here.
		 */
		return;
	}

	/* Get the flags (xx) in the header */
	unsync = (data[5] & 0x80) > 0;
	ext_header = (data[5] & 0x40) > 0;
	experimental = (data[5] & 0x20) > 0;

	/* We don't handle experimental cases */
	if (experimental) {
		g_debug ("[v24] Experimental MP3s are not extracted, doing nothing");
		return;
	}

	/* Get the complete tag size (zz) in the header:
	 * Tag size is size of the complete tag after
	 * unsychronisation, including padding, excluding the header
	 * but not excluding the extended header (total tag size - 10)
	 */
	tsize = extract_uint32_7bit (&data[6]);

	/* Check if we can read even the first frame, The complete
	 * tag size (tsize) does not include the header which is 10
	 * bytes, so we check that there is some content AFTER the
	 * headers. */
	if (tsize > size - header_size) {
		g_debug ("[v24] Expected MP3 tag size and header size to be within file size boundaries");
		return;
	}

	/* Start after the header (10 bytes long) */
	pos = header_size;

	/* Completely optional */
	if (ext_header) {
		/* Extended header is expected to be:
		 *   Extended header size   $xx xx xx xx (4 chars)
		 *   Extended Flags         $xx xx
		 *   Size of padding        $xx xx xx xx
		 */
		ext_header_size = extract_uint32_7bit (&data[10]);

		/* Where the 'Extended header size', currently 6 or 10
		 * bytes, excludes itself. The 'Size of padding' is
		 * simply the total tag size excluding the frames and
		 * the headers, in other words the padding.
		 */
		if (ext_header_size > size - header_size - tsize) {
			g_debug ("[v24] Expected MP3 tag size and extended header size to be within file size boundaries");
			return;
		}

		pos += ext_header_size;
	}

	while (pos < tsize + header_size) {
		const char *frame_name;
		id3v24frame frame;
		size_t csize;
		unsigned short flags;

		g_assert (pos <= size - frame_size);

		/* Frames are 10 bytes each and made up of:
		 *   Frame ID       $xx xx xx xx (4 chars)
		 *   Size           $xx xx xx xx
		 *   Flags          $xx xx
		 */
		if (pos + frame_size > tsize + header_size) {
			g_debug ("[v24] Expected MP3 frame size (%d) to be within tag size (%d) boundaries, position = %d",
			         frame_size,
			         tsize + header_size,
			         pos);
			break;
		}

		frame_name = &data[pos];

		/* We found padding after all frames */
		if (frame_name[0] == '\0')
			break;

		/* We found a IDv2 footer */
		if (frame_name[0] == '3' &&
		    frame_name[1] == 'D' &&
		    frame_name[2] == 'I')
			break;

		frame = id3v24_get_frame (frame_name);

		csize = (size_t) extract_uint32_7bit (&data[pos + 4]);

		if (csize > size - frame_size - pos) {
			g_debug ("[v24] Size of current frame '%s' (%" G_GSIZE_FORMAT ") "
			         "exceeds file boundaries (%" G_GSIZE_FORMAT "), "
			         "not processing any more frames",
			         frame_name, csize, size);
			break;
		}

		flags = extract_uint16 (&data[pos + 8]);

		pos += frame_size;

		if (frame == ID3V24_UNKNOWN) {
			/* Ignore unknown frames */
			g_debug ("[v24] Ignoring unknown frame '%s' (pos:%d, size:%" G_GSIZE_FORMAT ")", frame_name, pos, csize);
			pos += csize;
			continue;
		} else {
			g_debug ("[v24] Processing frame '%s'", frame_name);
		}

		/* If content size is more than size of file, stop. If
		 * If content size is 0 then continue to next frame. */
		if (pos + csize > tsize + header_size) {
			g_debug ("[v24] Position (%d) + content size (%" G_GSIZE_FORMAT ") > tag size (%d), not processing any more frames", pos, csize, tsize + header_size);
			break;
		} else if (csize == 0) {
			g_debug ("[v24] Content size was 0, moving to next frame");
			continue;
		}

		/* Frame flags expected are in format of:
		 *
		 *   %abc00000 %ijk00000
		 *
		 * a - Tag alter preservation
		 * b - File alter preservation
		 * c - Read only
		 * i - Compression
		 * j - Encryption
		 * k - Grouping identity
		 */
		if (((flags & 0x80) > 0) ||
		    ((flags & 0x40) > 0)) {
			g_debug ("[v23] Ignoring frame '%s', frame flags 0x80 or 0x40 found (compression / encryption)", frame_name);
			pos += csize;
			continue;
		}

		if ((flags & 0x20) > 0) {
			/* The "group" identifier, skip a byte */
			pos++;
			csize--;
		}

		if ((flags & 0x02) || unsync) {
			size_t unsync_size;
			gchar *body;

			un_unsync (&data[pos], csize, (unsigned char **) &body, &unsync_size);
			get_id3v24_tags (frame, body, unsync_size, info, uri, resource, filedata);
			g_free (body);
		} else {
			get_id3v24_tags (frame, &data[pos], csize, info, uri, resource, filedata);
		}

		pos += csize;
	}

	*offset_delta = tsize + header_size;
}

static void
parse_id3v23 (const gchar          *data,
              size_t                size,
              id3tag               *info,
              const gchar          *uri,
              TrackerResource      *resource,
              MP3Data              *filedata,
              size_t               *offset_delta)
{
	const gint header_size = 10;
	const gint frame_size = 10;
	gint unsync;
	gint ext_header;
	gint experimental;
	guint tsize;
	guint pos;
	guint ext_header_size;

	/* Check header, expecting (in hex), 10 bytes long:
	 *
	 *   $ 49 44 33 yy yy xx zz zz zz zz
	 *
	 * Where yy is less than $FF, xx is the 'flags' byte and zz is
	 * less than $80.
	 *
	 * Here yy is the version, so v23 == 03 00.
	 *
	 * MP3's look like this:
	 *
	 *   [Header][?External Header?][Tags][Content]
	 */
	if ((size < 16) ||
	    (data[0] != 0x49) ||
	    (data[1] != 0x44) ||
	    (data[2] != 0x33) ||
	    (data[3] != 0x03) ||
	    (data[4] != 0x00)) {
		/* It's not an error, we might try another function
		 * if we have the wrong version header here.
		 */
		return;
	}

	/* Get the flags (xx) in the header */
	unsync = (data[5] & 0x80) > 0;
	ext_header = (data[5] & 0x40) > 0;
	experimental = (data[5] & 0x20) > 0;

	/* We don't handle experimental cases */
	if (experimental) {
		g_debug ("[v23] Experimental MP3s are not extracted, doing nothing");
		return;
	}

	/* Get the complete tag size (zz) in the header:
	 * Tag size is size of the complete tag after
	 * unsychronisation, including padding, excluding the header
	 * but not excluding the extended header (total tag size - 10)
	 */
	tsize = extract_uint32_7bit (&data[6]);

	/* Check if we can read even the first frame, The complete
	 * tag size (tsize) does not include the header which is 10
	 * bytes, so we check that there is some content AFTER the
	 * headers. */
	if (tsize > size - header_size) {
		g_debug ("[v23] Expected MP3 tag size and header size to be within file size boundaries");
		return;
	}

	/* Start after the header (10 bytes long) */
	pos = header_size;

	/* Completely optional */
	if (ext_header) {
		/* Extended header is expected to be:
		 *   Extended header size   $xx xx xx xx (4 chars)
		 *   Extended Flags         $xx xx
		 *   Size of padding        $xx xx xx xx
		 */
		ext_header_size = extract_uint32 (&data[10]);

		/* Where the 'Extended header size', currently 6 or 10
		 * bytes, excludes itself. The 'Size of padding' is
		 * simply the total tag size excluding the frames and
		 * the headers, in other words the padding.
		 */
		if (ext_header_size > size - header_size - tsize) {
			g_debug ("[v23] Expected MP3 tag size and extended header size to be within file size boundaries");
			return;
		}

		pos += ext_header_size;
	}

	while (pos < tsize + header_size) {
		const char *frame_name;
		id3v24frame frame;
		size_t csize;
		unsigned short flags;

		g_assert (pos <= size - frame_size);

		/* Frames are 10 bytes each and made up of:
		 *   Frame ID       $xx xx xx xx (4 chars)
		 *   Size           $xx xx xx xx
		 *   Flags          $xx xx
		 */
		if (pos + frame_size > tsize + header_size) {
			g_debug ("[v23] Expected MP3 frame size (%d) to be within tag size (%d) boundaries, position = %d",
			         frame_size,
			         tsize + header_size,
			         pos);
			break;
		}

		frame_name = &data[pos];

		/* We found padding after all frames */
		if (frame_name[0] == '\0')
			break;

		frame = id3v24_get_frame (frame_name);

		csize = (size_t) extract_uint32 (&data[pos + 4]);

		if (csize > size - frame_size - pos) {
			g_debug ("[v23] Size of current frame '%s' (%" G_GSIZE_FORMAT ") "
			         "exceeds file boundaries (%" G_GSIZE_FORMAT "), "
			         "not processing any more frames",
			         frame_name, csize, size);
			break;
		}

		flags = extract_uint16 (&data[pos + 8]);

		pos += frame_size;

		if (frame == ID3V24_UNKNOWN) {
			/* Ignore unknown frames */
			g_debug ("[v23] Ignoring unknown frame '%s' (pos:%d, size:%" G_GSIZE_FORMAT ")", frame_name, pos, csize);
			pos += csize;
			continue;
		} else {
			g_debug ("[v23] Processing frame '%s'", frame_name);
		}

		/* If content size is more than size of file, stop. If
		 * If content size is 0 then continue to next frame. */
		if (pos + csize > tsize + header_size) {
			g_debug ("[v23] Position (%d) + content size (%" G_GSIZE_FORMAT ") > tag size (%d), not processing any more frames", pos, csize, tsize + header_size);
			break;
		} else if (csize == 0) {
			g_debug ("[v23] Content size was 0, moving to next frame");
			continue;
		}

		/* Frame flags expected are in format of:
		 *
		 *   %abc00000 %ijk00000
		 *
		 * a - Tag alter preservation
		 * b - File alter preservation
		 * c - Read only
		 * i - Compression
		 * j - Encryption
		 * k - Grouping identity
		 */
		if (((flags & 0x80) > 0) ||
		    ((flags & 0x40) > 0)) {
			g_debug ("[v23] Ignoring frame '%s', frame flags 0x80 or 0x40 found (compression / encryption)", frame_name);
			pos += csize;
			continue;
		}

		if ((flags & 0x20) > 0) {
			/* The "group" identifier, skip a byte */
			pos++;
			csize--;
		}

		if ((flags & 0x02) || unsync) {
			size_t unsync_size;
			gchar *body;

			un_unsync (&data[pos], csize, (unsigned char **) &body, &unsync_size);
			get_id3v23_tags (frame, body, unsync_size, info, uri, resource, filedata);
			g_free (body);
		} else {
			get_id3v23_tags (frame, &data[pos], csize, info, uri, resource, filedata);
		}

		pos += csize;
	}

	*offset_delta = tsize + header_size;
}

static void
parse_id3v20 (const gchar          *data,
              size_t                size,
              id3tag               *info,
              const gchar          *uri,
              TrackerResource      *resource,
              MP3Data              *filedata,
              size_t               *offset_delta)
{
	const gint header_size = 10;
	const gint frame_size = 6;
	gint unsync;
	guint tsize;
	guint pos;

	if ((size < header_size + frame_size) ||
	    (data[0] != 0x49) ||
	    (data[1] != 0x44) ||
	    (data[2] != 0x33) ||
	    (data[3] != 0x02) ||
	    (data[4] != 0x00)) {
		/* It's not an error, we might try another function
		 * if we have the wrong version header here.
		 */
		return;
	}

	unsync = (data[5] & 0x80) > 0;
	tsize = extract_uint32_7bit (&data[6]);

	if (tsize > size - header_size)  {
		g_debug ("[v20] Expected MP3 tag size and header size to be within file size boundaries");
		return;
	}

	pos = header_size;

	while (pos < tsize + header_size) {
		const char *frame_name;
		id3v2frame frame;
		size_t csize;

		g_assert (pos <= size - frame_size);

		if (pos + frame_size > tsize + header_size)  {
			g_debug ("[v20] Expected MP3 frame size (%d) to be within tag size (%d) boundaries, position = %d",
			         frame_size,
			         tsize + header_size,
			         pos);
			break;
		}

		frame_name = &data[pos];

		/* We found padding after all frames */
		if (frame_name[0] == '\0')
			break;

		frame = id3v2_get_frame (frame_name);

		csize = (size_t) extract_uint32_3byte (&data[pos + 3]);

		if (csize > size - pos - frame_size) {
			g_debug ("[v20] Size of current frame '%s' (%" G_GSIZE_FORMAT ") "
			         "exceeds file boundaries (%" G_GSIZE_FORMAT "), "
			         "not processing any more frames",
			         frame_name, csize, size);
			break;
		}

		pos += frame_size;

		if (frame == ID3V2_UNKNOWN) {
			/* ignore unknown frames */
			g_debug ("[v20] Ignoring unknown frame '%s' (pos:%d, size:%" G_GSIZE_FORMAT ")", frame_name, pos, csize);
			pos += csize;
			continue;
		}

		/* If content size is more than size of file, stop. If
		 * If content size is 0 then continue to next frame. */
		if (pos + csize > tsize + header_size) {
			g_debug ("[v20] Position (%d) + content size (%" G_GSIZE_FORMAT ") > tag size (%d), not processing any more frames", pos, csize, tsize + header_size);
			break;
		} else if (csize == 0) {
			g_debug ("[v20] Content size was 0, moving to next frame");
		}

		/* Early versions do not have unsynch per frame */
		if (unsync) {
			size_t  unsync_size;
			gchar  *body;

			un_unsync (&data[pos], csize, (unsigned char **) &body, &unsync_size);
			get_id3v20_tags (frame, body, unsync_size, info, uri, resource, filedata);
			g_free (body);
		} else {
			get_id3v20_tags (frame, &data[pos], csize, info, uri, resource, filedata);
		}

		pos += csize;
	}

	*offset_delta = tsize + header_size;
}

static goffset
parse_id3v2 (const gchar          *data,
             size_t                size,
             id3tag               *info,
             const gchar          *uri,
             TrackerResource      *resource,
             MP3Data              *filedata)
{
	gboolean done = FALSE;
	size_t offset = 0;

	do {
		size_t offset_delta = 0;
		parse_id3v24 (data + offset, size - offset, info, uri, resource, filedata, &offset_delta);
		parse_id3v23 (data + offset, size - offset, info, uri, resource, filedata, &offset_delta);
		parse_id3v20 (data + offset, size - offset, info, uri, resource, filedata, &offset_delta);

		if (offset_delta == 0) {
			done = TRUE;
			filedata->id3v2_size = offset;
		} else {
			offset += offset_delta;
		}

	} while (!done);

	return offset;
}

G_MODULE_EXPORT gboolean
tracker_extract_get_metadata (TrackerExtractInfo  *info,
                              GError             **error)
{
	gchar *filename, *uri, *resource_uri;
	int fd;
	void *buffer;
	void *id3v1_buffer;
	goffset size;
	goffset  buffer_size;
	goffset audio_offset;
	MP3Data md = { 0 };
	GFile *file;
	gboolean parsed;
	TrackerResource *main_resource;

	file = tracker_extract_info_get_file (info);
	filename = g_file_get_path (file);

	size = tracker_file_get_size (filename);

	if (size == 0) {
		g_free (filename);
		return FALSE;
	}

	md.size = size;
	buffer_size = MIN (size, MAX_FILE_READ);

	fd = tracker_file_open_fd (filename);

	if (fd == -1) {
		return FALSE;
	}

#ifndef G_OS_WIN32
	/* We don't use GLib's mmap because size can not be specified */
	buffer = mmap (NULL,
	               buffer_size,
	               PROT_READ,
	               MAP_PRIVATE,
	               fd,
	               0);
#endif

	id3v1_buffer = read_id3v1_buffer (fd, size);

#ifdef HAVE_POSIX_FADVISE
	if (posix_fadvise (fd, 0, 0, POSIX_FADV_DONTNEED) != 0)
		g_warning ("posix_fadvise() call failed: %m");
#endif /* HAVE_POSIX_FADVISE */

	close (fd);

	if (buffer == NULL || buffer == (void*) -1) {
		g_free (filename);
		return FALSE;
	}

	if (!get_id3 (id3v1_buffer, ID3V1_SIZE, &md.id3v1)) {
		/* Do nothing? */
	}

	g_free (id3v1_buffer);

	resource_uri = tracker_file_get_content_identifier (file, NULL, NULL);
	main_resource = tracker_resource_new (resource_uri);
	g_free (resource_uri);

	/* Get other embedded tags */
	uri = g_file_get_uri (file);
	audio_offset = parse_id3v2 (buffer, buffer_size, &md.id3v1, uri, main_resource, &md);

	md.title = tracker_coalesce_strip (4, md.id3v24.title2,
	                                   md.id3v23.title2,
	                                   md.id3v22.title2,
	                                   md.id3v1.title);

	md.lyricist_name = tracker_coalesce_strip (4, md.id3v24.text,
	                                           md.id3v23.toly,
	                                           md.id3v23.text,
	                                           md.id3v22.text);

	md.composer_name = tracker_coalesce_strip (3, md.id3v24.composer,
	                                           md.id3v23.composer,
	                                           md.id3v22.composer);

	md.artist_name = tracker_coalesce_strip (4, md.id3v24.artist1,
	                                            md.id3v23.artist1,
	                                            md.id3v22.artist1,
	                                            md.id3v1.artist);

	if (md.id3v24.performers) {
		md.performers_names = g_strdupv (md.id3v24.performers);
	} else if (md.id3v23.performers) {
		md.performers_names = g_strdupv (md.id3v23.performers);
	}

	md.album_artist_name = tracker_coalesce_strip (3, md.id3v24.artist2,
	                                               md.id3v23.artist2,
	                                               md.id3v22.artist2);

	md.album_name = tracker_coalesce_strip (4, md.id3v24.album,
	                                        md.id3v23.album,
	                                        md.id3v22.album,
	                                        md.id3v1.album);

	md.genre = tracker_coalesce_strip (7, md.id3v24.content_type,
	                                   md.id3v24.title1,
	                                   md.id3v23.content_type,
	                                   md.id3v23.title1,
	                                   md.id3v22.content_type,
	                                   md.id3v22.title1,
	                                   md.id3v1.genre);

	md.recording_time = tracker_coalesce_strip (7, md.id3v24.recording_time,
	                                            md.id3v24.release_time,
	                                            md.id3v23.recording_time,
	                                            md.id3v23.release_time,
	                                            md.id3v22.recording_time,
	                                            md.id3v22.release_time,
	                                            md.id3v1.recording_time);

	md.publisher = tracker_coalesce_strip (3, md.id3v24.publisher,
	                                       md.id3v23.publisher,
	                                       md.id3v22.publisher);

	md.copyright = tracker_coalesce_strip (3, md.id3v24.copyright,
	                                       md.id3v23.copyright,
	                                       md.id3v22.copyright);

	md.comment = tracker_coalesce_strip (7, md.id3v24.title3,
	                                     md.id3v24.comment,
	                                     md.id3v23.title3,
	                                     md.id3v23.comment,
	                                     md.id3v22.title3,
	                                     md.id3v22.comment,
	                                     md.id3v1.comment);

	md.encoded_by = tracker_coalesce_strip (3, md.id3v24.encoded_by,
	                                        md.id3v23.encoded_by,
	                                        md.id3v22.encoded_by);

	md.acoustid_fingerprint = tracker_coalesce_strip (2, md.id3v24.acoustid_fingerprint,
	                                                  md.id3v23.acoustid_fingerprint);

	md.mb_recording_id = tracker_coalesce_strip (2, md.id3v24.mb_recording_id,
	                                             md.id3v23.mb_recording_id);

	md.mb_track_id = tracker_coalesce_strip (2, md.id3v24.mb_track_id,
	                                         md.id3v23.mb_track_id);

	md.mb_release_id = tracker_coalesce_strip (2, md.id3v24.mb_release_id,
	                                           md.id3v23.mb_release_id);

	md.mb_artist_id = tracker_coalesce_strip (2, md.id3v24.mb_artist_id,
	                                          md.id3v23.mb_artist_id);

	md.mb_release_group_id = tracker_coalesce_strip (2, md.id3v24.mb_release_group_id,
	                                                 md.id3v23.mb_release_group_id);

	if (md.id3v24.track_number != 0) {
		md.track_number = md.id3v24.track_number;
	} else if (md.id3v23.track_number != 0) {
		md.track_number = md.id3v23.track_number;
	} else if (md.id3v22.track_number != 0) {
		md.track_number = md.id3v22.track_number;
	} else if (md.id3v1.track_number != 0) {
		md.track_number = md.id3v1.track_number;
	}

	if (md.id3v24.track_count != 0) {
		md.track_count = md.id3v24.track_count;
	} else if (md.id3v23.track_count != 0) {
		md.track_count = md.id3v23.track_count;
	} else if (md.id3v22.track_count != 0) {
		md.track_count = md.id3v22.track_count;
	}

	if (md.id3v24.set_number != 0) {
		md.set_number = md.id3v24.set_number;
	} else if (md.id3v23.set_number != 0) {
		md.set_number = md.id3v23.set_number;
	} else if (md.id3v22.set_number != 0) {
		md.set_number = md.id3v22.set_number;
	}

	if (md.id3v24.set_count != 0) {
		md.set_count = md.id3v24.set_count;
	} else if (md.id3v23.set_count != 0) {
		md.set_count = md.id3v23.set_count;
	} else if (md.id3v22.set_count != 0) {
		md.set_count = md.id3v22.set_count;
	}

	if (md.artist_name) {
		md.artist = tracker_extract_new_artist (md.artist_name);
	}

	if (md.performers_names) {
		gint i = 0;
		gchar *performer_name = md.performers_names[i];

		while (performer_name != NULL) {
			md.performer = tracker_extract_new_artist (performer_name);
			tracker_resource_add_relation (main_resource, "nmm:performer", md.performer);
			g_object_unref (md.performer);

			performer_name = md.performers_names[++i];
		}

		g_strfreev (md.performers_names);
	}

	if (md.composer_name) {
		md.composer = tracker_extract_new_artist (md.composer_name);
	}

	if (md.lyricist_name) {
		md.lyricist = tracker_extract_new_artist (md.lyricist_name);
	}

	if (md.album_name) {
		TrackerResource *album_disc = NULL, *album_artist = NULL;
		TrackerResource *mb_release = NULL, *mb_release_group = NULL;

		if (md.album_artist_name)
			album_artist = tracker_extract_new_artist (md.album_artist_name);

		album_disc = tracker_extract_new_music_album_disc (md.album_name,
		                                                   album_artist,
		                                                   md.set_number > 0 ? md.set_number : 1,
		                                                   md.recording_time);

		md.album = tracker_resource_get_first_relation (album_disc, "nmm:albumDiscAlbum");

		tracker_resource_set_take_relation (main_resource, "nmm:musicAlbumDisc", album_disc);

		if (md.mb_release_id) {
			g_autofree char *mb_release_uri = g_strdup_printf("https://musicbrainz.org/release/%s", md.mb_release_id);
			mb_release = tracker_extract_new_external_reference("https://musicbrainz.org/doc/Release",
			                                                    md.mb_release_id,
			                                                    mb_release_uri);

			tracker_resource_set_take_relation (md.album, "tracker:hasExternalReference", mb_release);
		}

		if (md.mb_release_group_id) {
			g_autofree char *mb_release_group_uri = g_strdup_printf("https://musicbrainz.org/release-group/%s", md.mb_release_group_id);
			mb_release_group = tracker_extract_new_external_reference("https://musicbrainz.org/doc/Release_Group",
			                                                          md.mb_release_group_id,
			                                                          mb_release_group_uri);

			tracker_resource_add_take_relation (md.album, "tracker:hasExternalReference", mb_release_group);
		}

		if (md.track_count > 0) {
			tracker_resource_set_int (md.album, "nmm:albumTrackCount", md.track_count);
		}

		g_clear_object (&album_artist);
	}

	tracker_resource_add_uri (main_resource, "rdf:type", "nmm:MusicPiece");
	tracker_resource_add_uri (main_resource, "rdf:type", "nfo:Audio");

	tracker_guarantee_resource_title_from_file (main_resource,
	                                            "nie:title",
	                                            md.title,
	                                            uri,
	                                            NULL);

	if (md.lyricist) {
		tracker_resource_set_relation (main_resource, "nmm:lyricist", md.lyricist);
		g_object_unref (md.lyricist);
	}

	if (md.artist) {
		tracker_resource_set_relation (main_resource, "nmm:artist", md.artist);
		if (md.mb_artist_id) {
			g_autofree char *mb_artist_uri = g_strdup_printf("https://musicbrainz.org/artist/%s", md.mb_artist_id);
			g_autoptr(TrackerResource) mb_artist = tracker_extract_new_external_reference(
			    "https://musicbrainz.org/doc/Artist", md.mb_artist_id, mb_artist_uri);
			tracker_resource_add_relation (md.artist, "tracker:hasExternalReference", mb_artist);
		}
	}

	if (md.composer) {
		tracker_resource_set_relation (main_resource, "nmm:composer", md.composer);
		g_object_unref (md.composer);
	}

	if (md.album) {
		tracker_resource_set_relation (main_resource, "nmm:musicAlbum", md.album);
	}

	if (md.recording_time) {
		tracker_resource_set_string (main_resource, "nie:contentCreated", md.recording_time);
	}

	if (md.genre) {
		tracker_resource_set_string (main_resource, "nfo:genre", md.genre);
	}

	if (md.copyright) {
		tracker_resource_set_string (main_resource, "nie:copyright", md.copyright);
	}

	if (md.comment) {
		tracker_resource_set_string (main_resource, "nie:comment", md.comment);
	}

	if (md.publisher) {
		TrackerResource *publisher = tracker_extract_new_contact (md.publisher);
		tracker_resource_set_relation (main_resource, "nco:publisher", publisher);
		g_object_unref(publisher);
	}

	if (md.encoded_by) {
		tracker_resource_set_string (main_resource,  "nfo:encodedBy", md.encoded_by);
	}

	if (md.track_number > 0) {
		tracker_resource_set_int (main_resource, "nmm:trackNumber", md.track_number);
	}

	if (md.mb_recording_id) {
		g_autofree char *mb_recording_uri = NULL;
		g_autoptr(TrackerResource) mb_recording = NULL;

		mb_recording_uri = g_strdup_printf("https://musicbrainz.org/recording/%s", md.mb_recording_id);
		mb_recording = tracker_extract_new_external_reference("https://musicbrainz.org/doc/Recording",
		                                                      md.mb_recording_id, mb_recording_uri);

		tracker_resource_add_relation (main_resource, "tracker:hasExternalReference", mb_recording);
	}

	if (md.mb_track_id) {
		g_autofree char *mb_track_uri = NULL;
		g_autoptr(TrackerResource) mb_track = NULL;

		mb_track_uri = g_strdup_printf("https://musicbrainz.org/track/%s", md.mb_track_id);
		mb_track = tracker_extract_new_external_reference("https://musicbrainz.org/doc/Track",
		                                                  md.mb_track_id, mb_track_uri);

		tracker_resource_add_relation (main_resource, "tracker:hasExternalReference", mb_track);
	}

	if (md.acoustid_fingerprint) {
		TrackerResource *hash_resource, *file_resource;

		hash_resource = tracker_resource_new (NULL);
		tracker_resource_set_uri (hash_resource, "rdf:type", "nfo:FileHash");

		tracker_resource_set_string (hash_resource, "nfo:hashValue", md.acoustid_fingerprint);
		tracker_resource_set_string (hash_resource, "nfo:hashAlgorithm", "chromaprint");

		file_resource = tracker_resource_new (uri);
		tracker_resource_add_take_relation (main_resource, "nie:isStoredAs", file_resource);

		tracker_resource_set_relation (file_resource, "nfo:hasHash", hash_resource);

		g_object_unref (hash_resource);
	}

	/* Get mp3 stream info */
	parsed = mp3_parse (buffer, buffer_size, audio_offset, uri, main_resource, &md);
	g_clear_object (&md.artist);

	id3v2tag_free (&md.id3v22);
	id3v2tag_free (&md.id3v23);
	id3v2tag_free (&md.id3v24);
	id3tag_free (&md.id3v1);

#ifndef G_OS_WIN32
	munmap (buffer, buffer_size);
#endif

	if (main_resource) {
		tracker_extract_info_set_resource (info, main_resource);
		g_object_unref (main_resource);
	}

	g_free (filename);
	g_free (uri);

	return parsed;
}
