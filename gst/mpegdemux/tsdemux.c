/*
 * tsdemux.c - 
 * Copyright (C) 2009 Zaheer Abbas Merali
 *               2010 Edward Hervey
 * 
 * Authors:
 *   Zaheer Abbas Merali <zaheerabbas at merali dot org>
 *   Edward Hervey <bilboed at gmail dot com>
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

  GstPad *pad;

  /* set to FALSE before a push and TRUE after */
  gboolean pushed;

  /* the return of the latest push */
  GstFlowReturn flow_return;

  /* Current data to be pushed out */
  GstBufferList *current;
  GstBufferListIterator *currentit;
  /* First buffer in the list, used to set/modify metadata */
  GstBuffer *firstbuf;
  /* Whether firstbuf has a timestamp set on it */
  gboolean havets;
};

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

static GstStaticPadTemplate subpicture_template =
GST_STATIC_PAD_TEMPLATE ("subpicture_%04x",
    GST_PAD_SRC,
    GST_PAD_SOMETIMES,
    SUBPICTURE_CAPS);

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
static void
gst_ts_demux_stream_added (MpegTSBase * base, MpegTSBaseStream * stream);
static void
gst_ts_demux_stream_removed (MpegTSBase * base, MpegTSBaseStream * stream);

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
  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&audio_template));
  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&subpicture_template));
  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&private_template));

  gst_element_class_set_details_simple (element_class,
      "MPEG transport stream demuxer",
      "Codec/Demuxer",
      "Demuxes MPEG2 transport streams",
      "Zaheer Abbas Merali <zaheerabbas at merali dot org>; Edward Hervey <bilboed at bilboed dot com>");
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
  ts_class->stream_added = gst_ts_demux_stream_added;
  ts_class->stream_removed = gst_ts_demux_stream_removed;
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

static GstPad *
create_pad_for_stream (GstTSDemux * demux, MpegTSBaseStream * bstream)
{
  TSDemuxStream *stream = (TSDemuxStream *) bstream;
  gchar *name = NULL;
  GstCaps *caps = NULL;
  GstPadTemplate *template = NULL;
  guint8 *desc = NULL;
  GstPad *pad = NULL;

  GST_LOG ("Attempting to create pad for stream %d with stream_type %d",
      bstream->pid, bstream->stream_type);

  switch (bstream->stream_type) {
    case ST_VIDEO_MPEG1:
    case ST_VIDEO_MPEG2:
      GST_LOG ("mpeg video");
      template = gst_static_pad_template_get (&video_template);
      name = g_strdup_printf ("video_%04x", bstream->pid);
      caps = gst_caps_new_simple ("video/mpeg",
          "mpegversion", G_TYPE_INT,
          bstream->stream_type == ST_VIDEO_MPEG1 ? 1 : 2, "systemstream",
          G_TYPE_BOOLEAN, FALSE, NULL);

      break;
    case ST_AUDIO_MPEG1:
    case ST_AUDIO_MPEG2:
      GST_LOG ("mpeg audio");
      template = gst_static_pad_template_get (&audio_template);
      name = g_strdup_printf ("audio_%04x", bstream->pid);
      caps =
          gst_caps_new_simple ("audio/mpeg", "mpegversion", G_TYPE_INT, 1,
          NULL);
      break;
    case ST_PRIVATE_DATA:
      GST_LOG ("private data");
      desc =
          mpegts_get_descriptor_from_stream ((MpegTSBaseStream *) stream,
          DESC_DVB_AC3);
      if (desc) {
        GST_LOG ("ac3 audio");
        template = gst_static_pad_template_get (&audio_template);
        name = g_strdup_printf ("audio_%04x", bstream->pid);
        caps = gst_caps_new_simple ("audio/x-ac3", NULL);
        g_free (desc);
        break;
      }
      desc =
          mpegts_get_descriptor_from_stream ((MpegTSBaseStream *) stream,
          DESC_DVB_TELETEXT);
      if (desc) {
        GST_LOG ("teletext");
        template = gst_static_pad_template_get (&private_template);
        name = g_strdup_printf ("private_%04x", bstream->pid);
        caps = gst_caps_new_simple ("private/teletext", NULL);
        g_free (desc);
        break;
      }
      desc =
          mpegts_get_descriptor_from_stream ((MpegTSBaseStream *) stream,
          DESC_DVB_SUBTITLING);
      if (desc) {
        GST_LOG ("subtitling");
        template = gst_static_pad_template_get (&private_template);
        name = g_strdup_printf ("private_%04x", bstream->pid);
        caps = gst_caps_new_simple ("private/x-dvbsub", NULL);
        g_free (desc);
      }
      /* hack for itv hd (sid 10510, video pid 3401 */
      if (demux->program_number == 10510 && bstream->pid == 3401) {
        template = gst_static_pad_template_get (&video_template);
        name = g_strdup_printf ("video_%04x", bstream->pid);
        caps = gst_caps_new_simple ("video/x-h264", NULL);
      }
      break;
    case ST_HDV_AUX_V:
      /* We don't expose those streams since they're only helper streams */
      /* template = gst_static_pad_template_get (&private_template); */
      /* name = g_strdup_printf ("private_%04x", bstream->pid); */
      /* caps = gst_caps_new_simple ("hdv/aux-v", NULL); */
      break;
    case ST_HDV_AUX_A:
      /* We don't expose those streams since they're only helper streams */
      /* template = gst_static_pad_template_get (&private_template); */
      /* name = g_strdup_printf ("private_%04x", bstream->pid); */
      /* caps = gst_caps_new_simple ("hdv/aux-a", NULL); */
      break;
    case ST_PRIVATE_SECTIONS:
    case ST_MHEG:
    case ST_DSMCC:
      break;
    case ST_AUDIO_AAC:
      template = gst_static_pad_template_get (&audio_template);
      name = g_strdup_printf ("audio_%04x", bstream->pid);
      caps = gst_caps_new_simple ("audio/mpeg",
          "mpegversion", G_TYPE_INT, 4, NULL);
      break;
    case ST_VIDEO_MPEG4:
      template = gst_static_pad_template_get (&video_template);
      name = g_strdup_printf ("video_%04x", bstream->pid);
      caps = gst_caps_new_simple ("video/mpeg",
          "mpegversion", G_TYPE_INT, 4,
          "systemstream", G_TYPE_BOOLEAN, FALSE, NULL);
      break;
    case ST_VIDEO_H264:
      template = gst_static_pad_template_get (&video_template);
      name = g_strdup_printf ("video_%04x", bstream->pid);
      caps = gst_caps_new_simple ("video/x-h264", NULL);
      break;
    case ST_VIDEO_DIRAC:
      desc =
          mpegts_get_descriptor_from_stream ((MpegTSBaseStream *) stream,
          DESC_REGISTRATION);
      if (desc) {
        if (DESC_LENGTH (desc) >= 4) {
          if (DESC_REGISTRATION_format_identifier (desc) == 0x64726163) {
            GST_LOG ("dirac");
            /* dirac in hex */
            template = gst_static_pad_template_get (&video_template);
            name = g_strdup_printf ("video_%04x", bstream->pid);
            caps = gst_caps_new_simple ("video/x-dirac", NULL);
          }
        }
        g_free (desc);
      }
      break;
    case ST_PRIVATE_EA:        /* Try to detect a VC1 stream */
    {
      desc =
          mpegts_get_descriptor_from_stream ((MpegTSBaseStream *) stream,
          DESC_REGISTRATION);
      if (desc) {
        if (DESC_LENGTH (desc) >= 4) {
          if (DESC_REGISTRATION_format_identifier (desc) == DRF_ID_VC1) {
            GST_WARNING ("0xea private stream type found but no descriptor "
                "for VC1. Assuming plain VC1.");
            template = gst_static_pad_template_get (&video_template);
            name = g_strdup_printf ("video_%04x", bstream->pid);
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
    case ST_BD_AUDIO_AC3:
      desc =
          mpegts_get_descriptor_from_program (demux->program,
          DESC_REGISTRATION);

      if (desc) {
        if (DESC_REGISTRATION_format_identifier (desc) == DRF_ID_HDMV) {
          template = gst_static_pad_template_get (&audio_template);
          name = g_strdup_printf ("audio_%04x", bstream->pid);
          caps = gst_caps_new_simple ("audio/x-eac3", NULL);
        }
        g_free (desc);
      }
      if (template)
        break;
      else {
        desc =
            mpegts_get_descriptor_from_stream ((MpegTSBaseStream *) stream,
            DESC_DVB_ENHANCED_AC3);
        if (desc) {
          template = gst_static_pad_template_get (&audio_template);
          name = g_strdup_printf ("audio_%04x", bstream->pid);
          caps = gst_caps_new_simple ("audio/x-eac3", NULL);
          g_free (desc);
          break;
        } else {
          desc =
              mpegts_get_descriptor_from_stream ((MpegTSBaseStream *) stream,
              DESC_DVB_AC3);
          if (!desc)
            GST_WARNING ("AC3 stream type found but no corresponding "
                "descriptor to differentiate between AC3 and EAC3. "
                "Assuming plain AC3.");
          if (desc)
            g_free (desc);
          template = gst_static_pad_template_get (&audio_template);
          name = g_strdup_printf ("audio_%04x", bstream->pid);
          caps = gst_caps_new_simple ("audio/x-ac3", NULL);

        }
      }
      break;
    case ST_BD_AUDIO_EAC3:
      template = gst_static_pad_template_get (&audio_template);
      name = g_strdup_printf ("audio_%04x", bstream->pid);
      caps = gst_caps_new_simple ("audio/x-eac3", NULL);
      break;
    case ST_PS_AUDIO_DTS:
      template = gst_static_pad_template_get (&audio_template);
      name = g_strdup_printf ("audio_%04x", bstream->pid);
      caps = gst_caps_new_simple ("audio/x-dts", NULL);
      break;
    case ST_PS_AUDIO_LPCM:
      template = gst_static_pad_template_get (&audio_template);
      name = g_strdup_printf ("audio_%04x", bstream->pid);
      caps = gst_caps_new_simple ("audio/x-lpcm", NULL);
      break;
    case ST_BD_AUDIO_LPCM:
      template = gst_static_pad_template_get (&audio_template);
      name = g_strdup_printf ("audio_%04x", bstream->pid);
      caps = gst_caps_new_simple ("audio/x-private-ts-lpcm", NULL);
      break;
    case ST_PS_DVD_SUBPICTURE:
      template = gst_static_pad_template_get (&subpicture_template);
      name = g_strdup_printf ("subpicture_%04x", bstream->pid);
      caps = gst_caps_new_simple ("video/x-dvd-subpicture", NULL);
      break;
    case ST_BD_PGS_SUBPICTURE:
      template = gst_static_pad_template_get (&subpicture_template);
      name = g_strdup_printf ("subpicture_%04x", bstream->pid);
      caps = gst_caps_new_simple ("subpicture/x-pgs", NULL);
      break;
  }
  if (template && name && caps) {
    GST_LOG ("creating pad with name %s and caps %s", name,
        gst_caps_to_string (caps));
    pad = gst_pad_new_from_template (template, name);
    gst_pad_use_fixed_caps (pad);
    gst_pad_set_caps (pad, caps);
    gst_caps_unref (caps);
  }

  return pad;
}

static void
gst_ts_demux_stream_added (MpegTSBase * base, MpegTSBaseStream * bstream)
{
  GstTSDemux *tsdemux = (GstTSDemux *) base;
  TSDemuxStream *stream = (TSDemuxStream *) bstream;

  if (!stream->pad) {
    /* Create the pad */
    stream->pad = create_pad_for_stream (tsdemux, bstream);
  }
  stream->flow_return = GST_FLOW_OK;
}

static void
gst_ts_demux_stream_removed (MpegTSBase * base, MpegTSBaseStream * bstream)
{
  TSDemuxStream *stream = (TSDemuxStream *) bstream;

  if (stream->pad) {
    /* Unref the pad, clear it */
    gst_object_unref (stream->pad);
    stream->pad = NULL;
  }
  stream->flow_return = GST_FLOW_NOT_LINKED;
}

static void
activate_pad_for_stream (GstTSDemux * tsdemux, TSDemuxStream * stream)
{
  if (stream->pad) {
    GST_DEBUG_OBJECT (tsdemux, "Adding pad %s:%s",
        GST_DEBUG_PAD_NAME (stream->pad));
    gst_pad_set_active (stream->pad, TRUE);
    gst_element_add_pad ((GstElement *) tsdemux, stream->pad);
    GST_DEBUG_OBJECT (tsdemux, "done adding pad");
  }
}

static void
gst_ts_demux_program_started (MpegTSBase * base, MpegTSBaseProgram * program)
{
  GstTSDemux *demux = GST_TS_DEMUX (base);

  if (demux->program_number == -1 ||
      demux->program_number == program->program_number) {
    guint i;

    GST_LOG ("program %d started", program->program_number);
    demux->program_number = program->program_number;
    demux->program = program;

    /* Activate all stream pads, the pads will already have been created */

    /* FIXME : Actually, we don't want to activate *ALL* streams !
     * For example, we don't want to expose HDV AUX private streams, we will just
     * be using them directly for seeking and metadata. */
    for (i = 0; i < 0x2000; i++)
      if (program->streams[i])
        activate_pad_for_stream (demux, (TSDemuxStream *) program->streams[i]);

  }
}

static void
gst_ts_demux_program_stopped (MpegTSBase * base, MpegTSBaseProgram * program)
{
  GstTSDemux *demux = GST_TS_DEMUX (base);
  GST_LOG ("program %d stopped", program->program_number);

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

 /* ONLY CALL THIS WITH packet->payload != NULL */
static inline void
gst_ts_demux_queue_data (GstTSDemux * demux, TSDemuxStream * stream,
    MpegTSPacketizerPacket * packet)
{
  GstBuffer *buf;

  buf = packet->buffer;
  /* HACK : Instead of creating a new buffer, we just modify the data/size
   * of the buffer to point to the payload */
  GST_BUFFER_DATA (buf) = packet->payload;
  GST_BUFFER_SIZE (buf) = packet->data_end - packet->payload;

  /* Create a new bufferlist */
  if (G_UNLIKELY (stream->current == NULL)) {
    stream->current = gst_buffer_list_new ();
    stream->currentit = gst_buffer_list_iterate (stream->current);
    gst_buffer_list_iterator_add_group (stream->currentit);
    stream->firstbuf = buf;
  }

  /* Set the timestamp */
  if (G_UNLIKELY ((stream->havets == FALSE) &&
          (packet->adaptation_field_control & 0x2) &&
          (packet->afc_flags & MPEGTS_AFC_PCR_FLAG))) {
    GST_DEBUG ("Setting timestamp on first buffer");
    GST_BUFFER_TIMESTAMP (stream->firstbuf) = MPEGTIME_TO_GSTTIME (packet->pcr);
    stream->havets = TRUE;
  }

  /* We add the buffer to the list and increment the reference count
   * since one is currently being owned by mpegpacketizer and will
   * be removed after this packet is being processed. */
  gst_buffer_list_iterator_add (stream->currentit, gst_buffer_ref (buf));
}

static GstFlowReturn
gst_ts_demux_push_pending_data (GstTSDemux * demux, TSDemuxStream * stream)
{
  GstFlowReturn res = GST_FLOW_OK;

  /* Nothing to push out */
  if (G_UNLIKELY (stream->current == NULL))
    goto beach;

  /* FIXME : If needed, parse the packet to extra stream-specific information
   * (like PTS/DTS for mpeg video streams) */

  gst_buffer_list_iterator_free (stream->currentit);

  if (stream->pad) {
    GST_DEBUG_OBJECT (stream->pad, "Pushing buffer list");
    res = gst_pad_push_list (stream->pad, stream->current);
    GST_DEBUG_OBJECT (stream->pad, "Returned %s", gst_flow_get_name (res));
  }

  stream->current = NULL;
  stream->havets = FALSE;

beach:
  return res;
}

static GstFlowReturn
gst_ts_demux_handle_packet (GstTSDemux * demux, TSDemuxStream * stream,
    MpegTSPacketizerPacket * packet, MpegTSPacketizerSection * section)
{
  GstFlowReturn res = GST_FLOW_OK;

  GST_DEBUG ("buffer:%p, data:%p", GST_BUFFER_DATA (packet->buffer),
      packet->data);
  GST_DEBUG ("pid 0x%04x pusi:%d, afc:%d, cont:%d, payload:%p",
      packet->pid,
      packet->payload_unit_start_indicator,
      packet->adaptation_field_control,
      packet->continuity_counter, packet->payload);

  if (section) {
    GST_DEBUG ("section complete:%d, buffer size %d",
        section->complete, GST_BUFFER_SIZE (section->buffer));
  }

  if (packet->payload_unit_start_indicator) {
    res = gst_ts_demux_push_pending_data (demux, stream);
  }

  if (packet->adaptation_field_control & 0x2) {
    if (packet->afc_flags & MPEGTS_AFC_PCR_FLAG)
      GST_DEBUG ("pid 0x%04x pcr:%" GST_TIME_FORMAT " at offset %"
          G_GUINT64_FORMAT, packet->pid,
          GST_TIME_ARGS (MPEGTIME_TO_GSTTIME (packet->pcr)),
          GST_BUFFER_OFFSET (packet->buffer));
    if (packet->afc_flags & MPEGTS_AFC_OPCR_FLAG)
      GST_DEBUG ("pid 0x%04x opcr:%" GST_TIME_FORMAT " at offset %"
          G_GUINT64_FORMAT, packet->pid,
          GST_TIME_ARGS (MPEGTIME_TO_GSTTIME (packet->opcr)),
          GST_BUFFER_OFFSET (packet->buffer));
  }

  if (packet->payload)
    gst_ts_demux_queue_data (demux, stream, packet);

  return res;
}

static GstFlowReturn
gst_ts_demux_push (MpegTSBase * base, MpegTSPacketizerPacket * packet,
    MpegTSPacketizerSection * section)
{
  GstTSDemux *demux = GST_TS_DEMUX_CAST (base);
  TSDemuxStream *stream = NULL;
  GstFlowReturn res = GST_FLOW_OK;

  if (G_LIKELY (demux->program)) {
    stream = (TSDemuxStream *) demux->program->streams[packet->pid];

    if (stream) {
      res = gst_ts_demux_handle_packet (demux, stream, packet, section);
    }
  }
  return res;
}

gboolean
gst_ts_demux_plugin_init (GstPlugin * plugin)
{
  GST_DEBUG_CATEGORY_INIT (ts_demux_debug, "tsdemux", 0,
      "MPEG transport stream demuxer");

  return gst_element_register (plugin, "tsdemux",
      GST_RANK_NONE, GST_TYPE_TS_DEMUX);
}
