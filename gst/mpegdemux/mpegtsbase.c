/*
 * mpegtsbase.c - 
 * Copyright (C) 2007 Alessandro Decina
 * 
 * Authors:
 *   Alessandro Decina <alessandro@nnva.org>
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
#include "gstmpegdesc.h"

/* latency in mseconds */
#define TS_LATENCY 700

#define TABLE_ID_UNSET 0xFF

GST_DEBUG_CATEGORY_STATIC (mpegts_base_debug);
#define GST_CAT_DEFAULT mpegts_base_debug

static GQuark QUARK_PROGRAMS;
static GQuark QUARK_PROGRAM_NUMBER;
static GQuark QUARK_PID;
static GQuark QUARK_PCR_PID;
static GQuark QUARK_STREAMS;
static GQuark QUARK_STREAM_TYPE;

static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("video/mpegts, " "systemstream = (boolean) true ")
    );

enum
{
  ARG_0,
  /* FILL ME */
};

static void mpegts_base_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void mpegts_base_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);
static void mpegts_base_dispose (GObject * object);
static void mpegts_base_finalize (GObject * object);

static void mpegts_base_free_program (MpegTSBaseProgram * program);
static void mpegts_base_free_stream (MpegTSBaseStream * ptream);

static GstFlowReturn mpegts_base_chain (GstPad * pad, GstBuffer * buf);
static gboolean mpegts_base_sink_event (GstPad * pad, GstEvent * event);
static GstStateChangeReturn mpegts_base_change_state (GstElement * element,
    GstStateChange transition);
static void _extra_init (GType type);

GST_BOILERPLATE_FULL (MpegTSBase, mpegts_base, GstElement, GST_TYPE_ELEMENT,
    _extra_init);

static const guint32 crc_tab[256] = {
  0x00000000, 0x04c11db7, 0x09823b6e, 0x0d4326d9, 0x130476dc, 0x17c56b6b,
  0x1a864db2, 0x1e475005, 0x2608edb8, 0x22c9f00f, 0x2f8ad6d6, 0x2b4bcb61,
  0x350c9b64, 0x31cd86d3, 0x3c8ea00a, 0x384fbdbd, 0x4c11db70, 0x48d0c6c7,
  0x4593e01e, 0x4152fda9, 0x5f15adac, 0x5bd4b01b, 0x569796c2, 0x52568b75,
  0x6a1936c8, 0x6ed82b7f, 0x639b0da6, 0x675a1011, 0x791d4014, 0x7ddc5da3,
  0x709f7b7a, 0x745e66cd, 0x9823b6e0, 0x9ce2ab57, 0x91a18d8e, 0x95609039,
  0x8b27c03c, 0x8fe6dd8b, 0x82a5fb52, 0x8664e6e5, 0xbe2b5b58, 0xbaea46ef,
  0xb7a96036, 0xb3687d81, 0xad2f2d84, 0xa9ee3033, 0xa4ad16ea, 0xa06c0b5d,
  0xd4326d90, 0xd0f37027, 0xddb056fe, 0xd9714b49, 0xc7361b4c, 0xc3f706fb,
  0xceb42022, 0xca753d95, 0xf23a8028, 0xf6fb9d9f, 0xfbb8bb46, 0xff79a6f1,
  0xe13ef6f4, 0xe5ffeb43, 0xe8bccd9a, 0xec7dd02d, 0x34867077, 0x30476dc0,
  0x3d044b19, 0x39c556ae, 0x278206ab, 0x23431b1c, 0x2e003dc5, 0x2ac12072,
  0x128e9dcf, 0x164f8078, 0x1b0ca6a1, 0x1fcdbb16, 0x018aeb13, 0x054bf6a4,
  0x0808d07d, 0x0cc9cdca, 0x7897ab07, 0x7c56b6b0, 0x71159069, 0x75d48dde,
  0x6b93dddb, 0x6f52c06c, 0x6211e6b5, 0x66d0fb02, 0x5e9f46bf, 0x5a5e5b08,
  0x571d7dd1, 0x53dc6066, 0x4d9b3063, 0x495a2dd4, 0x44190b0d, 0x40d816ba,
  0xaca5c697, 0xa864db20, 0xa527fdf9, 0xa1e6e04e, 0xbfa1b04b, 0xbb60adfc,
  0xb6238b25, 0xb2e29692, 0x8aad2b2f, 0x8e6c3698, 0x832f1041, 0x87ee0df6,
  0x99a95df3, 0x9d684044, 0x902b669d, 0x94ea7b2a, 0xe0b41de7, 0xe4750050,
  0xe9362689, 0xedf73b3e, 0xf3b06b3b, 0xf771768c, 0xfa325055, 0xfef34de2,
  0xc6bcf05f, 0xc27dede8, 0xcf3ecb31, 0xcbffd686, 0xd5b88683, 0xd1799b34,
  0xdc3abded, 0xd8fba05a, 0x690ce0ee, 0x6dcdfd59, 0x608edb80, 0x644fc637,
  0x7a089632, 0x7ec98b85, 0x738aad5c, 0x774bb0eb, 0x4f040d56, 0x4bc510e1,
  0x46863638, 0x42472b8f, 0x5c007b8a, 0x58c1663d, 0x558240e4, 0x51435d53,
  0x251d3b9e, 0x21dc2629, 0x2c9f00f0, 0x285e1d47, 0x36194d42, 0x32d850f5,
  0x3f9b762c, 0x3b5a6b9b, 0x0315d626, 0x07d4cb91, 0x0a97ed48, 0x0e56f0ff,
  0x1011a0fa, 0x14d0bd4d, 0x19939b94, 0x1d528623, 0xf12f560e, 0xf5ee4bb9,
  0xf8ad6d60, 0xfc6c70d7, 0xe22b20d2, 0xe6ea3d65, 0xeba91bbc, 0xef68060b,
  0xd727bbb6, 0xd3e6a601, 0xdea580d8, 0xda649d6f, 0xc423cd6a, 0xc0e2d0dd,
  0xcda1f604, 0xc960ebb3, 0xbd3e8d7e, 0xb9ff90c9, 0xb4bcb610, 0xb07daba7,
  0xae3afba2, 0xaafbe615, 0xa7b8c0cc, 0xa379dd7b, 0x9b3660c6, 0x9ff77d71,
  0x92b45ba8, 0x9675461f, 0x8832161a, 0x8cf30bad, 0x81b02d74, 0x857130c3,
  0x5d8a9099, 0x594b8d2e, 0x5408abf7, 0x50c9b640, 0x4e8ee645, 0x4a4ffbf2,
  0x470cdd2b, 0x43cdc09c, 0x7b827d21, 0x7f436096, 0x7200464f, 0x76c15bf8,
  0x68860bfd, 0x6c47164a, 0x61043093, 0x65c52d24, 0x119b4be9, 0x155a565e,
  0x18197087, 0x1cd86d30, 0x029f3d35, 0x065e2082, 0x0b1d065b, 0x0fdc1bec,
  0x3793a651, 0x3352bbe6, 0x3e119d3f, 0x3ad08088, 0x2497d08d, 0x2056cd3a,
  0x2d15ebe3, 0x29d4f654, 0xc5a92679, 0xc1683bce, 0xcc2b1d17, 0xc8ea00a0,
  0xd6ad50a5, 0xd26c4d12, 0xdf2f6bcb, 0xdbee767c, 0xe3a1cbc1, 0xe760d676,
  0xea23f0af, 0xeee2ed18, 0xf0a5bd1d, 0xf464a0aa, 0xf9278673, 0xfde69bc4,
  0x89b8fd09, 0x8d79e0be, 0x803ac667, 0x84fbdbd0, 0x9abc8bd5, 0x9e7d9662,
  0x933eb0bb, 0x97ffad0c, 0xafb010b1, 0xab710d06, 0xa6322bdf, 0xa2f33668,
  0xbcb4666d, 0xb8757bda, 0xb5365d03, 0xb1f740b4
};

/* relicenced to LGPL from fluendo ts demuxer */
static guint32
mpegts_base_calc_crc32 (guint8 * data, guint datalen)
{
  gint i;
  guint32 crc = 0xffffffff;

  for (i = 0; i < datalen; i++) {
    crc = (crc << 8) ^ crc_tab[((crc >> 24) ^ *data++) & 0xff];
  }
  return crc;
}

static void
_extra_init (GType type)
{
  QUARK_PROGRAMS = g_quark_from_string ("programs");
  QUARK_PROGRAM_NUMBER = g_quark_from_string ("program-number");
  QUARK_PID = g_quark_from_string ("pid");
  QUARK_PCR_PID = g_quark_from_string ("pcr-pid");
  QUARK_STREAMS = g_quark_from_string ("streams");
  QUARK_STREAM_TYPE = g_quark_from_string ("stream-type");
}

static void
mpegts_base_base_init (gpointer klass)
{
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);

  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&sink_template));
}

static void
mpegts_base_class_init (MpegTSBaseClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *element_class;

  element_class = GST_ELEMENT_CLASS (klass);
  element_class->change_state = mpegts_base_change_state;

  gobject_class = G_OBJECT_CLASS (klass);
  gobject_class->set_property = mpegts_base_set_property;
  gobject_class->get_property = mpegts_base_get_property;
  gobject_class->dispose = mpegts_base_dispose;
  gobject_class->finalize = mpegts_base_finalize;

}

static gboolean
foreach_psi_pid_remove (gpointer key, gpointer value, gpointer data)
{
  return TRUE;
}

static void
mpegts_base_reset (MpegTSBase * base)
{
  mpegts_packetizer_clear (base->packetizer);
  g_hash_table_foreach_remove (base->psi_pids, foreach_psi_pid_remove, NULL);

  /* PAT */
  g_hash_table_insert (base->psi_pids,
      GINT_TO_POINTER (0), GINT_TO_POINTER (1));
  if (base->pat != NULL)
    gst_structure_free (base->pat);
  base->pat = NULL;
  /* pmt pids will be added and removed dynamically */

}

static void
mpegts_base_init (MpegTSBase * base, MpegTSBaseClass * klass)
{
  base->sinkpad = gst_pad_new_from_static_template (&sink_template, "sink");
  gst_pad_set_chain_function (base->sinkpad, mpegts_base_chain);
  gst_pad_set_event_function (base->sinkpad, mpegts_base_sink_event);
  gst_element_add_pad (GST_ELEMENT (base), base->sinkpad);

  base->disposed = FALSE;
  base->packetizer = mpegts_packetizer_new ();
  base->programs = g_hash_table_new_full (g_direct_hash, g_direct_equal,
      NULL, (GDestroyNotify) mpegts_base_free_program);
  base->psi_pids = g_hash_table_new (g_direct_hash, g_direct_equal);
  base->pes_pids = g_hash_table_new (g_direct_hash, g_direct_equal);

  mpegts_base_reset (base);
  base->program_size = sizeof (MpegTSBaseProgram);
}

static void
mpegts_base_dispose (GObject * object)
{
  MpegTSBase *base = GST_MPEGTS_BASE (object);

  if (!base->disposed) {
    g_object_unref (base->packetizer);
    base->disposed = TRUE;
  }

  if (G_OBJECT_CLASS (parent_class)->dispose)
    G_OBJECT_CLASS (parent_class)->dispose (object);
}

static void
mpegts_base_finalize (GObject * object)
{
  MpegTSBase *base = GST_MPEGTS_BASE (object);

  if (base->pat) {
    gst_structure_free (base->pat);
    base->pat = NULL;
  }
  g_hash_table_destroy (base->programs);
  g_hash_table_destroy (base->psi_pids);
  g_hash_table_destroy (base->pes_pids);

  if (G_OBJECT_CLASS (parent_class)->finalize)
    G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
mpegts_base_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  /* MpegTSBase *base = GST_MPEGTS_BASE (object); */

  switch (prop_id) {
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
  }
}

static void
mpegts_base_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  /* MpegTSBase *base = GST_MPEGTS_BASE (object); */

  switch (prop_id) {
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
  }
}

MpegTSBaseProgram *
mpegts_base_add_program (MpegTSBase * base,
    gint program_number, guint16 pmt_pid)
{
  MpegTSBaseProgram *program;

  program = g_malloc0 (base->program_size);
  program->program_number = program_number;
  program->pmt_pid = pmt_pid;
  program->pcr_pid = G_MAXUINT16;
  program->streams = g_hash_table_new_full (g_direct_hash, g_direct_equal,
      NULL, (GDestroyNotify) mpegts_base_free_stream);
  program->patcount = 0;

  g_hash_table_insert (base->programs,
      GINT_TO_POINTER (program_number), program);

  /* FIXME : Signal subclasses about new program ? */
  return program;
}

MpegTSBaseProgram *
mpegts_base_get_program (MpegTSBase * base, gint program_number)
{
  MpegTSBaseProgram *program;

  program = (MpegTSBaseProgram *) g_hash_table_lookup (base->programs,
      GINT_TO_POINTER ((gint) program_number));

  return program;
}

#if 0
static GstPad *
mpegts_base_activate_program (MpegTSBase * base, MpegTSBaseProgram * program)
{
  MpegTSBasePad *tspad;
  gchar *pad_name;

  pad_name = g_strdup_printf ("program_%d", program->program_number);

  tspad = mpegts_base_create_tspad (base, pad_name);
  tspad->program_number = program->program_number;
  tspad->program = program;
  program->tspad = tspad;
  g_free (pad_name);
  gst_pad_set_active (tspad->pad, TRUE);
  program->active = TRUE;

  return tspad->pad;
}

static GstPad *
mpegts_base_deactivate_program (MpegTSBase * base, MpegTSBaseProgram * program)
{
  MpegTSBasePad *tspad;

  tspad = program->tspad;
  gst_pad_set_active (tspad->pad, FALSE);
  program->active = FALSE;

  /* tspad will be destroyed in GstElementClass::pad_removed */

  return tspad->pad;
}
#endif


static void
mpegts_base_free_program (MpegTSBaseProgram * program)
{
  if (program->pmt_info)
    gst_structure_free (program->pmt_info);

  g_hash_table_destroy (program->streams);

  g_free (program);
}

static void
mpegts_base_remove_program (MpegTSBase * base, gint program_number)
{
  g_hash_table_remove (base->programs, GINT_TO_POINTER (program_number));
}

static MpegTSBaseStream *
mpegts_base_program_add_stream (MpegTSBase * base,
    MpegTSBaseProgram * program, guint16 pid, guint8 stream_type)
{
  MpegTSBaseStream *stream;

  /* FIXME : Signal subclasses ? */
  /* FIXME : allow Stream subclasses ? */
  stream = g_new0 (MpegTSBaseStream, 1);
  stream->pid = pid;
  stream->stream_type = stream_type;

  g_hash_table_insert (program->streams, GINT_TO_POINTER ((gint) pid), stream);

  return stream;
}

static void
mpegts_base_free_stream (MpegTSBaseStream * stream)
{
  g_free (stream);
}

static void
mpegts_base_program_remove_stream (MpegTSBase * base,
    MpegTSBaseProgram * program, guint16 pid)
{
  g_hash_table_remove (program->streams, GINT_TO_POINTER ((gint) pid));
}

static void
mpegts_base_deactivate_pmt (MpegTSBase * base, MpegTSBaseProgram * program)
{
  gint i;
  guint pid;
  guint stream_type;
  GstStructure *stream;
  const GValue *streams;
  const GValue *value;

  if (program->pmt_info) {
    streams = gst_structure_id_get_value (program->pmt_info, QUARK_STREAMS);

    for (i = 0; i < gst_value_list_get_size (streams); ++i) {
      value = gst_value_list_get_value (streams, i);
      stream = g_value_get_boxed (value);
      gst_structure_id_get (stream, QUARK_PID, G_TYPE_UINT, &pid,
          QUARK_STREAM_TYPE, G_TYPE_UINT, &stream_type, NULL);
      mpegts_base_program_remove_stream (base, program, (guint16) pid);
      g_hash_table_remove (base->pes_pids, GINT_TO_POINTER ((gint) pid));
    }

    /* remove pcr stream */
    mpegts_base_program_remove_stream (base, program, program->pcr_pid);
    g_hash_table_remove (base->pes_pids,
        GINT_TO_POINTER ((gint) program->pcr_pid));
  }
}


static gboolean
mpegts_base_is_psi (MpegTSBase * base, MpegTSPacketizerPacket * packet)
{
  gboolean retval = FALSE;
  guint8 table_id;
  int i;
  static const guint8 si_tables[] =
      { 0x00, 0x01, 0x02, 0x03, 0x40, 0x41, 0x42, 0x46, 0x4A,
    0x4E, 0x4F, 0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59,
    0x5A, 0x5B, 0x5C, 0x5D, 0x5E, 0x5F, 0x60, 0x61, 0x62, 0x63, 0x64, 0x65,
    0x66, 0x67, 0x68, 0x69, 0x6A, 0x6B, 0x6C, 0x6D, 0x6E, 0x6F, 0x70, 0x71,
    0x72, 0x73, 0x7E, 0x7F, TABLE_ID_UNSET
  };
  if (g_hash_table_lookup (base->psi_pids,
          GINT_TO_POINTER ((gint) packet->pid)) != NULL)
    retval = TRUE;
  /* check is it is a pes pid */
  if (g_hash_table_lookup (base->pes_pids,
          GINT_TO_POINTER ((gint) packet->pid)) != NULL)
    return FALSE;
  if (!retval) {
    if (packet->payload_unit_start_indicator) {
      table_id = *(packet->data);
      i = 0;
      while (si_tables[i] != TABLE_ID_UNSET) {
        if (G_UNLIKELY (si_tables[i] == table_id)) {
          GST_DEBUG_OBJECT (base, "Packet has table id 0x%x", table_id);
          retval = TRUE;
          break;
        }
        i++;
      }
    } else {
      MpegTSPacketizerStream *stream = (MpegTSPacketizerStream *)
          base->packetizer->streams[packet->pid];

      if (stream) {
        i = 0;
        GST_DEBUG_OBJECT (base, "section table id: 0x%x",
            stream->section_table_id);
        while (si_tables[i] != TABLE_ID_UNSET) {
          if (G_UNLIKELY (si_tables[i] == stream->section_table_id)) {
            retval = TRUE;
            break;
          }
          i++;
        }
      }
    }
  }

  GST_DEBUG_OBJECT (base, "Packet of pid 0x%x is psi: %d", packet->pid, retval);
  return retval;
}

static void
mpegts_base_apply_pat (MpegTSBase * base, GstStructure * pat_info)
{
  const GValue *value;
  GstStructure *old_pat;
  GstStructure *program_info;
  guint program_number;
  guint pid;
  MpegTSBaseProgram *program;
  gint i;
  const GValue *programs;

  old_pat = base->pat;
  base->pat = gst_structure_copy (pat_info);

  GST_INFO_OBJECT (base, "PAT %" GST_PTR_FORMAT, pat_info);

  gst_element_post_message (GST_ELEMENT_CAST (base),
      gst_message_new_element (GST_OBJECT (base),
          gst_structure_copy (pat_info)));

  GST_OBJECT_LOCK (base);
  programs = gst_structure_id_get_value (pat_info, QUARK_PROGRAMS);
  /* activate the new table */
  for (i = 0; i < gst_value_list_get_size (programs); ++i) {
    value = gst_value_list_get_value (programs, i);

    program_info = g_value_get_boxed (value);
    gst_structure_id_get (program_info, QUARK_PROGRAM_NUMBER, G_TYPE_UINT,
        &program_number, QUARK_PID, G_TYPE_UINT, &pid, NULL);

    program = mpegts_base_get_program (base, program_number);
    if (program) {
      if (program->pmt_pid != pid) {
        if (program->pmt_pid != G_MAXUINT16) {
          /* pmt pid changed */
          g_hash_table_remove (base->psi_pids,
              GINT_TO_POINTER ((gint) program->pmt_pid));
        }

        program->pmt_pid = pid;
        g_hash_table_insert (base->psi_pids,
            GINT_TO_POINTER ((gint) pid), GINT_TO_POINTER (1));
      }
    } else {
      g_hash_table_insert (base->psi_pids,
          GINT_TO_POINTER ((gint) pid), GINT_TO_POINTER (1));
      program = mpegts_base_add_program (base, program_number, pid);
    }
    program->patcount += 1;
  }

  if (old_pat) {
    /* deactivate the old table */

    programs = gst_structure_id_get_value (old_pat, QUARK_PROGRAMS);
    for (i = 0; i < gst_value_list_get_size (programs); ++i) {
      value = gst_value_list_get_value (programs, i);

      program_info = g_value_get_boxed (value);
      gst_structure_id_get (program_info,
          QUARK_PROGRAM_NUMBER, G_TYPE_UINT, &program_number,
          QUARK_PID, G_TYPE_UINT, &pid, NULL);

      program = mpegts_base_get_program (base, program_number);
      if (program == NULL) {
        GST_DEBUG_OBJECT (base, "broken PAT, duplicated entry for program %d",
            program_number);
        continue;
      }

      if (--program->patcount > 0)
        /* the program has been referenced by the new pat, keep it */
        continue;

      GST_INFO_OBJECT (base, "PAT removing program %" GST_PTR_FORMAT,
          program_info);

      mpegts_base_deactivate_pmt (base, program);
      mpegts_base_remove_program (base, program_number);
      g_hash_table_remove (base->psi_pids, GINT_TO_POINTER ((gint) pid));
      mpegts_packetizer_remove_stream (base->packetizer, pid);
    }

    gst_structure_free (old_pat);
  }

  GST_OBJECT_UNLOCK (base);

#if 0
  mpegts_base_sync_program_pads (base);
#endif
}

static void
mpegts_base_apply_pmt (MpegTSBase * base,
    guint16 pmt_pid, GstStructure * pmt_info)
{
  MpegTSBaseProgram *program;
  guint program_number;
  guint pcr_pid;
  guint pid;
  guint stream_type;
  GstStructure *stream;
  gint i;
  const GValue *new_streams;
  const GValue *value;

  gst_structure_id_get (pmt_info,
      QUARK_PROGRAM_NUMBER, G_TYPE_UINT, &program_number,
      QUARK_PCR_PID, G_TYPE_UINT, &pcr_pid, NULL);
  new_streams = gst_structure_id_get_value (pmt_info, QUARK_STREAMS);

  GST_OBJECT_LOCK (base);
  program = mpegts_base_get_program (base, program_number);
  if (program) {
    /* deactivate old pmt */
    mpegts_base_deactivate_pmt (base, program);
    if (program->pmt_info)
      gst_structure_free (program->pmt_info);
    program->pmt_info = NULL;
  } else {
    /* no PAT?? */
    g_hash_table_insert (base->psi_pids,
        GINT_TO_POINTER ((gint) pmt_pid), GINT_TO_POINTER (1));
    program = mpegts_base_add_program (base, program_number, pid);
  }

  /* activate new pmt */
  program->pmt_info = gst_structure_copy (pmt_info);
  program->pmt_pid = pmt_pid;
  program->pcr_pid = pcr_pid;
  mpegts_base_program_add_stream (base, program, (guint16) pcr_pid, -1);
  g_hash_table_insert (base->pes_pids, GINT_TO_POINTER ((gint) pcr_pid),
      GINT_TO_POINTER (1));

  for (i = 0; i < gst_value_list_get_size (new_streams); ++i) {
    value = gst_value_list_get_value (new_streams, i);
    stream = g_value_get_boxed (value);

    gst_structure_id_get (stream, QUARK_PID, G_TYPE_UINT, &pid,
        QUARK_STREAM_TYPE, G_TYPE_UINT, &stream_type, NULL);
    mpegts_base_program_add_stream (base, program,
        (guint16) pid, (guint8) stream_type);
    g_hash_table_insert (base->pes_pids, GINT_TO_POINTER ((gint) pid),
        GINT_TO_POINTER ((gint) 1));

  }
  GST_OBJECT_UNLOCK (base);

  GST_DEBUG_OBJECT (base, "new pmt %" GST_PTR_FORMAT, pmt_info);

  gst_element_post_message (GST_ELEMENT_CAST (base),
      gst_message_new_element (GST_OBJECT (base),
          gst_structure_copy (pmt_info)));
}

static void
mpegts_base_apply_nit (MpegTSBase * base,
    guint16 pmt_pid, GstStructure * nit_info)
{
  gst_element_post_message (GST_ELEMENT_CAST (base),
      gst_message_new_element (GST_OBJECT (base),
          gst_structure_copy (nit_info)));
}

static void
mpegts_base_apply_sdt (MpegTSBase * base,
    guint16 pmt_pid, GstStructure * sdt_info)
{
  gst_element_post_message (GST_ELEMENT_CAST (base),
      gst_message_new_element (GST_OBJECT (base),
          gst_structure_copy (sdt_info)));
}

static void
mpegts_base_apply_eit (MpegTSBase * base,
    guint16 pmt_pid, GstStructure * eit_info)
{
  gst_element_post_message (GST_ELEMENT_CAST (base),
      gst_message_new_element (GST_OBJECT (base),
          gst_structure_copy (eit_info)));
}

static gboolean
mpegts_base_handle_psi (MpegTSBase * base, MpegTSPacketizerSection * section)
{
  gboolean res = TRUE;
  GstStructure *structure = NULL;

  if (G_UNLIKELY (mpegts_base_calc_crc32 (GST_BUFFER_DATA (section->buffer),
              GST_BUFFER_SIZE (section->buffer)) != 0)) {
    GST_WARNING_OBJECT (base, "bad crc in psi pid 0x%x", section->pid);
    return FALSE;
  }

  switch (section->table_id) {
    case 0x00:
      /* PAT */
      structure = mpegts_packetizer_parse_pat (base->packetizer, section);
      if (G_LIKELY (structure))
        mpegts_base_apply_pat (base, structure);
      else
        res = FALSE;

      break;
    case 0x02:
      structure = mpegts_packetizer_parse_pmt (base->packetizer, section);
      if (G_LIKELY (structure))
        mpegts_base_apply_pmt (base, section->pid, structure);
      else
        res = FALSE;

      break;
    case 0x40:
      /* NIT, actual network */
    case 0x41:
      /* NIT, other network */
      structure = mpegts_packetizer_parse_nit (base->packetizer, section);
      if (G_LIKELY (structure))
        mpegts_base_apply_nit (base, section->pid, structure);
      else
        res = FALSE;

      break;
    case 0x42:
    case 0x46:
      structure = mpegts_packetizer_parse_sdt (base->packetizer, section);
      if (G_LIKELY (structure))
        mpegts_base_apply_sdt (base, section->pid, structure);
      else
        res = FALSE;
      break;
    case 0x4E:
    case 0x4F:
      /* EIT, present/following */
    case 0x50:
    case 0x51:
    case 0x52:
    case 0x53:
    case 0x54:
    case 0x55:
    case 0x56:
    case 0x57:
    case 0x58:
    case 0x59:
    case 0x5A:
    case 0x5B:
    case 0x5C:
    case 0x5D:
    case 0x5E:
    case 0x5F:
    case 0x60:
    case 0x61:
    case 0x62:
    case 0x63:
    case 0x64:
    case 0x65:
    case 0x66:
    case 0x67:
    case 0x68:
    case 0x69:
    case 0x6A:
    case 0x6B:
    case 0x6C:
    case 0x6D:
    case 0x6E:
    case 0x6F:
      /* EIT, schedule */
      structure = mpegts_packetizer_parse_eit (base->packetizer, section);
      if (G_LIKELY (structure))
        mpegts_base_apply_eit (base, section->pid, structure);
      else
        res = FALSE;
      break;
    default:
      break;
  }

  if (structure)
    gst_structure_free (structure);

  return res;
}

static gboolean
mpegts_base_sink_event (GstPad * pad, GstEvent * event)
{
  gboolean res;
  MpegTSBase *base = GST_MPEGTS_BASE (gst_object_get_parent (GST_OBJECT (pad)));

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_FLUSH_STOP:
      mpegts_packetizer_clear (base->packetizer);
      res = gst_pad_event_default (pad, event);
      break;
    default:
      res = gst_pad_event_default (pad, event);
  }

  gst_object_unref (base);
  return res;
}

static inline GstFlowReturn
mpegts_base_push (MpegTSBase * base, MpegTSPacketizerPacket * packet,
    MpegTSPacketizerSection * section)
{
  MpegTSBaseClass *klass = GST_MPEGTS_BASE_GET_CLASS (base);

  /* Call implementation */
  if (G_UNLIKELY (klass->push == NULL)) {
    GST_ERROR_OBJECT (base, "Class doesn't have a 'push' implementation !");
    return GST_FLOW_ERROR;
  }

  return klass->push (base, packet, section);
}

static GstFlowReturn
mpegts_base_chain (GstPad * pad, GstBuffer * buf)
{
  GstFlowReturn res = GST_FLOW_OK;
  MpegTSBase *base;
  gboolean based;
  MpegTSPacketizerPacketReturn pret;
  MpegTSPacketizer *packetizer;
  MpegTSPacketizerPacket packet;

  base = GST_MPEGTS_BASE (gst_object_get_parent (GST_OBJECT (pad)));
  packetizer = base->packetizer;

  mpegts_packetizer_push (base->packetizer, buf);
  while (((pret =
              mpegts_packetizer_next_packet (base->packetizer,
                  &packet)) != PACKET_NEED_MORE) && !GST_FLOW_IS_FATAL (res)) {
    if (G_UNLIKELY (pret == PACKET_BAD))
      /* bad header, skip the packet */
      goto next;

    /* base PSI data */
    if (packet.payload != NULL && mpegts_base_is_psi (base, &packet)) {
      MpegTSPacketizerSection section;

      based = mpegts_packetizer_push_section (packetizer, &packet, &section);
      if (G_UNLIKELY (!based))
        /* bad section data */
        goto next;

      if (G_LIKELY (section.complete)) {
        /* section complete */
        based = mpegts_base_handle_psi (base, &section);
        gst_buffer_unref (section.buffer);

        if (G_UNLIKELY (!based))
          /* bad PSI table */
          goto next;
      }
      /* we need to push section packet downstream */
      res = mpegts_base_push (base, &packet, &section);

    } else {
      /* push the packet downstream */
      res = mpegts_base_push (base, &packet, NULL);
    }

  next:
    mpegts_packetizer_clear_packet (base->packetizer, &packet);
  }

  gst_object_unref (base);
  return res;
}

static GstStateChangeReturn
mpegts_base_change_state (GstElement * element, GstStateChange transition)
{
  MpegTSBase *base;
  GstStateChangeReturn ret;

  base = GST_MPEGTS_BASE (element);
  ret = GST_ELEMENT_CLASS (parent_class)->change_state (element, transition);

  switch (transition) {
    case GST_STATE_CHANGE_PAUSED_TO_READY:
      mpegts_base_reset (base);
      break;
    default:
      break;
  }

  return ret;
}

gboolean
gst_mpegtsbase_plugin_init (GstPlugin * plugin)
{
  GST_DEBUG_CATEGORY_INIT (mpegts_base_debug, "mpegtsbase", 0,
      "MPEG transport stream base class");

  gst_mpegtsdesc_init_debug ();

  return TRUE;
}
