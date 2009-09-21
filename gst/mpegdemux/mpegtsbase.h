/*
 * mpegtsbase.h - GStreamer MPEG transport stream base class
 * Copyright (C) 2009 Edward Hervey <bilboed@bilboed.com>
 *               2007 Alessandro Decina
 * 
 * Authors:
 *   Alessandro Decina <alessandro@nnva.org>
 *   Edward Hervey <bilboed@bilboed.com>
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


#ifndef GST_MPEG_TS_BASE_H
#define GST_MPEG_TS_BASE_H

#include <gst/gst.h>
#include "mpegtspacketizer.h"

G_BEGIN_DECLS

#define GST_TYPE_MPEGTS_BASE \
  (mpegts_base_get_type())
#define GST_MPEGTS_BASE(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_MPEGTS_BASE,MpegTSBase))
#define GST_MPEGTS_BASE_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_MPEGTS_BASE,MpegTSBaseClass))
#define GST_IS_MPEGTS_BASE(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_MPEGTS_BASE))
#define GST_IS_MPEGTS_BASE_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_MPEGTS_BASE))
#define GST_MPEGTS_BASE_GET_CLASS(obj) \
  (G_TYPE_INSTANCE_GET_CLASS ((obj), GST_TYPE_MPEGTS_BASE, MpegTSBaseClass))

typedef struct _MpegTSBase MpegTSBase;
typedef struct _MpegTSBaseClass MpegTSBaseClass;
typedef struct _MpegTSBaseStream MpegTSBaseStream;
typedef struct _MpegTSBaseProgram MpegTSBaseProgram;

struct _MpegTSBaseStream
{
  guint16 pid;
  guint8 stream_type;
};

struct _MpegTSBaseProgram
{
  gint program_number;
  guint16 pmt_pid;
  guint16 pcr_pid;
  GstStructure *pmt_info;
  GHashTable *streams;
  gint patcount;
};


struct _MpegTSBase {
  GstElement element;

  GstPad *sinkpad;

  /* the following vars must be protected with the OBJECT_LOCK as they can be
   * accessed from the application thread and the streaming thread */
  GHashTable *programs;

  GstStructure *pat;
  MpegTSPacketizer *packetizer;
  /* arrays that say whether a pid is a known psi pid or a pes pid */
  gboolean *known_psi;
  gboolean *is_pes;

  gboolean disposed;

  /* size of the MpegTSBaseProgram structure, can be overridden
   * by subclasses if they have their own MpegTSBaseProgram subclasses. */
  gsize program_size;
};

struct _MpegTSBaseClass {
  GstElementClass parent_class;

  /* Virtual methods */
  GstFlowReturn (*push) (MpegTSBase *base, MpegTSPacketizerPacket *packet, MpegTSPacketizerSection * section);

  /* signals */
  void (*pat_info) (GstStructure *pat);
  void (*pmt_info) (GstStructure *pmt);
  void (*nit_info) (GstStructure *nit);
  void (*sdt_info) (GstStructure *sdt);
  void (*eit_info) (GstStructure *eit);
};

GType mpegts_base_get_type(void);

MpegTSBaseProgram *mpegts_base_get_program (MpegTSBase * base, gint program_number);
MpegTSBaseProgram *mpegts_base_add_program (MpegTSBase * base, gint program_number, guint16 pmt_pid);
gboolean gst_mpegtsbase_plugin_init (GstPlugin * plugin);

G_END_DECLS

#endif /* GST_MPEG_TS_BASE_H */
