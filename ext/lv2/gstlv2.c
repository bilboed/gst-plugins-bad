/* GStreamer
 * Copyright (C) 1999 Erik Walthinsen <omega@cse.ogi.edu>
 *               2001 Steve Baker <stevebaker_org@yahoo.co.uk>
 *               2003 Andy Wingo <wingo at pobox.com>
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
#include <string.h>
#include <math.h>
#include <gst/audio/audio.h>
#include <gst/controller/gstcontroller.h>

#include "gstlv2.h"
#include <slv2/slv2.h>

#define GST_SLV2_PLUGIN_QDATA g_quark_from_static_string("slv2-plugin")

static void gst_lv2_set_property (GObject * object,
    guint prop_id, const GValue * value, GParamSpec * pspec);

static void gst_lv2_get_property (GObject * object,
    guint prop_id, GValue * value, GParamSpec * pspec);

static gboolean gst_lv2_setup (GstSignalProcessor * sigproc, guint sample_rate);
static gboolean gst_lv2_start (GstSignalProcessor * sigproc);
static void gst_lv2_stop (GstSignalProcessor * sigproc);
static void gst_lv2_cleanup (GstSignalProcessor * sigproc);
static void gst_lv2_process (GstSignalProcessor * sigproc, guint nframes);

static SLV2World world;
SLV2Value audio_class;
SLV2Value control_class;
SLV2Value input_class;
SLV2Value output_class;
SLV2Value integer_prop;
SLV2Value toggled_prop;

static GstSignalProcessorClass *parent_class;

static GstPlugin *gst_lv2_plugin;

GST_DEBUG_CATEGORY_STATIC (lv2_debug);
#define GST_CAT_DEFAULT lv2_debug


static void
gst_lv2_base_init (gpointer g_class)
{
  GstLV2Class *klass = (GstLV2Class *) g_class;
  GstElementClass *element_class = GST_ELEMENT_CLASS (g_class);
  GstSignalProcessorClass *gsp_class = GST_SIGNAL_PROCESSOR_CLASS (g_class);
  GstElementDetails *details;
  SLV2Plugin lv2plugin;
  SLV2Value val;
  guint j, audio_in_count, audio_out_count, control_in_count, control_out_count;
  gchar *klass_tags;

  GST_DEBUG ("base_init %p", g_class);

  lv2plugin = (SLV2Plugin) g_type_get_qdata (G_OBJECT_CLASS_TYPE (klass),
      GST_SLV2_PLUGIN_QDATA);

  g_assert (lv2plugin);

  /* pad templates */
  gsp_class->num_audio_in = 0;
  gsp_class->num_audio_out = 0;
  /* properties */
  gsp_class->num_control_in = 0;
  gsp_class->num_control_out = 0;

  for (j = 0; j < slv2_plugin_get_num_ports (lv2plugin); j++) {
    SLV2Port port = slv2_plugin_get_port_by_index (lv2plugin, j);
    if (slv2_port_is_a (lv2plugin, port, audio_class)) {
      gchar *name =
          g_strdup (slv2_value_as_string (slv2_port_get_symbol (lv2plugin,
                  port)));

      GST_DEBUG ("LV2 port name: \"%s\"", name);

      if (slv2_port_is_a (lv2plugin, port, input_class))
        gst_signal_processor_class_add_pad_template (gsp_class, name,
            GST_PAD_SINK, gsp_class->num_audio_in++);
      else if (slv2_port_is_a (lv2plugin, port, output_class))
        gst_signal_processor_class_add_pad_template (gsp_class, name,
            GST_PAD_SRC, gsp_class->num_audio_out++);
      /* TODO: else ignore plugin */

      g_free (name);
    } else if (slv2_port_is_a (lv2plugin, port, control_class)) {
      if (slv2_port_is_a (lv2plugin, port, input_class))
        gsp_class->num_control_in++;
      else if (slv2_port_is_a (lv2plugin, port, output_class))
        gsp_class->num_control_out++;
      /* TODO: else ignore plugin */
    }
    /* TODO: else ignore plugin */
  }

  /* construct the element details struct */
  details = g_new0 (GstElementDetails, 1);
  val = slv2_plugin_get_name (lv2plugin);
  if (val) {
    details->longname = g_strdup (slv2_value_as_string (val));
    slv2_value_free (val);
  } else {
    details->longname = g_strdup ("no description available");
  }
  details->description = details->longname;
  val = slv2_plugin_get_author_name (lv2plugin);
  if (val) {
    details->author = g_strdup (slv2_value_as_string (val));
    slv2_value_free (val);
  } else {
    details->author = g_strdup ("no author available");
  }

  if (gsp_class->num_audio_in == 0)
    klass_tags = "Source/Audio/LV2";
  else if (gsp_class->num_audio_out == 0) {
    if (gsp_class->num_control_out == 0)
      klass_tags = "Sink/Audio/LV2";
    else
      klass_tags = "Sink/Analyzer/Audio/LV2";
  } else
    klass_tags = "Filter/Effect/Audio/LV2";

  details->klass = klass_tags;
  GST_INFO ("tags : %s", details->klass);
  gst_element_class_set_details (element_class, details);
  g_free (details->longname);
  g_free (details->author);
  g_free (details);

  klass->audio_in_portnums = g_new0 (gint, gsp_class->num_audio_in);
  klass->audio_out_portnums = g_new0 (gint, gsp_class->num_audio_out);
  klass->control_in_portnums = g_new0 (gint, gsp_class->num_control_in);
  klass->control_out_portnums = g_new0 (gint, gsp_class->num_control_out);

  audio_in_count = audio_out_count = control_in_count = control_out_count = 0;

  for (j = 0; j < slv2_plugin_get_num_ports (lv2plugin); j++) {
    SLV2Port port = slv2_plugin_get_port_by_index (lv2plugin, j);
    gboolean is_input = slv2_port_is_a (lv2plugin, port, input_class);
    if (slv2_port_is_a (lv2plugin, port, audio_class)) {
      if (is_input)
        klass->audio_in_portnums[audio_in_count++] = j;
      else
        klass->audio_out_portnums[audio_out_count++] = j;
    } else if (slv2_port_is_a (lv2plugin, port, control_class)) {
      if (is_input)
        klass->control_in_portnums[control_in_count++] = j;
      else
        klass->control_out_portnums[control_out_count++] = j;
    }
  }

  g_assert (audio_in_count == gsp_class->num_audio_in);
  g_assert (audio_out_count == gsp_class->num_audio_out);
  g_assert (control_in_count == gsp_class->num_control_in);
  g_assert (control_out_count == gsp_class->num_control_out);

  /*if (!LV2_IS_INPLACE_BROKEN (desc->Properties))
     GST_SIGNAL_PROCESSOR_CLASS_SET_CAN_PROCESS_IN_PLACE (klass); */

  klass->plugin = lv2plugin;
}

static gchar *
gst_lv2_class_get_param_name (GstLV2Class * klass, gint portnum)
{
  SLV2Plugin lv2plugin = klass->plugin;
  SLV2Port port = slv2_plugin_get_port_by_index (lv2plugin, portnum);
  return g_strdup (slv2_value_as_string (slv2_port_get_symbol (lv2plugin,
              port)));
}

static GParamSpec *
gst_lv2_class_get_param_spec (GstLV2Class * klass, gint portnum)
{
  SLV2Plugin lv2plugin = klass->plugin;
  SLV2Port port = slv2_plugin_get_port_by_index (lv2plugin, portnum);
  SLV2Value lv2def, lv2min, lv2max;
  GParamSpec *ret;
  gchar *name;
  gint perms;
  gfloat lower = 0.0f, upper = 1.0f, def = 0.0f;

  name = gst_lv2_class_get_param_name (klass, portnum);
  perms = G_PARAM_READABLE;
  if (slv2_port_is_a (lv2plugin, port, input_class))
    perms |= G_PARAM_WRITABLE | G_PARAM_CONSTRUCT;
  if (slv2_port_is_a (lv2plugin, port, control_class))
    perms |= GST_PARAM_CONTROLLABLE;

  if (slv2_port_has_property (lv2plugin, port, toggled_prop)) {
    ret = g_param_spec_boolean (name, name, name, FALSE, perms);
    g_free (name);
    return ret;
  }

  slv2_port_get_range (lv2plugin, port, &lv2def, &lv2min, &lv2max);

  if (lv2def)
    def = slv2_value_as_float (lv2def);
  if (lv2min)
    lower = slv2_value_as_float (lv2min);
  if (lv2max)
    upper = slv2_value_as_float (lv2max);

  if (def < lower) {
    GST_WARNING ("%s has lower bound %f > default %f\n",
        slv2_value_as_string (slv2_plugin_get_uri (lv2plugin)), lower, def);
    lower = def;
  }

  if (def > upper) {
    GST_WARNING ("%s has upper bound %f < default %f\n",
        slv2_value_as_string (slv2_plugin_get_uri (lv2plugin)), upper, def);
    upper = def;
  }

  if (slv2_port_has_property (lv2plugin, port, integer_prop))
    ret = g_param_spec_int (name, name, name, lower, upper, def, perms);
  else
    ret = g_param_spec_float (name, name, name, lower, upper, def, perms);

  g_free (name);

  return ret;
}

static void
gst_lv2_class_init (GstLV2Class * klass, SLV2Plugin lv2plugin)
{
  GObjectClass *gobject_class;
  GstSignalProcessorClass *gsp_class;
  gint i;

  GST_DEBUG ("class_init %p", klass);

  gobject_class = (GObjectClass *) klass;
  gobject_class->set_property = gst_lv2_set_property;
  gobject_class->get_property = gst_lv2_get_property;

  gsp_class = GST_SIGNAL_PROCESSOR_CLASS (klass);
  gsp_class->setup = gst_lv2_setup;
  gsp_class->start = gst_lv2_start;
  gsp_class->stop = gst_lv2_stop;
  gsp_class->cleanup = gst_lv2_cleanup;
  gsp_class->process = gst_lv2_process;

  klass->plugin = lv2plugin;

  /* register properties */

  for (i = 0; i < gsp_class->num_control_in; i++) {
    GParamSpec *p;

    p = gst_lv2_class_get_param_spec (klass, klass->control_in_portnums[i]);

    /* properties have an offset of 1 */
    g_object_class_install_property (G_OBJECT_CLASS (klass), i + 1, p);
  }

  for (i = 0; i < gsp_class->num_control_out; i++) {
    GParamSpec *p;

    p = gst_lv2_class_get_param_spec (klass, klass->control_out_portnums[i]);

    /* properties have an offset of 1, and we already added num_control_in */
    g_object_class_install_property (G_OBJECT_CLASS (klass),
        gsp_class->num_control_in + i + 1, p);
  }
}

static void
gst_lv2_init (GstLV2 * lv2, GstLV2Class * klass)
{
#if 0
  lv2->plugin = klass->plugin;
  lv2->instance = NULL;
  lv2->activated = FALSE;
  lv2->inplace_broken = LV2_IS_INPLACE_BROKEN (lv2->descriptor->Properties);
#endif
}

static void
gst_lv2_set_property (GObject * object, guint prop_id, const GValue * value,
    GParamSpec * pspec)
{
#if 0
  GstSignalProcessor *gsp;
  GstSignalProcessorClass *gsp_class;

  gsp = GST_SIGNAL_PROCESSOR (object);
  gsp_class = GST_SIGNAL_PROCESSOR_GET_CLASS (object);

  /* remember, properties have an offset of 1 */
  prop_id--;

  /* only input ports */
  g_return_if_fail (prop_id < gsp_class->num_control_in);

  /* now see what type it is */
  switch (pspec->value_type) {
    case G_TYPE_BOOLEAN:
      gsp->control_in[prop_id] = g_value_get_boolean (value) ? 1.f : 0.f;
      break;
    case G_TYPE_INT:
      gsp->control_in[prop_id] = g_value_get_int (value);
      break;
    case G_TYPE_FLOAT:
      gsp->control_in[prop_id] = g_value_get_float (value);
      break;
    default:
      g_assert_not_reached ();
  }
#endif
}

static void
gst_lv2_get_property (GObject * object, guint prop_id, GValue * value,
    GParamSpec * pspec)
{
#if 0
  GstSignalProcessor *gsp;
  GstSignalProcessorClass *gsp_class;
  gfloat *controls;

  gsp = GST_SIGNAL_PROCESSOR (object);
  gsp_class = GST_SIGNAL_PROCESSOR_GET_CLASS (object);

  /* remember, properties have an offset of 1 */
  prop_id--;

  if (prop_id < gsp_class->num_control_in) {
    controls = gsp->control_in;
  } else if (prop_id < gsp_class->num_control_in + gsp_class->num_control_out) {
    controls = gsp->control_out;
    prop_id -= gsp_class->num_control_in;
  } else {
    g_return_if_reached ();
  }

  /* now see what type it is */
  switch (pspec->value_type) {
    case G_TYPE_BOOLEAN:
      g_value_set_boolean (value, controls[prop_id] > 0.5);
      break;
    case G_TYPE_INT:
      g_value_set_int (value, CLAMP (controls[prop_id], G_MININT, G_MAXINT));
      break;
    case G_TYPE_FLOAT:
      g_value_set_float (value, controls[prop_id]);
      break;
    default:
      g_return_if_reached ();
  }
#endif
}

static gboolean
gst_lv2_setup (GstSignalProcessor * gsp, guint sample_rate)
{
#if 0
  GstLV2 *ladspa;
  GstLV2Class *oclass;
  GstSignalProcessorClass *gsp_class;
  LV2_Descriptor *desc;
  int i;

  gsp_class = GST_SIGNAL_PROCESSOR_GET_CLASS (gsp);
  ladspa = (GstLV2 *) gsp;
  oclass = (GstLV2Class *) gsp_class;
  desc = ladspa->descriptor;

  g_return_val_if_fail (ladspa->handle == NULL, FALSE);
  g_return_val_if_fail (ladspa->activated == FALSE, FALSE);

  GST_DEBUG_OBJECT (ladspa, "instantiating the plugin at %d Hz", sample_rate);

  ladspa->handle = desc->instantiate (desc, sample_rate);

  g_return_val_if_fail (ladspa->handle != NULL, FALSE);

  /* connect the control ports */
  for (i = 0; i < gsp_class->num_control_in; i++)
    desc->connect_port (ladspa->handle,
        oclass->control_in_portnums[i], &(gsp->control_in[i]));
  for (i = 0; i < gsp_class->num_control_out; i++)
    desc->connect_port (ladspa->handle,
        oclass->control_out_portnums[i], &(gsp->control_out[i]));
#endif
  return TRUE;
}

static gboolean
gst_lv2_start (GstSignalProcessor * gsp)
{
#if 0
  GstLV2 *ladspa;
  LV2_Descriptor *desc;

  ladspa = (GstLV2 *) gsp;
  desc = ladspa->descriptor;

  g_return_val_if_fail (ladspa->activated == FALSE, FALSE);
  g_return_val_if_fail (ladspa->handle != NULL, FALSE);

  GST_DEBUG_OBJECT (ladspa, "activating");

  if (desc->activate)
    desc->activate (ladspa->handle);

  ladspa->activated = TRUE;
#endif

  return TRUE;
}

static void
gst_lv2_stop (GstSignalProcessor * gsp)
{
#if 0
  GstLV2 *ladspa;
  LV2_Descriptor *desc;

  ladspa = (GstLV2 *) gsp;
  desc = ladspa->descriptor;

  g_return_if_fail (ladspa->activated == TRUE);
  g_return_if_fail (ladspa->handle != NULL);

  GST_DEBUG_OBJECT (ladspa, "deactivating");

  if (desc->activate)
    desc->activate (ladspa->handle);

  ladspa->activated = FALSE;
#endif
}

static void
gst_lv2_cleanup (GstSignalProcessor * gsp)
{
#if 0
  GstLV2 *ladspa;
  LV2_Descriptor *desc;

  ladspa = (GstLV2 *) gsp;
  desc = ladspa->descriptor;

  g_return_if_fail (ladspa->activated == FALSE);
  g_return_if_fail (ladspa->handle != NULL);

  GST_DEBUG_OBJECT (ladspa, "cleaning up");

  if (desc->cleanup)
    desc->cleanup (ladspa->handle);

  ladspa->handle = NULL;
#endif
}

static void
gst_lv2_process (GstSignalProcessor * gsp, guint nframes)
{
#if 0
  GstSignalProcessorClass *gsp_class;
  GstLV2 *ladspa;
  GstLV2Class *oclass;
  LV2_Descriptor *desc;
  guint i;

  gsp_class = GST_SIGNAL_PROCESSOR_GET_CLASS (gsp);
  ladspa = (GstLV2 *) gsp;
  oclass = (GstLV2Class *) gsp_class;
  desc = ladspa->descriptor;

  for (i = 0; i < gsp_class->num_audio_in; i++)
    desc->connect_port (ladspa->handle, oclass->audio_in_portnums[i],
        gsp->audio_in[i]);
  for (i = 0; i < gsp_class->num_audio_out; i++)
    desc->connect_port (ladspa->handle, oclass->audio_out_portnums[i],
        gsp->audio_out[i]);

  desc->run (ladspa->handle, nframes);
#endif
}

/* search the plugin path
 */
static gboolean
lv2_plugin_discover (void)
{
  unsigned i;
  SLV2Plugins plugins = slv2_world_get_all_plugins (world);
  for (i = 0; i < slv2_plugins_size (plugins); ++i) {
    SLV2Plugin lv2plugin = slv2_plugins_get_at (plugins, i);
    GTypeInfo typeinfo = {
      sizeof (GstLV2Class),
      (GBaseInitFunc) gst_lv2_base_init,
      NULL,
      (GClassInitFunc) gst_lv2_class_init,
      NULL,
      lv2plugin,
      sizeof (GstLV2),
      0,
      (GInstanceInitFunc) gst_lv2_init,
    };

    GType type;

    /* construct the type name from plugin URI */
    gchar *type_name = g_strdup_printf ("%s",
        slv2_value_as_uri (slv2_plugin_get_uri (lv2plugin)));
    g_strcanon (type_name, G_CSET_A_2_Z G_CSET_a_2_z G_CSET_DIGITS "-+", '-');

    /* if it's already registered, drop it */
    if (g_type_from_name (type_name))
      goto next;

    /* create the type */
    type =
        g_type_register_static (GST_TYPE_SIGNAL_PROCESSOR, type_name, &typeinfo,
        0);

    /* FIXME: not needed anymore when we can add pad templates, etc in class_init
     * as class_data contains the LADSPA_Descriptor too */
    g_type_set_qdata (type, GST_SLV2_PLUGIN_QDATA, (gpointer) lv2plugin);

    if (!gst_element_register (gst_lv2_plugin, type_name, GST_RANK_NONE, type))
      goto next;

  next:
    g_free (type_name);
  }
  return TRUE;
}

static gboolean
plugin_init (GstPlugin * plugin)
{
  GST_DEBUG_CATEGORY_INIT (lv2_debug, "lv2",
      GST_DEBUG_FG_GREEN | GST_DEBUG_BG_BLACK | GST_DEBUG_BOLD, "LV2");

  world = slv2_world_new ();
  slv2_world_load_all (world);

  audio_class = slv2_value_new_uri (world, SLV2_PORT_CLASS_AUDIO);
  control_class = slv2_value_new_uri (world, SLV2_PORT_CLASS_CONTROL);
  input_class = slv2_value_new_uri (world, SLV2_PORT_CLASS_INPUT);
  output_class = slv2_value_new_uri (world, SLV2_PORT_CLASS_OUTPUT);
  integer_prop =
      slv2_value_new_uri (world, "http://lv2plug.in/ns/lv2core#integer");
  toggled_prop =
      slv2_value_new_uri (world, "http://lv2plug.in/ns/lv2core#toggled");

  parent_class = g_type_class_ref (GST_TYPE_SIGNAL_PROCESSOR);

  gst_lv2_plugin = plugin;

  return lv2_plugin_discover ();
}

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    "lv2",
    "All LV2 plugins",
    plugin_init, VERSION, GST_LICENSE, GST_PACKAGE_NAME, GST_PACKAGE_ORIGIN)
