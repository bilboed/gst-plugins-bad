plugin_LTLIBRARIES = libgstvideomeasure.la 

noinst_HEADERS = gstvideomeasure_ssim.h gstvideomeasure_collector.h

libgstvideomeasure_la_SOURCES = \
    gstvideomeasure.c \
    gstvideomeasure_ssim.c \
    gstvideomeasure_collector.c

libgstvideomeasure_la_CFLAGS = $(GST_CFLAGS) $(GST_BASE_CFLAGS) \
    $(GST_PLUGINS_BASE_CFLAGS)
libgstvideomeasure_la_LIBADD = $(GST_PLUGINS_BASE_LIBS) \
    -lgstvideo-@GST_MAJORMINOR@ $(GST_BASE_LIBS) $(GST_LIBS)
libgstvideomeasure_la_LDFLAGS = $(GST_PLUGIN_LDFLAGS)
libgstvideomeasure_la_LIBTOOLFLAGS = --tag=disable-static

