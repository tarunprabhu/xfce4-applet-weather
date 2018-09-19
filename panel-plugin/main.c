/*
 *  Generic Monitor plugin for the Xfce4 panel
 *  Main file for the Battmon plugin
 *  Copyright (c) 2004 Roger Seguin <roger_seguin@msn.com>
 *                                  <http://rmlx.dyndns.org>
 *  Copyright (c) 2006 Julien Devemy <jujucece@gmail.com>
 *  Copyright (c) 2012 John Lindgren <john.lindgren@aol.com>
 *  Copyright (c) 2017 Tarun Prabhu <tarun.prabhu@gmail.com>
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.

 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.

 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <gtk/gtk.h>

#include <libxfce4panel/xfce-panel-convenience.h>
#include <libxfce4panel/xfce-panel-plugin.h>
#include <libxfce4ui/libxfce4ui.h>
#include <libxfce4util/libxfce4util.h>

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define PLUGIN_NAME "Battmon"
#define BORDER 2

typedef struct gui_t {
    /* Configuration GUI widgets */
    GtkWidget      *wSc_Period;
    GtkWidget      *wPB_Font;
} gui_t;

typedef struct param_t {
  /* Configurable parameters */
  uint32_t iPeriod_ms;
  char *acFont;
} param_t;

typedef struct conf_t {
  GtkWidget *wTopLevel;
  struct gui_t oGUI; /* Configuration/option dialog */
  struct param_t oParam;
} conf_t;

typedef struct monitor_t {
  /* Plugin monitor */
  GtkWidget *wEventBox;
  GtkWidget *wBox;
  GtkWidget *wImgBox;
  GtkWidget *wValue;
  GtkWidget *wImage;
} monitor_t;

typedef struct battmon_t {
  XfcePanelPlugin *plugin;
  unsigned int iTimerId; /* Cyclic update */
  struct conf_t oConf;
  struct monitor_t oMonitor;
} battmon_t;

typedef enum battstatus_t {
  BattStatus_NoBatt,
  BattStatus_Full,
  BattStatus_Charging,
  BattStatus_Discharging,
  BattStatus_Unknown
} battstatus_t;

typedef enum battlevel_t {
  BattLevel_Full,
  BattLevel_OK,
  BattLevel_Low,
  BattLevel_Critical,
  BattLevel_Unknown,
} battlevel_t;

static battlevel_t GetBatteryLevel(int percent) {
  if(percent > 100)
    percent = 100;
  else if(percent < 0)
    percent = 0;

  if(percent > 90)
    return BattLevel_Full;
  else if(percent > 45)
    return BattLevel_OK;
  else if(percent > 15)
    return BattLevel_Low;
  else
    return BattLevel_Critical;
}

static int GetBatteryPercent() {
  const char* file = "/sys/class/power_supply/BAT0/capacity";
  FILE* fp = NULL;
  int percent = 0;
  
  if(fp = fopen(file, "r")) {
    fscanf(fp, "%d", &percent);
    fclose(fp);
    return percent;
  }

  return percent;
}

static battstatus_t GetBatteryStatus() {
  const char* file = "/sys/class/power_supply/BAT0/status";
  char status[256];
  FILE* fp = NULL;
  
  if(fp = fopen(file, "r")) {
    fgets(status, 256, fp);
    fclose(fp);
    if(strcmp(status, "Full\n") == 0)
      return BattStatus_Full;
    else if(strcmp(status, "Charging\n") == 0)
      return BattStatus_Charging;
    else if(strcmp(status, "Discharging\n") == 0)
      return BattStatus_Discharging;
    return BattStatus_Unknown;
  }
  
  return BattStatus_NoBatt;
}

static long ReadFile(const char* filename) {
  long val = -1;
  FILE* fp = NULL;
  if(fp = fopen(filename, "r")) {
    fscanf(fp, "%ld", &val);
    fclose(fp);
  }
  return val;
}

static long ReadFileAlt(const char* filename1, const char* filename2) {
  long val = -1;
  val = ReadFile(filename1);
  if(val == -1)
    val = ReadFile(filename2);
  return val;
}

static int GetBatteryTime(battstatus_t status, int* hrs, int* mins) {
  const char* fileCharge     = "/sys/class/power_supply/BAT0/charge_now";
  const char* fileChargeFull = "/sys/class/power_supply/BAT0/charge_full";
  const char* fileEnergy     = "/sys/class/power_supply/BAT0/energy_now";
  const char* fileEnergyFull = "/sys/class/power_supply/BAT0/energy_full";
  const char* fileCurrent    = "/sys/class/power_supply/BAT0/current_now";
  const char* filePower      = "/sys/class/power_supply/BAT0/power_now";
  long total = 0, now = 1, full = 0;
  double ratio = 0.0;
  int ret = 0;

  total = ReadFileAlt(fileCharge, fileEnergy);
  now = ReadFileAlt(fileCurrent, filePower);
  if(status == BattStatus_Charging) {
    full = ReadFileAlt(fileChargeFull, fileEnergyFull);
    if(total != -1 && now != -1 && full != -1 && now > 0) {
      ret = 1;
      ratio = (float)(full - total) / (float) now;
    }
  } else if(status == BattStatus_Discharging) {
    if(total != -1 && now != -1 && now > 0) {
      ret = 1;
      ratio = (float) total / (float) now;
    }
  }
  
  if(ret) {
    *hrs = floor(ratio);
    *mins = floor((ratio - *hrs) * 60);
  }

  return ret;
}

/**************************************************************/
static int DisplayBatteryLevel(struct battmon_t *p_poPlugin)
/* Launch the command, get its output and display it in the panel-docked
   text field */
{
  struct param_t *poConf = &(p_poPlugin->oConf.oParam);
  struct monitor_t *poMonitor = &(p_poPlugin->oMonitor);
  const char* icon = NULL;
  int percent = 0, hrs = -1, mins = -1;
  battstatus_t status = BattStatus_NoBatt;
  char text[5] = "----";
  char class[8];
  
  percent = GetBatteryPercent();
  status = GetBatteryStatus();
  switch(status) {
  case BattStatus_Full:
    icon = "battery-full-charging";
    break;
  case BattStatus_Charging:
    switch(GetBatteryLevel(percent)) {
    case BattLevel_Full:
      icon = "battery-full-charging";
      break;
    case BattLevel_OK:
      icon = "battery-good-charging";
      break;
    case BattLevel_Low:
    case BattLevel_Critical:
      icon = "battery-low-charging";
      break;
    default:
      /* Should never get here */
      break;
    }
    break;
  case BattStatus_Discharging:
    switch(GetBatteryLevel(percent)) {
    case BattLevel_Full:
      icon = "battery-full-charged";
      break;
    case BattLevel_OK:
      icon = "battery-good";
      break;
    case BattLevel_Low:
      icon = "battery-low";
      break;
    case BattLevel_Critical:
      icon = "battery-caution";
      break;
    default:
      /* Should never get here */
      break;
    }
    break;
  case BattStatus_Unknown:
    icon = "battery-full-charged";
    break;
  case BattStatus_NoBatt:
    icon = "battery-missing";
    break;
  default:
    /* Should never get here */
    break;
  }

  
  if(GetBatteryTime(status, &hrs, &mins)) {
    switch(status) {
    case BattStatus_Discharging:
      snprintf(class, sizeof(class), "p%d", percent/5);
      break;
    case BattStatus_Charging:
      snprintf(class, sizeof(class), "%s", "pblue");
      break;
    default:
      snprintf(class, sizeof(class), "%s", "pgray");
      break;
    }
    snprintf(text, sizeof(text), "%1d:%02d", hrs, mins);
  } else {
    switch(status) {
    case BattStatus_Charging:
      snprintf(class, sizeof(class), "%s", "pblue");
      break;
    default:
      snprintf(class, sizeof(class), "%s", "pgray");
      break;
    }
  }

  gtk_widget_set_name(poMonitor->wValue, class);
  gtk_label_set_text(GTK_LABEL(poMonitor->wValue), text);
  gtk_image_set_from_icon_name(GTK_IMAGE(poMonitor->wImage), icon,
                               GTK_ICON_SIZE_LARGE_TOOLBAR);

  gtk_widget_show(poMonitor->wImage);
  gtk_widget_show(poMonitor->wValue);

  return (0);

} /* DisplayBatteryLevel() */

/**************************************************************/

static gboolean SetTimer(void *p_pvPlugin)
/* Recurrently update the panel-docked monitor through a timer */
/* Warning : should not be called directly (except the 1st time) */
/* To avoid multiple timers */
{
  struct battmon_t *poPlugin = (battmon_t *)p_pvPlugin;
  struct param_t *poConf = &(poPlugin->oConf.oParam);

  DisplayBatteryLevel(poPlugin);

  if (poPlugin->iTimerId == 0) {
    poPlugin->iTimerId =
        g_timeout_add(poConf->iPeriod_ms, (GSourceFunc)SetTimer, poPlugin);
    return FALSE;
  }

  return TRUE;
} /* SetTimer() */


static battmon_t *battmon_create_control(XfcePanelPlugin *plugin)
/* Plugin API */
/* Create the plugin */
{
  struct battmon_t *poPlugin;
  struct param_t *poConf;
  struct monitor_t *poMonitor;
  GtkOrientation orientation = xfce_panel_plugin_get_orientation(plugin);
  GtkSettings *settings;
  gchar *default_font;

#if GTK_CHECK_VERSION(3, 16, 0)
  GtkStyleContext *context;
  GtkCssProvider *css_provider;
  gchar *css;
#endif

  poPlugin = g_new(battmon_t, 1);
  memset(poPlugin, 0, sizeof(battmon_t));
  poConf = &(poPlugin->oConf.oParam);
  poMonitor = &(poPlugin->oMonitor);

  poPlugin->plugin = plugin;

  poConf->iPeriod_ms = 30 * 1000;
  poPlugin->iTimerId = 0;

  // PangoFontDescription needs a font and we can't use "(Default)" anymore.
  // Use GtkSettings to get the current default font and use that, or set
  // default to "Sans 10"
  settings = gtk_settings_get_default();
  if (g_object_class_find_property(G_OBJECT_GET_CLASS(settings),
                                   "gtk-font-name")) {
    g_object_get(settings, "gtk-font-name", &default_font, NULL);
    poConf->acFont = g_strdup(default_font);
  } else
    poConf->acFont = g_strdup("Sans 9.8");

  poMonitor->wEventBox = gtk_event_box_new();
  gtk_event_box_set_visible_window(GTK_EVENT_BOX(poMonitor->wEventBox), FALSE);
  gtk_widget_show(poMonitor->wEventBox);

  xfce_panel_plugin_add_action_widget(plugin, poMonitor->wEventBox);

  poMonitor->wBox = gtk_box_new(orientation, 0);
#if GTK_CHECK_VERSION(3, 16, 0)
  context = gtk_widget_get_style_context(poMonitor->wBox);
  gtk_style_context_add_class(context, "battmon_plugin");
#endif
  gtk_widget_show(poMonitor->wBox);
  gtk_container_set_border_width(GTK_CONTAINER(poMonitor->wBox), 0);
  gtk_container_add(GTK_CONTAINER(poMonitor->wEventBox), poMonitor->wBox);

  /* Create a Box to put image and text */
  poMonitor->wImgBox = gtk_box_new(orientation, 0);
#if GTK_CHECK_VERSION(3, 16, 0)
  context = gtk_widget_get_style_context(poMonitor->wImgBox);
  gtk_style_context_add_class(context, "battmon_imagebox");
#endif
  gtk_widget_show(poMonitor->wImgBox);
  gtk_container_set_border_width(GTK_CONTAINER(poMonitor->wImgBox), 0);
  gtk_container_add(GTK_CONTAINER(poMonitor->wBox), poMonitor->wImgBox);

  /* Add Image */
  poMonitor->wImage = gtk_image_new();
#if GTK_CHECK_VERSION(3, 16, 0)
  context = gtk_widget_get_style_context(poMonitor->wImage);
  gtk_style_context_add_class(context, "battmon_image");
#endif
  gtk_box_pack_start(GTK_BOX(poMonitor->wImgBox), GTK_WIDGET(poMonitor->wImage),
                     TRUE, FALSE, 0);

  /* Add Value */
  poMonitor->wValue = gtk_label_new("");
#if GTK_CHECK_VERSION(3, 16, 0)
  context = gtk_widget_get_style_context(poMonitor->wValue);
  gtk_style_context_add_class(context, "battmon_value");
#endif
  gtk_widget_show(poMonitor->wValue);
  gtk_box_pack_start(GTK_BOX(poMonitor->wImgBox), GTK_WIDGET(poMonitor->wValue),
                     TRUE, FALSE, 0);

/* make widget padding consistent */
#if GTK_CHECK_VERSION(3, 16, 0)
#if GTK_CHECK_VERSION(3, 20, 0)
  css = g_strdup_printf("\
            label { background-color: #0000FF; padding: 4px; } \
            label#p0 { background-color: #FF0000; } \
            label#p1 { background-color: #FF1A00; } \
            label#p2 { background-color: #FF3500; } \
            label#p3 { background-color: #FF5000; } \
            label#p4 { background-color: #FF6B00; } \
            label#p5 { background-color: #FF8600; } \
            label#p6 { background-color: #FFA100; } \
            label#p7 { background-color: #FFBB00; } \
            label#p8 { background-color: #FFD600; } \
            label#p9 { background-color: #FFF100; } \
            label#p10 { background-color: #F1FF00; } \
            label#p11 { background-color: #D6FF00; } \
            label#p12 { background-color: #BBFF00; } \
            label#p13 { background-color: #A1FF00; } \
            label#p14 { background-color: #86FF00; } \
            label#p15 { background-color: #6BFF00; } \
            label#p16 { background-color: #50FF00; } \
            label#p17 { background-color: #35FF00; } \
            label#p18 { background-color: #1AFF00; } \
            label#p19 { background-color: #00FF00; } \
            label#p20 { background-color: #00FF00; } \
            label#pblue { background-color: skyblue; } \
            label#pgray { background-color: darkgray; } \
            progressbar.horizontal trough { min-height: 6px; }\
            progressbar.horizontal progress { min-height: 6px; }\
            progressbar.vertical trough { min-width: 6px; }\
            progressbar.vertical progress { min-width: 6px; }");
#else
  css = g_strdup_printf("\
            label { background-color: #0000FF; padding: 4px; } \
            label#p0 { background-color: #FF0000; } \
            label#p1 { background-color: #FF1A00; } \
            label#p2 { background-color: #FF3500; } \
            label#p3 { background-color: #FF5000; } \
            label#p4 { background-color: #FF6B00; } \
            label#p5 { background-color: #FF8600; } \
            label#p6 { background-color: #FFA100; } \
            label#p7 { background-color: #FFBB00; } \
            label#p8 { background-color: #FFD600; } \
            label#p9 { background-color: #FFF100; } \
            label#p10 { background-color: #F1FF00; } \
            label#p11 { background-color: #D6FF00; } \
            label#p12 { background-color: #BBFF00; } \
            label#p13 { background-color: #A1FF00; } \
            label#p14 { background-color: #86FF00; } \
            label#p15 { background-color: #6BFF00; } \
            label#p16 { background-color: #50FF00; } \
            label#p17 { background-color: #35FF00; } \
            label#p18 { background-color: #1AFF00; } \
            label#p19 { background-color: #00FF00; } \
            label#p20 { background-color: #00FF00; } \
            label#pblue { background-color: skyblue; } \
            label#pgray { background-color: darkgray; } \
            .progressbar.horizontal trough { min-height: 6px; }\
            .progressbar.horizontal progress { min-height: 6px; }\
            .progressbar.vertical trough { min-width: 6px; }\
            .progressbar.vertical progress { min-width: 6px; }");
#endif

  css_provider = gtk_css_provider_new();
  gtk_css_provider_load_from_data(css_provider, css, strlen(css), NULL);
  gtk_style_context_add_provider(GTK_STYLE_CONTEXT(gtk_widget_get_style_context(
                                     GTK_WIDGET(poMonitor->wImage))),
                                 GTK_STYLE_PROVIDER(css_provider),
                                 GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  gtk_style_context_add_provider(GTK_STYLE_CONTEXT(gtk_widget_get_style_context(
                                     GTK_WIDGET(poMonitor->wValue))),
                                 GTK_STYLE_PROVIDER(css_provider),
                                 GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

  g_free(css);
#endif

  g_free(default_font);

  return poPlugin;
} /* battmon_create_control() */


static void battmon_free(XfcePanelPlugin *plugin, battmon_t *poPlugin)
/* Plugin API */
{
  TRACE("battmon_free()\n");

  if (poPlugin->iTimerId)
    g_source_remove(poPlugin->iTimerId);

  g_free(poPlugin->oConf.oParam.acFont);
  g_free(poPlugin);
} /* battmon_free() */


static int SetMonitorFont(void *p_pvPlugin) {
  struct battmon_t *poPlugin = (battmon_t *)p_pvPlugin;
  struct monitor_t *poMonitor = &(poPlugin->oMonitor);
  struct param_t *poConf = &(poPlugin->oConf.oParam);

#if GTK_CHECK_VERSION(3, 16, 0)
  GtkCssProvider *css_provider;
  gchar *css;
#if GTK_CHECK_VERSION(3, 20, 0)
  PangoFontDescription *font;
  font = pango_font_description_from_string(poConf->acFont);
  if (G_LIKELY(font)) {
    css = g_strdup_printf(
        "label { font-family: %s; font-size: %dpx; font-style: %s; "
        "font-weight: %s }",
        pango_font_description_get_family(font),
        pango_font_description_get_size(font) / PANGO_SCALE,
        (pango_font_description_get_style(font) == PANGO_STYLE_ITALIC ||
         pango_font_description_get_style(font) == PANGO_STYLE_OBLIQUE)
            ? "italic"
            : "normal",
        (pango_font_description_get_weight(font) >= PANGO_WEIGHT_BOLD)
            ? "bold"
            : "normal");
    pango_font_description_free(font);
  } else
    css = g_strdup_printf("label { font: %s; }",
#else
  css = g_strdup_printf(".label { font: %s; }",
#endif
                          poConf->acFont);
  /* Setup Gtk style */
  DBG("css: %s", css);

  css_provider = gtk_css_provider_new();
  gtk_css_provider_load_from_data(css_provider, css, strlen(css), NULL);
  gtk_style_context_add_provider(GTK_STYLE_CONTEXT(gtk_widget_get_style_context(
                                     GTK_WIDGET(poMonitor->wValue))),
                                 GTK_STYLE_PROVIDER(css_provider),
                                 GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_free(css);
#else

  PangoFontDescription *poFont;

  if (!strcmp(poConf->acFont, "(default)")) /* Default font */
    return (1);
  poFont = pango_font_description_from_string(poConf->acFont);
  if (!poFont)
    return (-1);

  gtk_widget_override_font(poMonitor->wValue, poFont);
  gtk_widget_override_font(poMonitor->wValButton, poFont);

  pango_font_description_free(poFont);

#endif

  return (0);
} /* SetMonitorFont() */

      
static void battmon_read_config(XfcePanelPlugin *plugin, battmon_t *poPlugin)
/* Plugin API */
/* Executed when the panel is started - Read the configuration
   previously stored in xml file */
{
  struct param_t *poConf = &(poPlugin->oConf.oParam);
  struct monitor_t *poMonitor = &(poPlugin->oMonitor);
  const char *pc;
  char *file;
  XfceRc *rc;

  if (!(file = xfce_panel_plugin_lookup_rc_file(plugin)))
    return;

  rc = xfce_rc_simple_open(file, TRUE);
  g_free(file);

  if (!rc)
    return;

  poConf->iPeriod_ms = xfce_rc_read_int_entry(rc, "UpdatePeriod", 30 * 1000);

  if ((pc = xfce_rc_read_entry(rc, "Font", NULL))) {
    g_free(poConf->acFont);
    poConf->acFont = g_strdup(pc);
  }

  xfce_rc_close(rc);
}


static void battmon_write_config(XfcePanelPlugin *plugin, battmon_t *poPlugin) {
  struct param_t *poConf = &(poPlugin->oConf.oParam);
  XfceRc *rc;
  char *file;

  if (!(file = xfce_panel_plugin_save_location(plugin, TRUE)))
    return;

  rc = xfce_rc_simple_open(file, FALSE);
  g_free(file);

  if (!rc)
    return;

  TRACE("battmon_write_config()\n");

  xfce_rc_write_int_entry(rc, "Update Period", poConf->iPeriod_ms);

  xfce_rc_write_entry(rc, "Font", poConf->acFont);

  xfce_rc_close(rc);
}

static void SetPeriod(GtkWidget *p_wSc, void *p_pvPlugin) {
  struct battmon_t *poPlugin = (battmon_t *)p_pvPlugin;
  struct param_t *poConf = &(poPlugin->oConf.oParam);
  float r;

  TRACE("SetPeriod()\n");
  r = gtk_spin_button_get_value(GTK_SPIN_BUTTON(p_wSc));
  poConf->iPeriod_ms = (r * 1000);
}

static void UpdateConf(void *p_pvPlugin) {
  struct battmon_t *poPlugin = (battmon_t *)p_pvPlugin;
  struct conf_t *poConf = &(poPlugin->oConf);
  struct gui_t *poGUI = &(poConf->oGUI);

  TRACE("UpdateConf()\n");
  SetMonitorFont(poPlugin);
  /* Restart timer */
  if (poPlugin->iTimerId) {
    g_source_remove(poPlugin->iTimerId);
    poPlugin->iTimerId = 0;
  }
  SetTimer(p_pvPlugin);
}
 

static void About(XfcePanelPlugin *plugin) {
  GdkPixbuf *icon;

  const gchar *auth[] = {"Tarun Prabhu <tarun.prabhu@gmail.com>", NULL };

  icon = xfce_panel_pixbuf_from_source("battery", NULL, 32);
  gtk_show_about_dialog(
      NULL, "logo", icon, "license",
      xfce_get_license_text(XFCE_LICENSE_TEXT_GPL), "version", VERSION,
      "program-name", PACKAGE, "comments",
      _("Monitors the battery"),
      "website",
      "",
      "copyright",
      _("Copyright \xc2\xa9 2017 Tarun Prabhu\n"),
      "author", auth, NULL);

  if (icon)
    g_object_unref(G_OBJECT(icon));
}


static void ChooseFont(GtkWidget *p_wPB, void *p_pvPlugin) {
  struct battmon_t *poPlugin = (battmon_t *)p_pvPlugin;
  struct param_t *poConf = &(poPlugin->oConf.oParam);
  GtkWidget *wDialog;
  const char *pcFont;
  int iResponse;

  wDialog = gtk_font_chooser_dialog_new(
      _("Font Selection"), GTK_WINDOW(gtk_widget_get_toplevel(p_wPB)));
  gtk_window_set_transient_for(GTK_WINDOW(wDialog),
                               GTK_WINDOW(poPlugin->oConf.wTopLevel));
  if (strcmp(poConf->acFont, "(default)")) /* Default font */
    gtk_font_chooser_set_font(GTK_FONT_CHOOSER(wDialog), poConf->acFont);
  iResponse = gtk_dialog_run(GTK_DIALOG(wDialog));
  if (iResponse == GTK_RESPONSE_OK) {
    pcFont = gtk_font_chooser_get_font(GTK_FONT_CHOOSER(wDialog));
    if (pcFont) {
      g_free(poConf->acFont);
      poConf->acFont = g_strdup(pcFont);
      gtk_button_set_label(GTK_BUTTON(p_wPB), poConf->acFont);
    }
  }
  gtk_widget_destroy(wDialog);
}
 

static void battmon_dialog_response(GtkWidget *dlg, int response,
                                    battmon_t *battmon) {
  UpdateConf(battmon);
  gtk_widget_destroy(dlg);
  xfce_panel_plugin_unblock_menu(battmon->plugin);
  battmon_write_config(battmon->plugin, battmon);
  DisplayBatteryLevel(battmon);
}

 
static int battmon_CreateConfigGUI(GtkWidget *vbox1, struct gui_t *p_poGUI) {
  GtkWidget *table1;
  GtkWidget *eventbox1;
  GtkAdjustment *wSc_Period_adj;
  GtkWidget *wSc_Period;
  GtkWidget *label2;
  GtkWidget *hseparator10;
  GtkWidget *wPB_Font;
  GtkWidget *hbox4;

  table1 = gtk_grid_new();
  gtk_grid_set_column_spacing(GTK_GRID(table1), 2);
  gtk_grid_set_row_spacing(GTK_GRID(table1), 2);
  gtk_widget_show(table1);
  gtk_box_pack_start(GTK_BOX(vbox1), table1, FALSE, TRUE, 0);

  eventbox1 = gtk_event_box_new();
  gtk_widget_show(eventbox1);
  gtk_grid_attach(GTK_GRID(table1), eventbox1, 1, 2, 1, 1);
  gtk_widget_set_valign(GTK_WIDGET(eventbox1), GTK_ALIGN_CENTER);
  gtk_widget_set_halign(GTK_WIDGET(eventbox1), GTK_ALIGN_CENTER);
  gtk_widget_set_vexpand(GTK_WIDGET(eventbox1), TRUE);
  gtk_widget_set_hexpand(GTK_WIDGET(eventbox1), TRUE);

  wSc_Period_adj = gtk_adjustment_new(15, .25, 60 * 60 * 24, .25, 1, 0);
  wSc_Period = gtk_spin_button_new(GTK_ADJUSTMENT(wSc_Period_adj), .25, 2);
  gtk_widget_show(wSc_Period);
  gtk_container_add(GTK_CONTAINER(eventbox1), wSc_Period);
  gtk_widget_set_tooltip_text(wSc_Period,
                              "Interval between 2 consecutive spawns");
  gtk_spin_button_set_numeric(GTK_SPIN_BUTTON(wSc_Period), TRUE);

  label2 = gtk_label_new(_("Period (s) "));
  gtk_widget_show(label2);
  gtk_grid_attach(GTK_GRID(table1), label2, 0, 2, 1, 1);
  gtk_label_set_justify(GTK_LABEL(label2), GTK_JUSTIFY_LEFT);
  gtk_widget_set_valign(label2, GTK_ALIGN_CENTER);

  hseparator10 = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
  gtk_widget_show(hseparator10);
  gtk_box_pack_start(GTK_BOX(vbox1), hseparator10, FALSE, FALSE, 0);

  wPB_Font = gtk_button_new_with_label(_("Select the display font..."));
  gtk_widget_show(wPB_Font);
  gtk_box_pack_start(GTK_BOX(vbox1), wPB_Font, TRUE, TRUE, 0);
  gtk_widget_set_tooltip_text(wPB_Font, "Press to change font...");

  hbox4 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
  gtk_widget_show(hbox4);
  gtk_container_add(GTK_CONTAINER(vbox1), hbox4);

  p_poGUI->wSc_Period = wSc_Period;
  p_poGUI->wPB_Font = wPB_Font;

  return 0;
}

 
static void battmon_create_options(XfcePanelPlugin *plugin,
                                   battmon_t *poPlugin) {
  GtkWidget *dlg, *vbox;
  struct param_t *poConf = &(poPlugin->oConf.oParam);
  struct gui_t *poGUI = &(poPlugin->oConf.oGUI);

  TRACE("battmon_create_options()\n");

  xfce_panel_plugin_block_menu(plugin);

  dlg = xfce_titled_dialog_new_with_buttons(
      _("Weather Monitor Configuration"),
      GTK_WINDOW(gtk_widget_get_toplevel(GTK_WIDGET(plugin))),
      GTK_DIALOG_DESTROY_WITH_PARENT, "gtk-close", GTK_RESPONSE_OK, NULL);

  g_signal_connect(dlg, "response", G_CALLBACK(battmon_dialog_response),
                   poPlugin);

  vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, BORDER + 6);
  gtk_container_set_border_width(GTK_CONTAINER(vbox), BORDER + 4);
  gtk_widget_show(vbox);
  gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dlg))),
                     vbox, TRUE, TRUE, 0);

  poPlugin->oConf.wTopLevel = dlg;

  (void)battmon_CreateConfigGUI(GTK_WIDGET(vbox), poGUI);

  gtk_spin_button_set_value(GTK_SPIN_BUTTON(poGUI->wSc_Period),
                            ((double)poConf->iPeriod_ms / 1000));
  g_signal_connect(GTK_WIDGET(poGUI->wSc_Period), "value_changed",
                   G_CALLBACK(SetPeriod), poPlugin);

  if (strcmp(poConf->acFont, "(default)"))
    gtk_button_set_label(GTK_BUTTON(poGUI->wPB_Font), poConf->acFont);
  g_signal_connect(G_OBJECT(poGUI->wPB_Font), "clicked", G_CALLBACK(ChooseFont),
                   poPlugin);

  gtk_widget_show(dlg);
}

 
static void battmon_set_orientation(XfcePanelPlugin *plugin,
                                    GtkOrientation p_iOrientation,
                                    battmon_t *poPlugin) {
  struct monitor_t *poMonitor = &(poPlugin->oMonitor);

  gtk_orientable_set_orientation(GTK_ORIENTABLE(poMonitor->wBox),
                                 p_iOrientation);
  gtk_orientable_set_orientation(GTK_ORIENTABLE(poMonitor->wImgBox),
                                 p_iOrientation);
  SetMonitorFont(poPlugin);
}


static gboolean battmon_remote_event(XfcePanelPlugin *plugin, const gchar *name,
                                     const GValue *value, battmon_t *battmon) {
  g_return_val_if_fail(value == NULL || G_IS_VALUE(value), FALSE);
  if (strcmp(name, "refresh") == 0) {
    if (value != NULL && G_VALUE_HOLDS_BOOLEAN(value) &&
        g_value_get_boolean(value)) {
      /* update the display */
      DisplayBatteryLevel(battmon);
    }
    return TRUE;
  }
  return FALSE;
}


static void battmon_construct(XfcePanelPlugin *plugin) {
  battmon_t *battmon;
  
  battmon = battmon_create_control(plugin);

  battmon_read_config(plugin, battmon);

  gtk_container_add(GTK_CONTAINER(plugin), battmon->oMonitor.wEventBox);

  SetMonitorFont(battmon);
  SetTimer(battmon);

  g_signal_connect(plugin, "free-data", G_CALLBACK(battmon_free), battmon);

  g_signal_connect(plugin, "save", G_CALLBACK(battmon_write_config), battmon);

  g_signal_connect(plugin, "orientation-changed",
                   G_CALLBACK(battmon_set_orientation), battmon);

  xfce_panel_plugin_menu_show_about(plugin);

  g_signal_connect(plugin, "about", G_CALLBACK(About), plugin);

  xfce_panel_plugin_menu_show_configure(plugin);
  g_signal_connect(plugin, "configure-plugin",
                   G_CALLBACK(battmon_create_options), battmon);

  g_signal_connect(plugin, "remote-event", G_CALLBACK(battmon_remote_event),
                   battmon);

}

XFCE_PANEL_PLUGIN_REGISTER(battmon_construct)
