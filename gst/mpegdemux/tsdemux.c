/*
 * tsdemux.c - 
 * Copyright (C) 2009 Zaheer Abbas Merali
 * 
 * Authors:
 *   Zaheer Abbas Merali <zaheerabbas at merali dot org>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdlib.h>

#include "mpegtsbase.h"
#include "tsdemux.h"
#include "gstmpegdesc.h"
#include "gstmpegdefs.h"
#include "mpegtspacketizer.h"

/* latency in mseconds */
#define TS_LATENCY 700

#define TABLE_ID_UNSET 0xFF

GST_DEBUG_CATEGORY_STATIC (ts_demux_debug);
#define GST_CAT_DEFAULT ts_demux_debug

typedef struct _TSDemuxStreamPad TSDemuxStreamPad;
typedef struct _TSDemuxStream TSDemuxStream;

struct _TSDemuxStream
{
  MpegTSBaseStream stream;
  TSDemuxStreamPad *pad;
};

struct _TSDemuxStreamPad
{
  GstPad *pad;

  /* the pid for this pad */
  gint pid;
  TSDemuxStream *stream;

  /* set to FALSE before a push and TRUE after */
  gboolean pushed;

  /* the return of the latest push */
  GstFlowReturn flow_return;
};

static GstElementDetails gst_ts_demux_details =
GST_ELEMENT_DETAILS ("MPEG transport stream demuxer",
    "Codec/Demuxer",
    "Demuxes MPEG2 transport streams",
    "Zaheer Abbas Merali <zaheerabbas at merali dot org>");

#define VIDEO_CAPS \
  GST_STATIC_CAPS (\
    "video/mpeg, " \
      "mpegversion = (int) { 1, 2, 4 }, " \
      "systemstream = (boolean) FALSE; " \
    "video/x-h264;" \
    "video/x-dirac;" \
    "video/x-wmv," \
      "wmvversion = (int) 3, " \
      "format = (fourcc) WVC1" \
  )

#define AUDIO_CAPS \
  GST_STATIC_CAPS ( \
    "audio/mpeg, " \
      "mpegversion = (int) { 1, 4 };" \
    "audio/x-lpcm, " \
      "width = (int) { 16, 20, 24 }, " \
      "rate = (int) { 48000, 96000 }, " \
      "channels = (int) [ 1, 8 ], " \
      "dynamic_range = (int) [ 0, 255 ], " \
      "emphasis = (boolean) { FALSE, TRUE }, " \
      "mute = (boolean) { FALSE, TRUE }; " \
    "audio/x-ac3; audio/x-eac3;" \
    "audio/x-dts;" \
    "audio/x-private-ts-lpcm" \
  )

/* Can also use the subpicture pads for text subtitles? */
#define SUBPICTURE_CAPS \
    GST_STATIC_CAPS ("subpicture/x-pgs; video/x-dvd-subpicture")

static GstStaticPadTemplate video_template =
GST_STATIC_PAD_TEMPLATE ("video_%04x", GST_PAD_SRC,
    GST_PAD_SOMETIMES,
    VIDEO_CAPS);

static GstStaticPadTemplate audio_template =
GST_STATIC_PAD_TEMPLATE ("audio_%04x",
    GST_PAD_SRC,
    GST_PAD_SOMETIMES,
    AUDIO_CAPS);

/*static GstStaticPadTemplate subpicture_template =
GST_STATIC_PAD_TEMPLATE ("subpicture_%04x",
    GST_PAD_SRC,
    GST_PAD_SOMETIMES,
    SUBPICTURE_CAPS);
*/
static GstStaticPadTemplate private_template =
GST_STATIC_PAD_TEMPLATE ("private_%04x",
    GST_PAD_SRC,
    GST_PAD_SOMETIMES,
    GST_STATIC_CAPS_ANY);

enum
{
  ARG_0,
  PROP_PROGRAM_NUMBER,
  /* FILL ME */
};

static void
gst_ts_demux_program_started (MpegTSBase * base, MpegTSBaseProgram * program);
static void
gst_ts_demux_program_stopped (MpegTSBase * base, MpegTSBaseProgram * program);
static GstFlowReturn
gst_ts_demux_push (MpegTSBase * base, MpegTSPacketizerPacket * packet,
    MpegTSPacketizerSection * section);

static void gst_ts_demux_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gst_ts_demux_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);
static void gst_ts_demux_finalize (GObject * object);
/*
static gboolean gst_ts_demux_src_pad_query (GstPad * pad, GstQuery * query);
*/
GST_BOILERPLATE (GstTSDemux, gst_ts_demux, MpegTSBase, GST_TYPE_MPEGTS_BASE);

static void
gst_ts_demux_base_init (gpointer klass)
{
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);

  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&video_template));

  gst_element_class_set_details (element_class, &gst_ts_demux_details);
}

static void
gst_ts_demux_class_init (GstTSDemuxClass * klass)
{
  GObjectClass *gobject_class;
  MpegTSBaseClass *ts_class;

  gobject_class = G_OBJECT_CLASS (klass);
  gobject_class->set_property = gst_ts_demux_set_property;
  gobject_class->get_property = gst_ts_demux_get_property;
  gobject_class->finalize = gst_ts_demux_finalize;

  g_object_class_install_property (gobject_class, PROP_PROGRAM_NUMBER,
      g_param_spec_int ("program-number", "Program number",
          "Program Number to demux for (-1 to ignore)", -1, G_MAXINT,
          -1, G_PARAM_READWRITE));

  ts_class = GST_MPEGTS_BASE_CLASS (klass);
  ts_class->push = GST_DEBUG_FUNCPTR (gst_ts_demux_push);
  ts_class->program_started = GST_DEBUG_FUNCPTR (gst_ts_demux_program_started);
  ts_class->program_stopped = GST_DEBUG_FUNCPTR (gst_ts_demux_program_stopped);
}

static void
gst_ts_demux_init (GstTSDemux * demux, GstTSDemuxClass * klass)
{
  demux->program_number = -1;
  GST_MPEGTS_BASE (demux)->stream_size = sizeof (TSDemuxStream);
}

static void
gst_ts_demux_finalize (GObject * object)
{
  if (G_OBJECT_CLASS (parent_class)->finalize)
    G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
gst_ts_demux_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstTSDemux *demux = GST_TS_DEMUX (object);

  switch (prop_id) {
    case PROP_PROGRAM_NUMBER:
      /* FIXME: do something if program is switched as opposed to set at
       * beginning */
      demux->program_number = g_value_get_int (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
  }
}

static void
gst_ts_demux_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstTSDemux *demux = GST_TS_DEMUX (object);

  switch (prop_id) {
    case PROP_PROGRAM_NUMBER:
      g_value_set_int (value, demux->program_number);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
  }
}

/* returns NULL if no matching descriptor found *
 * otherwise returns a descriptor that needs to *
 * be freed */
static guint8 *
get_descriptor_from_stream (TSDemuxStream * stream, guint8 tag)
{
  GValueArray *descriptors = NULL;
  GstStructure *stream_info = stream->stream.stream_info;
  guint8 *retval = NULL;
  int i;

  gst_structure_get (stream_info, "descriptors", G_TYPE_VALUE_ARRAY,
      &descriptors, NULL);
  if (descriptors) {
    for (i = 0; i < descriptors->n_values; i++) {
      GValue *value = g_value_array_get_nth (descriptors, i);
      guint8 *desc = g_value_dup_boxed (value);
      if (DESC_TAG (desc) == tag) {
        retval = desc;
        break;
      }
    }
    g_value_array_free (descriptors);
  }
  return retval;
}

static void
create_pad_for_stream (gpointer key, gpointer value, gpointer user_data)
{
  guint16 pid;
  TSDemuxStream *stream = (TSDemuxStream *) value;
  gchar *name = NULL;
  GstCaps *caps = NULL;
  GstPadTemplate *template = NULL;
  guint8 *desc = NULL;
  //GstTSDemux *demux = GST_TS_DEMUX (user_data);

  pid = (guint16) GPOINTER_TO_INT (key);
  g_print ("creating pad for stream %d with stream_type %d\n", pid,
      stream->stream.stream_type);
  switch (stream->stream.stream_type) {
    case ST_VIDEO_MPEG1:
    case ST_VIDEO_MPEG2:
      g_print ("mpeg video\n");
      template = gst_static_pad_template_get (&video_template);
      name = g_strdup_printf ("video_%04x", pid);
      caps = gst_caps_new_simple ("video/mpeg",
          "mpegversion", G_TYPE_INT,
          stream->stream.stream_type == ST_VIDEO_MPEG1 ? 1 : 2, "systemstream",
          G_TYPE_BOOLEAN, FALSE, NULL);

      break;
    case ST_AUDIO_MPEG1:
    case ST_AUDIO_MPEG2:
      g_print ("mpeg audio\n");
      template = gst_static_pad_template_get (&audio_template);
      name = g_strdup_printf ("audio_%04x", pid);
      caps =
          gst_caps_new_simple ("audio/mpeg", "mpegversion", G_TYPE_INT, 1,
          NULL);
      break;
    case ST_PRIVATE_DATA:
      g_print ("private data\n");
      desc = get_descriptor_from_stream (stream, DESC_DVB_AC3);
      if (desc) {
        g_print ("ac3 audio\n");
        g_free (desc);
        break;
      }
      desc = get_descriptor_from_stream (stream, DESC_DVB_TELETEXT);
      if (desc) {
        g_print ("teletext\n");
        g_free (desc);
        break;
      }
      desc = get_descriptor_from_stream (stream, DESC_DVB_SUBTITLING);
      if (desc) {
        g_print ("subtitling\n");
        g_free (desc);
      }
      break;
    case ST_HDV_AUX_V:
      template = gst_static_pad_template_get (&private_template);
      name = g_strdup_printf ("private_%04x", pid);
      caps = gst_caps_new_simple ("hdv/aux-v", NULL);
      break;
    case ST_HDV_AUX_A:
      template = gst_static_pad_template_get (&private_template);
      name = g_strdup_printf ("private_%04x", pid);
      caps = gst_caps_new_simple ("hdv/aux-a", NULL);
      break;
    case ST_PRIVATE_SECTIONS:
    case ST_MHEG:
    case ST_DSMCC:
      break;
    case ST_AUDIO_AAC:
      template = gst_static_pad_template_get (&audio_template);
      name = g_strdup_printf ("audio_%04x", pid);
      caps = gst_caps_new_simple ("audio/mpeg",
          "mpegversion", G_TYPE_INT, 4, NULL);
      break;
    case ST_VIDEO_MPEG4:
      template = gst_static_pad_template_get (&video_template);
      name = g_strdup_printf ("video_%04x", pid);
      caps = gst_caps_new_simple ("video/mpeg",
          "mpegversion", G_TYPE_INT, 4,
          "systemstream", G_TYPE_BOOLEAN, FALSE, NULL);
      break;
    case ST_VIDEO_H264:
      template = gst_static_pad_template_get (&video_template);
      name = g_strdup_printf ("video_%04x", pid);
      caps = gst_caps_new_simple ("video/x-h264", NULL);
      break;
    case ST_VIDEO_DIRAC:
      desc = get_descriptor_from_stream (stream, DESC_REGISTRATION);
      if (desc) {
        if (DESC_LENGTH (desc) >= 4) {
          if (DESC_REGISTRATION_format_identifier (desc) == 0x64726163) {
            g_print ("dirac\n");
            /* dirac in hex */
            template = gst_static_pad_template_get (&video_template);
            name = g_strdup_printf ("video_%04x", pid);
            caps = gst_caps_new_simple ("video/x-dirac", NULL);
          }
        }
        g_free (desc);
      }
      break;
    case ST_PRIVATE_EA:        /* Try to detect a VC1 stream */
    {
      desc = get_descriptor_from_stream (stream, DESC_REGISTRATION);
      if (desc) {
        if (DESC_LENGTH (desc) >= 4) {
          if (DESC_REGISTRATION_format_identifier (desc) == DRF_ID_VC1) {
            GST_WARNING ("0xea private stream type found but no descriptor "
                "for VC1. Assuming plain VC1.");
            template = gst_static_pad_template_get (&video_template);
            name = g_strdup_printf ("video_%04x", pid);
            caps = gst_caps_new_simple ("video/x-wmv",
                "wmvversion", G_TYPE_INT, 3,
                "format", GST_TYPE_FOURCC, GST_MAKE_FOURCC ('W', 'V', 'C', '1'),
                NULL);
          }
        }
        g_free (desc);
      }
      break;
    }
/*
    case ST_BD_AUDIO_AC3:
    {
      GstMpegTSStream *PMT_stream =
          gst_mpegts_demux_get_stream_for_PID (stream->demux, stream->PMT_pid);
      GstMPEGDescriptor *program_info = PMT_stream->PMT.program_info;
      guint8 *desc = NULL;

      if (program_info)
        desc = gst_mpeg_descriptor_find (program_info, DESC_REGISTRATION);

      if (desc && DESC_REGISTRATION_format_identifier (desc) == DRF_ID_HDMV) {
        template = klass->audio_template;
        name = g_strdup_printf ("audio_%04x", stream->PID);
        caps = gst_caps_new_simple ("audio/x-eac3", NULL);
      } else if (gst_mpeg_descriptor_find (stream->ES_info,
              DESC_DVB_ENHANCED_AC3)) {
        template = klass->audio_template;
        name = g_strdup_printf ("audio_%04x", stream->PID);
        caps = gst_caps_new_simple ("audio/x-eac3", NULL);
      } else {
        if (!gst_mpeg_descriptor_find (stream->ES_info, DESC_DVB_AC3)) {
          GST_WARNING ("AC3 stream type found but no corresponding "
              "descriptor to differentiate between AC3 and EAC3. "
              "Assuming plain AC3.");
        }
        template = klass->audio_template;
        name = g_strdup_printf ("audio_%04x", stream->PID);
        caps = gst_caps_new_simple ("audio/x-ac3", NULL);
      }
      break;
    case ST_BD_AUDIO_EAC3:
      template = klass->audio_template;
      name = g_strdup_printf ("audio_%04x", stream->PID);
      caps = gst_caps_new_simple ("audio/x-eac3", NULL);
      break;
    case ST_PS_AUDIO_DTS:
      template = klass->audio_template;
      name = g_strdup_printf ("audio_%04x", stream->PID);
      caps = gst_caps_new_simple ("audio/x-dts", NULL);
      break;
    case ST_PS_AUDIO_LPCM:
      template = klass->audio_template;
      name = g_strdup_printf ("audio_%04x", stream->PID);
      caps = gst_caps_new_simple ("audio/x-lpcm", NULL);
      break;
    case ST_BD_AUDIO_LPCM:
      template = klass->audio_template;
      name = g_strdup_printf ("audio_%04x", stream->PID);
      caps = gst_caps_new_simple ("audio/x-private-ts-lpcm", NULL);
      break;
    case ST_PS_DVD_SUBPICTURE:
      template = klass->subpicture_template;
      name = g_strdup_printf ("subpicture_%04x", stream->PID);
      caps = gst_caps_new_simple ("video/x-dvd-subpicture", NULL);
      break;
    case ST_BD_PGS_SUBPICTURE:
      template = klass->subpicture_template;
      name = g_strdup_printf ("subpicture_%04x", stream->PID);
      caps = gst_caps_new_simple ("subpicture/x-pgs", NULL);
      break;*/
  }
  if (template && name && caps) {
    GstPad *pad;
    g_print ("creating pad with name %s and caps %s\n", name,
        gst_caps_to_string (caps));
    pad = gst_pad_new_from_template (template, name);
    gst_pad_use_fixed_caps (pad);
    gst_pad_set_caps (pad, caps);
    gst_caps_unref (caps);
  }
}

static void
gst_ts_demux_program_started (MpegTSBase * base, MpegTSBaseProgram * program)
{
  GstTSDemux *demux = GST_TS_DEMUX (base);

  if (demux->program_number == -1 ||
      demux->program_number == program->program_number) {
    g_print ("program %d started\n", program->program_number);
    demux->program_number = program->program_number;
    demux->program = program;
    g_hash_table_foreach (program->streams, create_pad_for_stream, demux);
  }
}

static void
gst_ts_demux_program_stopped (MpegTSBase * base, MpegTSBaseProgram * program)
{
  GstTSDemux *demux = GST_TS_DEMUX (base);
  g_print ("program %d stopped\n", program->program_number);

  demux->program = NULL;
}

/*
static gboolean
gst_ts_demux_src_pad_query (GstPad * pad, GstQuery * query)
{
  GstTSDemux *demux = GST_TS_DEMUX (gst_pad_get_parent (pad));
  gboolean res;

  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_LATENCY:
    {
      if ((res = gst_pad_peer_query (((MpegTSBase *) demux)->sinkpad, query))) {
        gboolean is_live;
        GstClockTime min_latency, max_latency;

        gst_query_parse_latency (query, &is_live, &min_latency, &max_latency);
        if (is_live) {
          min_latency += TS_LATENCY * GST_MSECOND;
          if (max_latency != GST_CLOCK_TIME_NONE)
            max_latency += TS_LATENCY * GST_MSECOND;
        }

        gst_query_set_latency (query, is_live, min_latency, max_latency);
      }

      break;
    }
    default:
      res = gst_pad_query_default (pad, query);
  }
  gst_object_unref (demux);
  return res;
}
*/

static GstFlowReturn
gst_ts_demux_push (MpegTSBase * base, MpegTSPacketizerPacket * packet,
    MpegTSPacketizerSection * section)
{
  GstTSDemux *demux = GST_TS_DEMUX (base);
  TSDemuxStream *stream = NULL;

  if (G_LIKELY (demux->program)) {
    stream =
        g_hash_table_lookup (demux->program->streams,
        GINT_TO_POINTER (packet->pid));
    if (stream && stream->pad) {
      g_print ("We need to demux this one: pid %d\n", packet->pid);
    }
  }
  return GST_FLOW_OK;
}

gboolean
gst_ts_demux_plugin_init (GstPlugin * plugin)
{
  GST_DEBUG_CATEGORY_INIT (ts_demux_debug, "tsdemux", 0,
      "MPEG transport stream demuxer");

  return gst_element_register (plugin, "tsdemux",
      GST_RANK_NONE, GST_TYPE_TS_DEMUX);
}
