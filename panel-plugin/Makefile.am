plugindir = $(libdir)/xfce4/panel/plugins
plugin_LTLIBRARIES = libappletweather.la

libappletweather_la_CFLAGS =						\
	-DPACKAGE_LOCALE_DIR=\"$(localedir)\"			\
	@LIBXFCE4PANEL_CFLAGS@					\
	@LIBXFCE4UI_CFLAGS@ -g

libappletweather_LDFLAGS = 						\
	-avoid-version 						\
	-module 						\
	-no-undefined 						\
	-export-symbols-regex '^xfce_panel_module_(preinit|init|construct)'

libappletweather_la_LIBADD =						\
	@LIBXFCE4PANEL_LIBS@					\
	@LIBXFCE4UI_LIBS@

libappletweather_la_SOURCES =		\
	main.c

desktopdir = $(datadir)/xfce4/panel/plugins
desktop_DATA = applet-weather.desktop

EXTRA_DIST = 								\
	applet-weather.desktop.in

DISTCLEANFILES =							\
	$(desktop_DATA)

%.desktop: %.desktop.in
	cp $< $@
