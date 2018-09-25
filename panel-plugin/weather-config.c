/*  Copyright (c) 2003-2014 Xfce Development Team
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <string.h>
#include <libxfce4ui/libxfce4ui.h>

#include "weather-parsers.h"
#include "weather-data.h"
#include "weather.h"
#include "weather-debug.h"
#include "weather-config.h"
#include "weather-search.h"

#define UPDATE_TIMER_DELAY 7
#define OPTIONS_N 15
#define BORDER 4
#define LOC_NAME_MAX_LEN 50
#define TIMEZONE_MAX_LEN 40

#define ADD_PAGE(homogenous)                                        \
    palign = gtk_alignment_new(0.5, 0.5, 1, 1);                     \
    gtk_container_set_border_width(GTK_CONTAINER(palign), BORDER);  \
    page = gtk_vbox_new(homogenous, BORDER);                        \
    gtk_container_add(GTK_CONTAINER(palign), page);

#define ADD_LABEL(text, sg)                                         \
    label = gtk_label_new_with_mnemonic(text);                      \
    gtk_misc_set_alignment(GTK_MISC(label), 0, 0.5);                \
    if (G_LIKELY(sg))                                               \
        gtk_size_group_add_widget(sg, label);                       \
    gtk_box_pack_start(GTK_BOX(hbox), label, FALSE, FALSE, BORDER);

#define ADD_SPIN(spin, min, max, step, val, digits, sg)                 \
    spin = gtk_spin_button_new_with_range(min, max, step);              \
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin), val);              \
    gtk_spin_button_set_digits(GTK_SPIN_BUTTON(spin), digits);          \
    if (G_LIKELY(sg))                                                   \
        gtk_size_group_add_widget(sg, spin);                            \
    gtk_label_set_mnemonic_widget(GTK_LABEL(label), GTK_WIDGET(spin));  \
    gtk_box_pack_start(GTK_BOX(hbox), spin, FALSE, FALSE, 0);

#define ADD_COMBO(combo)                                                \
    combo = gtk_combo_box_new_text();                                   \
    gtk_label_set_mnemonic_widget(GTK_LABEL(label), GTK_WIDGET(combo)); \
    gtk_box_pack_start(GTK_BOX(hbox), combo, TRUE, TRUE, 0);

#define ADD_COMBO_VALUE(combo, text)                        \
    gtk_combo_box_append_text(GTK_COMBO_BOX(combo), text);

#define SET_COMBO_VALUE(combo, val)                         \
    gtk_combo_box_set_active(GTK_COMBO_BOX(combo), val);

#define SET_TOOLTIP(widget, markup)                             \
    gtk_widget_set_tooltip_markup(GTK_WIDGET(widget), markup);

#define TEXT_UNKNOWN(text) (text ? text : "-")

static void
spin_alt_value_changed(const GtkWidget *spin,
                       gpointer user_data);


static void
setup_units(xfceweather_dialog *dialog,
            const units_config *units);


static void
update_summary_window(xfceweather_dialog *dialog,
                      gboolean restore_position)
{
    if (dialog->pd->summary_window) {
        gint x, y;

        /* remember position */
        if (restore_position)
            gtk_window_get_position(GTK_WINDOW(dialog->pd->summary_window),
                                    &x, &y);

        /* call toggle function two times to close and open dialog */
        forecast_click(dialog->pd->summary_window, dialog->pd);
        forecast_click(dialog->pd->summary_window, dialog->pd);

        /* ask wm to restore position of new window */
        if (restore_position)
            gtk_window_move(GTK_WINDOW(dialog->pd->summary_window), x, y);

        /* bring config dialog to the front, it might have been hidden
         * beneath the summary window */
        gtk_window_present(GTK_WINDOW(dialog->dialog));
    }
}


static gboolean
schedule_data_update(gpointer user_data)
{
    xfceweather_dialog *dialog = (xfceweather_dialog *) user_data;
    plugin_data *pd = dialog->pd;

    /* force update of downloaded data */
    weather_debug("Delayed update timer expired, now scheduling data update.");
    update_weatherdata_with_reset(pd);

    if (dialog->update_spinner && GTK_IS_SPINNER(dialog->update_spinner)) {
        gtk_spinner_stop(GTK_SPINNER(dialog->update_spinner));
        gtk_widget_hide(GTK_WIDGET(dialog->update_spinner));
    }
    return FALSE;
}


static void
schedule_delayed_data_update(xfceweather_dialog *dialog)
{
    GSource *source;

    weather_debug("Starting delayed data update.");
    /* cancel any update that was scheduled before */
    if (dialog->timer_id) {
        source = g_main_context_find_source_by_id(NULL, dialog->timer_id);
        if (source) {
            g_source_destroy(source);
            dialog->timer_id = 0;
        }
    }

    /* stop any updates that could be performed by weather.c */
    if (dialog->pd->update_timer) {
        source = g_main_context_find_source_by_id(NULL, dialog->pd->update_timer);
        if (source) {
            g_source_destroy(source);
            dialog->pd->update_timer = 0;
        }
    }

    gtk_widget_show(GTK_WIDGET(dialog->update_spinner));
    gtk_spinner_start(GTK_SPINNER(dialog->update_spinner));
    dialog->timer_id =
        g_timeout_add_seconds(UPDATE_TIMER_DELAY,
                              (GSourceFunc) schedule_data_update, dialog);
}


static gchar *
sanitize_location_name(const gchar *location_name)
{
    gchar *pos, *pos2, sane[LOC_NAME_MAX_LEN * 4];
    glong len, offset;

    pos = g_utf8_strchr(location_name, -1, ',');
    if (pos != NULL) {
        pos2 = pos;
        while ((pos2 = g_utf8_next_char(pos2)))
            if (g_utf8_get_char(pos2) == ',') {
                pos = pos2;
                break;
            }
        offset = g_utf8_pointer_to_offset(location_name, pos);
        if (offset > LOC_NAME_MAX_LEN)
            offset = LOC_NAME_MAX_LEN;
        (void) g_utf8_strncpy(sane, location_name, offset);
        sane[LOC_NAME_MAX_LEN * 4 - 1] = '\0';
        return g_strdup(sane);
    } else {
        len = g_utf8_strlen(location_name, LOC_NAME_MAX_LEN);

        if (len >= LOC_NAME_MAX_LEN) {
            (void) g_utf8_strncpy(sane, location_name, len);
            sane[LOC_NAME_MAX_LEN * 4 - 1] = '\0';
            return g_strdup(sane);
        }

        if (len > 0)
            return g_strdup(location_name);

        return g_strdup(_("Unset"));
    }
}


static void
cb_lookup_altitude(SoupSession *session,
                   SoupMessage *msg,
                   gpointer user_data)
{
    xfceweather_dialog *dialog = (xfceweather_dialog *) user_data;
    xml_altitude *altitude;
    gdouble alt;

    altitude = (xml_altitude *)
        parse_xml_document(msg, (XmlParseFunc) parse_altitude);

    if (altitude) {
        alt = string_to_double(altitude->altitude, -9999);
        xml_altitude_free(altitude);
    }
    weather_debug("Altitude returned by GeoNames: %.0f meters", alt);
    if (alt < -420)
        alt = 0;
    else if (dialog->pd->units->altitude == FEET)
        alt /= 0.3048;
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(dialog->spin_alt), alt);
}


static void
cb_lookup_timezone(SoupSession *session,
                   SoupMessage *msg,
                   gpointer user_data)
{
    xfceweather_dialog *dialog = (xfceweather_dialog *) user_data;
    xml_timezone *timezone;

    timezone = (xml_timezone *)
        parse_xml_document(msg, (XmlParseFunc) parse_timezone);
    weather_dump(weather_dump_timezone, timezone);

    if (timezone) {
        gtk_entry_set_text(GTK_ENTRY(dialog->text_timezone),
                           timezone->timezone_id);
        xml_timezone_free(timezone);
    } else
        gtk_entry_set_text(GTK_ENTRY(dialog->text_timezone), "");
}


static void
lookup_altitude_timezone(const gpointer user_data)
{
    xfceweather_dialog *dialog = (xfceweather_dialog *) user_data;
    gchar *url, *latstr, *lonstr;
    gdouble lat, lon;

    lat = gtk_spin_button_get_value(GTK_SPIN_BUTTON(dialog->spin_lat));
    lon = gtk_spin_button_get_value(GTK_SPIN_BUTTON(dialog->spin_lon));

    latstr = double_to_string(lat, "%.6f");
    lonstr = double_to_string(lon, "%.6f");

    /* lookup altitude */
    url = g_strdup_printf("http://api.geonames.org"
                          "/srtm3XML?lat=%s&lng=%s&username=%s",
                          latstr, lonstr,
                          dialog->pd->geonames_username
                          ? dialog->pd->geonames_username : GEONAMES_USERNAME);
    weather_http_queue_request(dialog->pd->session, url,
                               cb_lookup_altitude, user_data);
    g_free(url);

    /* lookup timezone */
    url = g_strdup_printf("http://api.geonames.org"
                          "/timezone?lat=%s&lng=%s&username=%s",
                          latstr, lonstr,
                          dialog->pd->geonames_username
                          ? dialog->pd->geonames_username : GEONAMES_USERNAME);
    weather_http_queue_request(dialog->pd->session, url,
                               cb_lookup_timezone, user_data);
    g_free(url);

    g_free(lonstr);
    g_free(latstr);
}


static void
auto_locate_cb(const gchar *loc_name,
               const gchar *lat,
               const gchar *lon,
               const units_config *units,
               gpointer user_data)
{
    xfceweather_dialog *dialog = (xfceweather_dialog *) user_data;

    if (lat && lon && loc_name) {
        gtk_entry_set_text(GTK_ENTRY(dialog->text_loc_name), loc_name);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(dialog->spin_lat),
                                  string_to_double(lat, 0));
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(dialog->spin_lon),
                                  string_to_double(lon, 0));
        lookup_altitude_timezone(user_data);
    } else
        gtk_entry_set_text(GTK_ENTRY(dialog->text_loc_name), _("Unset"));
    setup_units(dialog, units);
    gtk_widget_set_sensitive(dialog->text_loc_name, TRUE);
}


static void
start_auto_locate(xfceweather_dialog *dialog)
{
    gtk_widget_set_sensitive(dialog->text_loc_name, FALSE);
    gtk_entry_set_text(GTK_ENTRY(dialog->text_loc_name), _("Detecting..."));
    gtk_spinner_start(GTK_SPINNER(dialog->update_spinner));
    weather_search_by_ip(dialog->pd->session, auto_locate_cb, dialog);
}


static gboolean
cb_findlocation(GtkButton *button,
                gpointer user_data)
{
    xfceweather_dialog *dialog = (xfceweather_dialog *) user_data;
    search_dialog *sdialog;
    gchar *loc_name, *lat, *lon;

    sdialog = create_search_dialog(NULL, dialog->pd->session);

    gtk_widget_set_sensitive(GTK_WIDGET(button), FALSE);
    if (run_search_dialog(sdialog)) {
        /* limit digit precision of coordinates from search results */
        lat = double_to_string(string_to_double(sdialog->result_lat, 0),
                               "%.6f");
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(dialog->spin_lat),
                                  string_to_double(lat, 0));
        lon = double_to_string(string_to_double(sdialog->result_lon, 0),
                               "%.6f");
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(dialog->spin_lon),
                                  string_to_double(lon, 0));
        loc_name = sanitize_location_name(sdialog->result_name);
        gtk_entry_set_text(GTK_ENTRY(dialog->text_loc_name), loc_name);
        g_free(loc_name);
        g_free(lon);
        g_free(lat);
    }
    free_search_dialog(sdialog);

    lookup_altitude_timezone(user_data);
    gtk_widget_set_sensitive(GTK_WIDGET(button), TRUE);

    return FALSE;
}


static void
setup_altitude(xfceweather_dialog *dialog)
{
    g_signal_handlers_block_by_func(dialog->spin_alt,
                                    G_CALLBACK(spin_alt_value_changed),
                                    dialog);
    switch (dialog->pd->units->altitude) {
    case FEET:
        gtk_label_set_text(GTK_LABEL(dialog->label_alt_unit),
                           _("feet"));
        gtk_spin_button_set_range(GTK_SPIN_BUTTON(dialog->spin_alt),
                                  -1378.0, 32808);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(dialog->spin_alt),
                                  (gdouble) dialog->pd->msl / 0.3048);
        break;

    case METERS:
    default:
        gtk_label_set_text(GTK_LABEL(dialog->label_alt_unit),
                           _("meters"));
        gtk_spin_button_set_range(GTK_SPIN_BUTTON(dialog->spin_alt),
                                  -420, 10000);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(dialog->spin_alt),
                                  (gdouble) (dialog->pd->msl));
        break;
    }
    g_signal_handlers_unblock_by_func(dialog->spin_alt,
                                      G_CALLBACK(spin_alt_value_changed),
                                      dialog);
    return;
}


static void
text_loc_name_changed(const GtkWidget *spin,
                      gpointer user_data)
{
    xfceweather_dialog *dialog = (xfceweather_dialog *) user_data;
    g_free(dialog->pd->location_name);
    dialog->pd->location_name =
        g_strdup(gtk_entry_get_text(GTK_ENTRY(dialog->text_loc_name)));
}


static void
spin_lat_value_changed(const GtkWidget *spin,
                       gpointer user_data)
{
    xfceweather_dialog *dialog = (xfceweather_dialog *) user_data;
    gdouble val;

    val = gtk_spin_button_get_value(GTK_SPIN_BUTTON(spin));
    g_free(dialog->pd->lat);
    dialog->pd->lat = double_to_string(val, "%.6f");
    schedule_delayed_data_update(dialog);
}


static void
spin_lon_value_changed(const GtkWidget *spin,
                       gpointer user_data)
{
    xfceweather_dialog *dialog = (xfceweather_dialog *) user_data;
    gdouble val;

    val = gtk_spin_button_get_value(GTK_SPIN_BUTTON(spin));
    g_free(dialog->pd->lon);
    dialog->pd->lon = double_to_string(val, "%.6f");
    schedule_delayed_data_update(dialog);
}


static void
spin_alt_value_changed(const GtkWidget *spin,
                       gpointer user_data)
{
    xfceweather_dialog *dialog = (xfceweather_dialog *) user_data;
    gdouble val;

    val = gtk_spin_button_get_value(GTK_SPIN_BUTTON(spin));
    if (dialog->pd->units->altitude == FEET)
        val *= 0.3048;
    dialog->pd->msl = (gint) val;
    schedule_delayed_data_update(dialog);
}


static void
text_timezone_changed(const GtkWidget *entry,
                      gpointer user_data)
{
    xfceweather_dialog *dialog = (xfceweather_dialog *) user_data;

    if (dialog->pd->timezone)
        g_free(dialog->pd->timezone);
    dialog->pd->timezone = g_strdup(gtk_entry_get_text(GTK_ENTRY(entry)));
    schedule_delayed_data_update(dialog);
}


static GtkWidget *
create_location_page(xfceweather_dialog *dialog)
{
    GtkWidget *palign, *page, *hbox, *vbox, *label, *image, *sep;
    GtkWidget *button_loc_change;
    GtkSizeGroup *sg_label, *sg_spin;

    ADD_PAGE(FALSE);
    sg_label = gtk_size_group_new(GTK_SIZE_GROUP_HORIZONTAL);
    sg_spin = gtk_size_group_new(GTK_SIZE_GROUP_HORIZONTAL);

    /* location name */
    hbox = gtk_hbox_new(FALSE, BORDER);
    ADD_LABEL(_("Location _name:"), sg_label);
    dialog->text_loc_name = gtk_entry_new();
    gtk_entry_set_max_length(GTK_ENTRY(dialog->text_loc_name),
                             LOC_NAME_MAX_LEN);
    gtk_label_set_mnemonic_widget(GTK_LABEL(label),
                                  GTK_WIDGET(dialog->text_loc_name));
    SET_TOOLTIP(dialog->text_loc_name,
                _("Change the name for the location to your liking. "
                  "It is used for display and does not affect the location "
                  "parameters in any way."));
    gtk_box_pack_start(GTK_BOX(hbox), dialog->text_loc_name, TRUE, TRUE, 0);
    button_loc_change = gtk_button_new_with_mnemonic(_("Chan_ge..."));
    image = gtk_image_new_from_stock(GTK_STOCK_FIND, GTK_ICON_SIZE_BUTTON);
    gtk_button_set_image(GTK_BUTTON(button_loc_change), image);
    SET_TOOLTIP(button_loc_change, _("Search for a new location and "
                                     "auto-detect its parameters."));
    g_signal_connect(G_OBJECT(button_loc_change), "clicked",
                     G_CALLBACK(cb_findlocation), dialog);
    gtk_box_pack_start(GTK_BOX(hbox), button_loc_change,
                       FALSE, FALSE, 0);
    if (dialog->pd->location_name)
        gtk_entry_set_text(GTK_ENTRY(dialog->text_loc_name),
                           dialog->pd->location_name);
    else
        gtk_entry_set_text(GTK_ENTRY(dialog->text_loc_name), _("Unset"));
    /* update spinner */
    dialog->update_spinner = gtk_spinner_new();
    gtk_box_pack_start(GTK_BOX(hbox), dialog->update_spinner, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(page), hbox, FALSE, FALSE, BORDER);

    /* latitude */
    vbox = gtk_vbox_new(FALSE, BORDER);
    hbox = gtk_hbox_new(FALSE, BORDER);
    ADD_LABEL(_("Latitud_e:"), sg_label);
    ADD_SPIN(dialog->spin_lat, -90, 90, 1,
             (string_to_double(dialog->pd->lat, 0)), 6, sg_spin);
    SET_TOOLTIP(dialog->spin_lat,
                _("Latitude specifies the north-south position of a point on "
                  "the Earth's surface. If you change this value manually, "
                  "you need to provide altitude and timezone manually too."));
    label = gtk_label_new("°");
    gtk_misc_set_alignment(GTK_MISC(label), 0, 0.5);
    gtk_box_pack_start(GTK_BOX(hbox), label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, BORDER);

    /* longitude */
    hbox = gtk_hbox_new(FALSE, BORDER);
    ADD_LABEL(_("L_ongitude:"), sg_label);
    ADD_SPIN(dialog->spin_lon, -180, 180, 1,
             (string_to_double(dialog->pd->lon, 0)), 6, sg_spin);
    SET_TOOLTIP(dialog->spin_lon,
                _("Longitude specifies the east-west position of a point on "
                  "the Earth's surface. If you change this value manually, "
                  "you need to provide altitude and timezone manually too."));
    label = gtk_label_new("°");
    gtk_misc_set_alignment(GTK_MISC(label), 0, 0.5);
    gtk_box_pack_start(GTK_BOX(hbox), label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, BORDER);

    /* altitude */
    hbox = gtk_hbox_new(FALSE, BORDER);
    ADD_LABEL(_("Altitu_de:"), sg_label);
    ADD_SPIN(dialog->spin_alt, -420, 10000, 1, dialog->pd->msl, 0, sg_spin);
    SET_TOOLTIP
        (dialog->spin_alt,
         _("For locations outside Norway the elevation model that's used by "
           "the met.no webservice is not very good, so it's usually necessary "
           "to specify the altitude as an additional parameter, otherwise the "
           "reported values will not be correct.\n\n"
           "The plugin tries to auto-detect the altitude using the GeoNames "
           "webservice, but that might not always be correct too, so you "
           "can change it here.\n\n"
           "Altitude is given in meters above sea level, or alternatively "
           "in feet by changing the unit on the units page. It should match "
           "the real value roughly, but small differences will have no "
           "influence on the weather data. Inside Norway, this setting has "
           "no effect at all."));
    dialog->label_alt_unit = gtk_label_new(NULL);
    gtk_misc_set_alignment(GTK_MISC(dialog->label_alt_unit), 0, 0.5);
    gtk_box_pack_start(GTK_BOX(hbox), dialog->label_alt_unit, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, BORDER);

    /* timezone */
    hbox = gtk_hbox_new(FALSE, BORDER);
    ADD_LABEL(_("_Timezone:"), sg_label);
    dialog->text_timezone = gtk_entry_new();
    gtk_entry_set_max_length(GTK_ENTRY(dialog->text_timezone),
                             LOC_NAME_MAX_LEN);
    gtk_label_set_mnemonic_widget(GTK_LABEL(label),
                                  GTK_WIDGET(dialog->text_timezone));
    SET_TOOLTIP
        (dialog->text_timezone,
         _("If the chosen location is not in your current timezone, then "
           "it is necessary to <i>put</i> the plugin into that other "
           "timezone for the times to be shown correctly. The proper "
           "timezone will be auto-detected via the GeoNames web service, "
           "but you might want to correct it if necessary.\n"
           "Leave this field empty to use the timezone set by your "
           "system. Invalid entries will cause the use of UTC time, but "
           "that may also depend on your system."));
    gtk_box_pack_start(GTK_BOX(hbox), dialog->text_timezone, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, BORDER);
    if (dialog->pd->timezone)
        gtk_entry_set_text(GTK_ENTRY(dialog->text_timezone),
                           dialog->pd->timezone);
    else
        gtk_entry_set_text(GTK_ENTRY(dialog->text_timezone), "");

    /* separator */
    sep = gtk_hseparator_new();
    gtk_box_pack_start(GTK_BOX(vbox), sep, FALSE, FALSE, BORDER * 2);

    /* instructions for correction of altitude and timezone */
    hbox = gtk_hbox_new(FALSE, BORDER);
    label = gtk_label_new(NULL);
    gtk_label_set_markup
        (GTK_LABEL(label),
         _("<i>Please change location name to your liking and "
           "correct\naltitude and timezone if they are "
           "not auto-detected correctly.</i>"));
    gtk_misc_set_alignment(GTK_MISC(label), 0, 0.5);
    gtk_box_pack_start(GTK_BOX(hbox), label, FALSE, FALSE, BORDER/2);
    gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, BORDER/2);
    gtk_box_pack_start(GTK_BOX(page), vbox, FALSE, FALSE, 0);

    /* set up the altitude spin box and unit label (meters/feet) */
    setup_altitude(dialog);

    g_object_unref(G_OBJECT(sg_label));
    g_object_unref(G_OBJECT(sg_spin));
    return palign;
}


static void
combo_unit_temperature_set_tooltip(GtkWidget *combo)
{
    gchar *text = NULL;
    gint value = gtk_combo_box_get_active(GTK_COMBO_BOX(combo));

    switch (value) {
    case CELSIUS:
        text = _("Named after the astronomer Anders Celsius who invented the "
                 "original scale in 1742, the Celsius scale is an "
                 "international standard unit and nowadays defined "
                 "using the Kelvin scale. 0 °C is equivalent to 273.15 K "
                 "and 1 °C difference in temperature is exactly the same "
                 "difference as 1 K. It is defined with the melting point "
                 "of water being roughly at 0 °C and its boiling point at "
                 "100 °C at one standard atmosphere (1 atm = 1013.5 hPa). "
                 "Until 1948, the unit was known as <i>centigrade</i> - from "
                 "Latin <i>centum</i> (100) and <i>gradus</i> (steps).\n"
                 "In meteorology and everyday life the Celsius scale is "
                 "very convenient for expressing temperatures because its "
                 "numbers can be an easy indicator for the formation of "
                 "black ice and snow.");
        break;
    case FAHRENHEIT:
        text = _("The current Fahrenheit temperature scale is based on one "
                 "proposed in 1724 by the physicist Daniel Gabriel "
                 "Fahrenheit. 0 °F was the freezing point of brine on the "
                 "original scale at standard atmospheric pressure, which "
                 "was the lowest temperature achievable with this mixture "
                 "of ice, salt and ammonium chloride. The melting point of "
                 "water is at 32 °F and its boiling point at 212 °F. "
                 "The Fahrenheit and Celsius scales intersect at -40 "
                 "degrees. Even in cold winters, the temperatures usually "
                 "do not fall into negative ranges on the Fahrenheit scale.\n"
                 "With its inventor being a member of the Royal Society in "
                 "London and having a high reputation, the Fahrenheit scale "
                 "enjoyed great popularity in many English-speaking countries, "
                 "but was replaced by the Celsius scale in most of these "
                 "countries during the metrification process in the mid to "
                 "late 20th century.");
        break;
    }
    gtk_widget_set_tooltip_markup(GTK_WIDGET(combo), text);
}


static void
combo_unit_temperature_changed(GtkWidget *combo,
                               gpointer user_data)
{
    xfceweather_dialog *dialog = (xfceweather_dialog *) user_data;
    dialog->pd->units->temperature =
        gtk_combo_box_get_active(GTK_COMBO_BOX(combo));
    combo_unit_temperature_set_tooltip(combo);
    update_valuebox(dialog->pd, TRUE);
    update_summary_window(dialog, TRUE);
}


static void
combo_unit_pressure_set_tooltip(GtkWidget *combo)
{
    gchar *text = NULL;
    gint value = gtk_combo_box_get_active(GTK_COMBO_BOX(combo));

    switch (value) {
    case HECTOPASCAL:
        text = _("The pascal, named after mathematician, physicist and "
                 "philosopher Blaise Pascal, is a SI derived unit and a "
                 "measure of force per unit area, defined as one newton "
                 "per square meter. One standard atmosphere (atm) is "
                 "1013.25 hPa.");
        break;
    case INCH_MERCURY:
        text = _("Inches of mercury is still widely used for barometric "
                 "pressure in weather reports, refrigeration and aviation "
                 "in the United States, but seldom used elsewhere. "
                 "It is defined as the pressure exerted by a 1 inch "
                 "circular column of mercury of 1 inch in height at "
                 "32 °F (0 °C) at the standard acceleration of gravity.");
        break;
    case PSI:
        text = _("The pound per square inch is a unit of pressure based on "
                 "avoirdupois units (a system of weights based on a pound of "
                 "16 ounces) and defined as the pressure resulting from a "
                 "force of one pound-force applied to an area of one square "
                 "inch. It is used in the United States and to varying "
                 "degrees in everyday life in Canada, the United Kingdom and "
                 "maybe some former British Colonies.");
        break;
    case TORR:
        text = _("The torr unit was named after the physicist and "
                 "mathematician Evangelista Torricelli who discovered the "
                 "principle of the barometer in 1644 and demonstrated the "
                 "first mercury barometer to the general public. A pressure "
                 "of 1 torr is approximately equal to one millimeter of "
                 "mercury, and one standard atmosphere (atm) equals 760 "
                 "Torr.");
        break;
    }
    gtk_widget_set_tooltip_markup(GTK_WIDGET(combo), text);
}


static void
combo_unit_pressure_changed(GtkWidget *combo,
                            gpointer user_data)
{
    xfceweather_dialog *dialog = (xfceweather_dialog *) user_data;
    dialog->pd->units->pressure =
        gtk_combo_box_get_active(GTK_COMBO_BOX(combo));
    combo_unit_pressure_set_tooltip(combo);
    update_valuebox(dialog->pd, TRUE);
    update_summary_window(dialog, TRUE);
}


static void
combo_unit_windspeed_set_tooltip(GtkWidget *combo)
{
    gchar *text = NULL;
    gint value = gtk_combo_box_get_active(GTK_COMBO_BOX(combo));

    switch (value) {
    case KMH:
        text = _("Wind speeds in TV or in the news are often provided in "
                 "km/h.");
        break;
    case MPH:
        text = _("Miles per hour is an imperial unit of speed expressing the "
                 "number of statute miles covered in one hour.");
        break;
    case MPS:
        text = _("Meter per second is <i>the</i> unit typically used by "
                 "meteorologists to denote wind speeds.");
        break;
    case FTS:
        text = _("The foot per second (pl. feet per second) in the imperial "
                 "unit system is the counterpart to the meter per second in "
                 "the International System of Units.");
        break;
    case KNOTS:
        text = _("The knot is a unit of speed equal to one international "
                 "nautical mile (1.852 km) per hour, or approximately "
                 "1.151 mph, and sees worldwide use in meteorology and "
                 "in maritime and air navigation. A vessel travelling at "
                 "1 knot along a meridian travels one minute of geographic "
                 "latitude in one hour.");
        break;
    }
    gtk_widget_set_tooltip_markup(GTK_WIDGET(combo), text);
}


static void
combo_unit_windspeed_changed(GtkWidget *combo,
                             gpointer user_data)
{
    xfceweather_dialog *dialog = (xfceweather_dialog *) user_data;
    dialog->pd->units->windspeed =
        gtk_combo_box_get_active(GTK_COMBO_BOX(combo));
    combo_unit_windspeed_set_tooltip(combo);
    update_valuebox(dialog->pd, TRUE);
    update_summary_window(dialog, TRUE);
}


static void
combo_unit_precipitation_set_tooltip(GtkWidget *combo)
{
    gchar *text = NULL;
    gint value = gtk_combo_box_get_active(GTK_COMBO_BOX(combo));

    switch (value) {
    case MILLIMETERS:
        text = _("1 millimeter is one thousandth of a meter - the fundamental "
                 "unit of length in the International System of Units -, or "
                 "approximately 0.04 inches.");
        break;
    case INCHES:
        text = _("The English word <i>inch</i> comes from Latin <i>uncia</i> "
                 "meaning <i>one-twelfth part</i> (in this case, one twelfth "
                 "of a foot). In the past, there have been many different "
                 "standards of the inch with varying sizes of measure, but "
                 "the current internationally accepted value is exactly 25.4 "
                 "millimeters.");
        break;
    }
    gtk_widget_set_tooltip_markup(GTK_WIDGET(combo), text);
}


static void
combo_unit_precipitation_changed(GtkWidget *combo,
                                 gpointer user_data)
{
    xfceweather_dialog *dialog = (xfceweather_dialog *) user_data;
    dialog->pd->units->precipitation =
        gtk_combo_box_get_active(GTK_COMBO_BOX(combo));
    combo_unit_precipitation_set_tooltip(combo);
    update_valuebox(dialog->pd, TRUE);
    update_summary_window(dialog, TRUE);
}


static void
combo_unit_altitude_set_tooltip(GtkWidget *combo)
{
    gchar *text = NULL;
    gint value = gtk_combo_box_get_active(GTK_COMBO_BOX(combo));

    switch (value) {
    case METERS:
        text = _("The meter is the fundamental unit of length in the "
                 "International System of Units. Originally intended "
                 "to be one ten-millionth of the distance from the Earth's "
                 "equator to the North Pole at sea level, its definition "
                 "has been periodically refined to reflect growing "
                 "knowledge of metrology (the science of measurement).");
        break;
    case FEET:
        text = _("A foot (plural feet) is a unit of length defined as being "
                 "0.3048 m exactly and used in the imperial system of units "
                 "and United States customary units. It is subdivided into "
                 "12 inches. The measurement of altitude in the aviation "
                 "industry is one of the few areas where the foot is widely "
                 "used outside the English-speaking world.");
        break;
    }
    gtk_widget_set_tooltip_markup(GTK_WIDGET(combo), text);
}


static void
combo_unit_altitude_changed(GtkWidget *combo,
                            gpointer user_data)
{
    xfceweather_dialog *dialog = (xfceweather_dialog *) user_data;

    dialog->pd->units->altitude =
        gtk_combo_box_get_active(GTK_COMBO_BOX(combo));
    combo_unit_altitude_set_tooltip(combo);
    setup_altitude(dialog);
    update_summary_window(dialog, TRUE);
}


static void
combo_apparent_temperature_set_tooltip(GtkWidget *combo)
{
    gchar *text = NULL;
    gint value = gtk_combo_box_get_active(GTK_COMBO_BOX(combo));

    switch (value) {
    case WINDCHILL_HEATINDEX:
        /*
         * TRANSLATORS: The Summer Simmer Index is similar to the heat
         * index, but usually used at night because of its better accuracy
         * at that time.
         */
        text = _("Used in North America, wind chill will be reported for low "
                 "temperatures and heat index for higher ones. At night, heat "
                 "index will be replaced by the Summer Simmer Index. For wind "
                 "chill, wind speeds need to be above 3.0 mph (4.828 km/h) "
                 "and air temperature below 50.0 °F (10.0 °C). For heat "
                 "index, air temperature needs to be above 80 °F (26.7 °C) "
                 "- or above 71.6 °F (22 °C) at night - and relative humidity "
                 "at least 40%. If these conditions are not met, the air "
                 "temperature will be shown.") ;
        break;
    case WINDCHILL_HUMIDEX:
        text = _("The Canadian counterpart to the US windchill/heat index, "
                 "with the wind chill being similar to the previous model "
                 "but with slightly different constraints. Instead of the "
                 "heat index <i>humidex</i> will be used. For wind chill "
                 "to become effective, wind speeds need to be above 2.0 "
                 "km/h (1.24 mph) and air temperature below or equal to "
                 "0 °C (32 °F). For humidex, air temperature needs to "
                 "be at least 20.0 °C (68 °F), with a dewpoint greater than "
                 "0 °C (32 °F). If these conditions are not met, the air "
                 "temperature will be shown.");
        break;
    case STEADMAN:
        text = _("This is the model used by the Australian Bureau of "
                 "Meteorology, especially adapted for the climate of this "
                 "continent. Possibly used in Central Europe and parts of "
                 "other continents too, but then windchill and similar values "
                 "had never gained that much popularity there as in the US "
                 "or Canada, so information about its usage is scarce or "
                 "uncertain. It depends on air temperature, wind speed and "
                 "humidity and can be used for lower and higher "
                 "temperatures alike.");
        break;
    case QUAYLE_STEADMAN:
        text = _("Improvements by Robert G. Quayle and Robert G. Steadman "
                 "applied in 1998 to earlier experiments/developments by "
                 "Steadman. This model only depends on wind speed and "
                 "temperature, not on relative humidity and can be used "
                 "for both heat and cold stress.");
        break;
    }
    gtk_widget_set_tooltip_markup(GTK_WIDGET(combo), text);
}


static void
combo_apparent_temperature_changed(GtkWidget *combo,
                                   gpointer user_data)
{
    xfceweather_dialog *dialog = (xfceweather_dialog *) user_data;
    dialog->pd->units->apparent_temperature =
        gtk_combo_box_get_active(GTK_COMBO_BOX(combo));
    combo_apparent_temperature_set_tooltip(combo);
    update_valuebox(dialog->pd, TRUE);
    update_summary_window(dialog, TRUE);
}


static GtkWidget *
create_units_page(xfceweather_dialog *dialog)
{
    GtkWidget *palign, *page, *hbox, *vbox, *label, *sep;
    GtkSizeGroup *sg_label;

    ADD_PAGE(FALSE);
    sg_label = gtk_size_group_new(GTK_SIZE_GROUP_HORIZONTAL);
    vbox = gtk_vbox_new(FALSE, BORDER);

    /* temperature */
    hbox = gtk_hbox_new(FALSE, BORDER);
    ADD_LABEL(_("_Temperature:"), sg_label);
    ADD_COMBO(dialog->combo_unit_temperature);
    ADD_COMBO_VALUE(dialog->combo_unit_temperature, _("Celsius (°C)"));
    ADD_COMBO_VALUE(dialog->combo_unit_temperature, _("Fahrenheit (°F)"));
    gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, BORDER);

    /* atmospheric pressure */
    hbox = gtk_hbox_new(FALSE, BORDER);
    ADD_LABEL(_("Barometric _pressure:"), sg_label);
    ADD_COMBO(dialog->combo_unit_pressure);
    ADD_COMBO_VALUE(dialog->combo_unit_pressure,
                    _("Hectopascals (hPa)"));
    ADD_COMBO_VALUE(dialog->combo_unit_pressure,
                    _("Inches of mercury (inHg)"));
    ADD_COMBO_VALUE(dialog->combo_unit_pressure,
                    _("Pound-force per square inch (psi)"));
    ADD_COMBO_VALUE(dialog->combo_unit_pressure,
                    _("Torr (mmHg)"));
    gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, BORDER);

    /* wind speed */
    hbox = gtk_hbox_new(FALSE, BORDER);
    ADD_LABEL(_("_Wind speed:"), sg_label);
    ADD_COMBO(dialog->combo_unit_windspeed);
    ADD_COMBO_VALUE(dialog->combo_unit_windspeed,
                    _("Kilometers per hour (km/h)"));
    ADD_COMBO_VALUE(dialog->combo_unit_windspeed,
                    _("Miles per hour (mph)"));
    ADD_COMBO_VALUE(dialog->combo_unit_windspeed,
                    _("Meters per second (m/s)"));
    ADD_COMBO_VALUE(dialog->combo_unit_windspeed,
                    _("Feet per second (ft/s)"));
    ADD_COMBO_VALUE(dialog->combo_unit_windspeed,
                    _("Knots (kt)"));
    gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, BORDER);

    /* precipitation */
    hbox = gtk_hbox_new(FALSE, BORDER);
    ADD_LABEL(_("Prec_ipitations:"), sg_label);
    ADD_COMBO(dialog->combo_unit_precipitation);
    ADD_COMBO_VALUE(dialog->combo_unit_precipitation,
                    _("Millimeters (mm)"));
    ADD_COMBO_VALUE(dialog->combo_unit_precipitation,
                    _("Inches (in)"));
    gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, BORDER);

    /* altitude */
    hbox = gtk_hbox_new(FALSE, BORDER);
    ADD_LABEL(_("Altitu_de:"), sg_label);
    ADD_COMBO(dialog->combo_unit_altitude);
    ADD_COMBO_VALUE(dialog->combo_unit_altitude,
                    _("Meters (m)"));
    ADD_COMBO_VALUE(dialog->combo_unit_altitude,
                    _("Feet (ft)"));
    gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, BORDER);

    /* separator */
    sep = gtk_hseparator_new();
    gtk_box_pack_start(GTK_BOX(vbox), sep, FALSE, FALSE, BORDER * 2);

    /* apparent temperature model */
    hbox = gtk_hbox_new(FALSE, BORDER);
    ADD_LABEL(_("Apparent te_mperature:"), sg_label);
    ADD_COMBO(dialog->combo_apparent_temperature);
    ADD_COMBO_VALUE(dialog->combo_apparent_temperature,
                    _("Windchill/Heat index"));
    ADD_COMBO_VALUE(dialog->combo_apparent_temperature,
                    _("Windchill/Humidex"));
    ADD_COMBO_VALUE(dialog->combo_apparent_temperature, _("Steadman"));
    ADD_COMBO_VALUE(dialog->combo_apparent_temperature, _("Quayle-Steadman"));
    gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, BORDER);

    /* initialize widgets with current data */
    if (dialog->pd)
        setup_units(dialog, dialog->pd->units);

    gtk_box_pack_start(GTK_BOX(page), vbox, FALSE, FALSE, 0);
    g_object_unref(G_OBJECT(sg_label));
    return palign;
}


static void
combo_icon_theme_set_tooltip(GtkWidget *combo,
                             gpointer user_data)
{
    xfceweather_dialog *dialog = (xfceweather_dialog *) user_data;
    gchar *text;

    if (G_UNLIKELY(dialog->pd->icon_theme == NULL)) {
        /* this should never happen, but who knows... */
        gtk_widget_set_tooltip_text(GTK_WIDGET(combo),
                                    _("Choose an icon theme."));
        return;
    }

    text = g_strdup_printf
        (_("<b>Directory:</b> %s\n\n"
           "<b>Author:</b> %s\n\n"
           "<b>Description:</b> %s\n\n"
           "<b>License:</b> %s"),
         TEXT_UNKNOWN(dialog->pd->icon_theme->dir),
         TEXT_UNKNOWN(dialog->pd->icon_theme->author),
         TEXT_UNKNOWN(dialog->pd->icon_theme->description),
         TEXT_UNKNOWN(dialog->pd->icon_theme->license));
    gtk_widget_set_tooltip_markup(GTK_WIDGET(combo), text);
    g_free(text);
}


static void
combo_icon_theme_changed(GtkWidget *combo,
                         gpointer user_data)
{
    xfceweather_dialog *dialog = (xfceweather_dialog *) user_data;
    icon_theme *theme;
    gint i;

    i = gtk_combo_box_get_active(GTK_COMBO_BOX(combo));
    if (G_UNLIKELY(i == -1))
        return;

    theme = g_array_index(dialog->icon_themes, icon_theme *, i);
    if (G_UNLIKELY(theme == NULL))
        return;

    icon_theme_free(dialog->pd->icon_theme);
    dialog->pd->icon_theme = icon_theme_copy(theme);
    combo_icon_theme_set_tooltip(combo, dialog);
    update_icon(dialog->pd);
    update_summary_window(dialog, TRUE);
}


static void
button_icons_dir_clicked(GtkWidget *button,
                         gpointer user_data)
{
    gchar *dir, *command;

    dir = get_user_icons_dir();
    g_mkdir_with_parents(dir, 0755);
    command = g_strdup_printf("exo-open %s", dir);
    g_free(dir);
    xfce_spawn_command_line_on_screen(gdk_screen_get_default(),
                                      command, FALSE, TRUE, NULL);
    g_free(command);
}


static void
check_single_row_toggled(GtkWidget *button,
                         gpointer user_data)
{
    xfceweather_dialog *dialog = (xfceweather_dialog *) user_data;
    dialog->pd->single_row =
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(button));
#if LIBXFCE4PANEL_CHECK_VERSION(4,9,0)
    xfceweather_set_mode(dialog->pd->plugin,
                         xfce_panel_plugin_get_mode(dialog->pd->plugin),
                         dialog->pd);
#endif
}


static void
combo_tooltip_style_changed(GtkWidget *combo,
                            gpointer user_data)
{
    xfceweather_dialog *dialog = (xfceweather_dialog *) user_data;
    dialog->pd->tooltip_style = gtk_combo_box_get_active(GTK_COMBO_BOX(combo));
}


static void
combo_forecast_layout_set_tooltip(GtkWidget *combo)
{
    gchar *text = NULL;
    gint value = gtk_combo_box_get_active(GTK_COMBO_BOX(combo));

    switch (value) {
    case FC_LAYOUT_CALENDAR:
        text = _("A more calendar-like view, with the days in columns and the "
                 "daytimes (morning, afternoon, evening, night) in rows.");
        break;
    case FC_LAYOUT_LIST:
        text = _("Shows the forecasts in a table with the daytimes (morning, "
                 "afternoon, evening, night) in columns and the days in rows.");
        break;
    }
    gtk_widget_set_tooltip_markup(GTK_WIDGET(combo), text);
}


static void
combo_forecast_layout_changed(GtkWidget *combo,
                              gpointer user_data)
{
    xfceweather_dialog *dialog = (xfceweather_dialog *) user_data;
    dialog->pd->forecast_layout =
        gtk_combo_box_get_active(GTK_COMBO_BOX(combo));
    combo_forecast_layout_set_tooltip(combo);
    update_summary_window(dialog, FALSE);
}


static void
spin_forecast_days_value_changed(const GtkWidget *spin,
                                 gpointer user_data)
{
    xfceweather_dialog *dialog = (xfceweather_dialog *) user_data;
    dialog->pd->forecast_days =
        gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(spin));
    update_summary_window(dialog, FALSE);
}


static void
check_round_values_toggled(GtkWidget *button,
                           gpointer user_data)
{
    xfceweather_dialog *dialog = (xfceweather_dialog *) user_data;
    dialog->pd->round =
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(button));
    update_valuebox(dialog->pd, TRUE);
    update_summary_window(dialog, TRUE);
}


static GtkWidget *
create_appearance_page(xfceweather_dialog *dialog)
{
    GtkWidget *palign, *page, *sep, *hbox, *vbox, *label, *image;
    GtkSizeGroup *sg;
    GtkSettings *default_settings;
    icon_theme *theme;
    gchar *text;
    gint i;

    ADD_PAGE(FALSE);
    sg = gtk_size_group_new(GTK_SIZE_GROUP_HORIZONTAL);

    /* font and color */
    hbox = gtk_hbox_new(FALSE, BORDER);
    label = gtk_label_new(_("Font and color:"));
    gtk_misc_set_alignment(GTK_MISC(label), 1, 0.5);
    gtk_box_pack_start(GTK_BOX(hbox), label, FALSE, FALSE, 0);
    dialog->button_appearance_font =
        gtk_button_new_with_mnemonic(_("Select _font"));
    SET_TOOLTIP(dialog->button_appearance_font,
                _("Choose a font you like and set it to a smaller or larger "
                  "size. Middle-click on the button to unset the font and use "
                  "your theme's default."));
    gtk_box_pack_start(GTK_BOX(hbox), dialog->button_appearance_font,
                       TRUE, TRUE, 0);
    if (dialog->pd->valuebox_font)
        gtk_button_set_label(GTK_BUTTON(dialog->button_appearance_font),
                             dialog->pd->valuebox_font);
    dialog->button_appearance_color =
        gtk_color_button_new_with_color(&(dialog->pd->valuebox_color));
    gtk_size_group_add_widget(sg, dialog->button_appearance_color);
    SET_TOOLTIP(dialog->button_appearance_color,
                _("There may be problems with some themes that cause the "
                  "scrollbox text to be hardly readable. If this is the case "
                  "or you simply want it to appear in another color, then "
                  "you can change it using this button. Middle-click on the "
                  "button to unset the scrollbox text color."));
    gtk_box_pack_start(GTK_BOX(hbox), dialog->button_appearance_color,
                       FALSE, FALSE, 0 );
    gtk_box_pack_start(GTK_BOX(page), hbox, FALSE, FALSE, 0);

    /* separator */
    sep = gtk_hseparator_new();
    gtk_box_pack_start(GTK_BOX(page), sep, FALSE, FALSE, BORDER * 2);
    
    /* icon theme */
    vbox = gtk_vbox_new(FALSE, BORDER);
    hbox = gtk_hbox_new(FALSE, BORDER);
    ADD_LABEL(_("_Icon theme:"), sg);
    SET_TOOLTIP(label,
                _("Available icon themes are listed here. You can add icon "
                  "themes to $HOME/.config/xfce4/weather/icons (or the "
                  "equivalent directory on your system). Information about "
                  "how to create or use icon themes can be found in the "
                  "README file. New icon themes will be detected everytime "
                  "you open this config dialog."));
    ADD_COMBO(dialog->combo_icon_theme);
    gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, 0);
    default_settings = gtk_settings_get_default();
    g_object_set(default_settings, "gtk-button-images", TRUE, NULL);
    image = gtk_image_new();
    gtk_image_set_from_stock(GTK_IMAGE(image), GTK_STOCK_OPEN,
                             GTK_ICON_SIZE_BUTTON);
    dialog->button_icons_dir = gtk_button_new();
    gtk_button_set_image(GTK_BUTTON(dialog->button_icons_dir), image);
    gtk_box_pack_start(GTK_BOX(hbox), dialog->button_icons_dir,
                       FALSE, FALSE, 0);
    SET_TOOLTIP(dialog->button_icons_dir,
                _("Open the user icon themes directory in your file manager, "
                  "creating it if necessary."));
    dialog->icon_themes = find_icon_themes();
    for (i = 0; i < dialog->icon_themes->len; i++) {
        theme = g_array_index(dialog->icon_themes, icon_theme *, i);
        ADD_COMBO_VALUE(dialog->combo_icon_theme, theme->name);
        /* set selection to current theme */
        if (G_LIKELY(dialog->pd->icon_theme) &&
            !strcmp(theme->dir, dialog->pd->icon_theme->dir)) {
            SET_COMBO_VALUE(dialog->combo_icon_theme, i);
            combo_icon_theme_set_tooltip(dialog->combo_icon_theme, dialog);
        }
    }

    /* always use small icon in panel */
    hbox = gtk_hbox_new(FALSE, BORDER);
    dialog->check_single_row =
        gtk_check_button_new_with_mnemonic(_("Use only a single _panel row"));
    SET_TOOLTIP(dialog->check_single_row,
                _("Check to always use only a single row on a multi-row panel "
                  "and a small icon in deskbar mode."));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(dialog->check_single_row),
                                 dialog->pd->single_row);
    gtk_box_pack_start(GTK_BOX(hbox), dialog->check_single_row,
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), vbox, FALSE, FALSE, 0);

    sep = gtk_hseparator_new();
    gtk_box_pack_start(GTK_BOX(page), sep, FALSE, FALSE, BORDER * 2);

    /* tooltip style */
    hbox = gtk_hbox_new(FALSE, BORDER);
    vbox = gtk_vbox_new(FALSE, BORDER);
    ADD_LABEL(_("_Tooltip style:"), sg);
    ADD_COMBO(dialog->combo_tooltip_style);
    ADD_COMBO_VALUE(dialog->combo_tooltip_style, _("Simple"));
    ADD_COMBO_VALUE(dialog->combo_tooltip_style, _("Verbose"));
    SET_COMBO_VALUE(dialog->combo_tooltip_style, dialog->pd->tooltip_style);
    SET_TOOLTIP(dialog->combo_tooltip_style,
                _("Choose your preferred tooltip style. Some styles "
                  "give a lot of useful data, some are clearer but "
                  "provide less data on a glance."));
    gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), vbox, FALSE, FALSE, 0);

    sep = gtk_hseparator_new();
    gtk_box_pack_start(GTK_BOX(page), sep, FALSE, FALSE, BORDER * 2);

    /* forecast layout */
    vbox = gtk_vbox_new(FALSE, BORDER);
    hbox = gtk_hbox_new(FALSE, BORDER);
    ADD_LABEL(_("_Forecast layout:"), sg);
    ADD_COMBO(dialog->combo_forecast_layout);
    ADD_COMBO_VALUE(dialog->combo_forecast_layout, _("Days in columns"));
    ADD_COMBO_VALUE(dialog->combo_forecast_layout, _("Days in rows"));
    SET_COMBO_VALUE(dialog->combo_forecast_layout,
                    dialog->pd->forecast_layout);
    combo_forecast_layout_set_tooltip(dialog->combo_forecast_layout);
    gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, 0);

    /* number of days shown in forecast */
    hbox = gtk_hbox_new(FALSE, BORDER);
    ADD_LABEL(_("_Number of forecast days:"), sg);
    ADD_SPIN(dialog->spin_forecast_days, 1, MAX_FORECAST_DAYS, 1,
             (dialog->pd->forecast_days ? dialog->pd->forecast_days : 5),
             0, NULL);
    text = g_strdup_printf
        (_("Met.no provides forecast data for up to %d days in the "
           "future. Choose how many days will be shown in the forecast "
           "tab in the summary window. On slower computers, a lower "
           "number might help against lags when opening the window. "
           "Note however that usually forecasts for more than three "
           "days in the future are unreliable at best ;-)"),
         MAX_FORECAST_DAYS);
    SET_TOOLTIP(dialog->spin_forecast_days, text);
    g_free(text);
    gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), vbox, FALSE, FALSE, 0);

    sep = gtk_hseparator_new();
    gtk_box_pack_start(GTK_BOX(page), sep, FALSE, FALSE, BORDER * 2);

    /* round temperature */
    vbox = gtk_vbox_new(FALSE, BORDER);
    dialog->check_round_values =
        gtk_check_button_new_with_mnemonic(_("_Round values"));
    SET_TOOLTIP(dialog->check_round_values,
                _("Check to round values everywhere except on the details "
                  "page in the summary window."));
    gtk_box_pack_start(GTK_BOX(vbox), dialog->check_round_values,
                       FALSE, FALSE, 0);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(dialog->check_round_values),
                                 dialog->pd->round);

    gtk_box_pack_start(GTK_BOX(page), vbox, FALSE, FALSE, 0);
    g_object_unref(G_OBJECT(sg));
    return palign;
}

static void spin_update_interval_value_changed(const GtkWidget *spin,
                                               gpointer user_data) {
  xfceweather_dialog *dialog = (xfceweather_dialog *)user_data;
  dialog->pd->update_interval =
      gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(spin));
  update_valuebox(dialog->pd, TRUE);
}


static gboolean
button_appearance_font_pressed(GtkWidget *button,
                              GdkEventButton *event,
                              gpointer user_data)
{
    xfceweather_dialog *dialog = (xfceweather_dialog *) user_data;

    if (event->type != GDK_BUTTON_PRESS)
        return FALSE;

    if (event->button == 2) {
        g_free(dialog->pd->valuebox_font);
        dialog->pd->valuebox_font = NULL;
        gtk_button_set_label(GTK_BUTTON(button), _("Select _font"));
        return TRUE;
    }

    return FALSE;
}

static gboolean
button_appearance_font_clicked(GtkWidget *button, gpointer user_data)
{
    xfceweather_dialog *dialog = (xfceweather_dialog *) user_data;
    GtkFontSelectionDialog *fsd;
    gchar *fontname;
    gint result;

    fsd = GTK_FONT_SELECTION_DIALOG
        (gtk_font_selection_dialog_new(_("Select font")));
    if (dialog->pd->valuebox_font)
        gtk_font_selection_dialog_set_font_name(fsd,
                                                dialog->pd->valuebox_font);

    result = gtk_dialog_run(GTK_DIALOG(fsd));
    if (result == GTK_RESPONSE_OK || result == GTK_RESPONSE_ACCEPT) {
        fontname = gtk_font_selection_dialog_get_font_name(fsd);
        if (fontname != NULL) {
            gtk_button_set_label(GTK_BUTTON(button), fontname);
            g_free(dialog->pd->valuebox_font);
            dialog->pd->valuebox_font = fontname;
            gtk_widget_modify_font(
                GTK_WIDGET(dialog->pd->valuebox),
                pango_font_description_from_string(fontname));
        }
    }
    gtk_widget_destroy(GTK_WIDGET(fsd));
    return FALSE;
}


static gboolean
button_appearance_color_pressed(GtkWidget *button,
                               GdkEventButton *event,
                               gpointer user_data)
{
    xfceweather_dialog *dialog = (xfceweather_dialog *) user_data;

    if (event->type != GDK_BUTTON_PRESS)
        return FALSE;

    if (event->button == 2)
        return TRUE;

    return FALSE;
}


static void
button_appearance_color_set(GtkWidget *button,
                           gpointer user_data)
{
    xfceweather_dialog *dialog = (xfceweather_dialog *) user_data;

    gtk_color_button_get_color(GTK_COLOR_BUTTON(button),
                               &(dialog->pd->valuebox_color));
    gtk_widget_modify_fg(GTK_WIDGET(dialog->pd->valuebox), GTK_STATE_NORMAL,
                         &dialog->pd->valuebox_color);
}


static GtkWidget *
create_interval_page(xfceweather_dialog *dialog)
{
    GtkWidget *palign, *page, *hbox, *label;
    GtkSizeGroup *sg;
    data_types type;
    gint i, n;
    gchar* update_interval;

    ADD_PAGE(FALSE);
    sg = gtk_size_group_new(GTK_SIZE_GROUP_HORIZONTAL);

    /* How often should the weather be checked */
    hbox = gtk_hbox_new(FALSE, BORDER);
    label = gtk_label_new_with_mnemonic(_("_Update interval:"));
    gtk_misc_set_alignment(GTK_MISC(label), 0, 0.5);
    gtk_box_pack_start(GTK_BOX(hbox), label, TRUE, TRUE, 0);

    update_interval = g_strdup_printf("%d", dialog->pd->update_interval);
    ADD_SPIN(dialog->spin_update_interval, 10, 9999, 10,
             dialog->pd->update_interval, 0, sg);
    SET_TOOLTIP(dialog->spin_update_interval,
                _("Decide how often the weather should be updated."));

    label = gtk_label_new("seconds");
    gtk_misc_set_alignment(GTK_MISC(label), 0, 0.5);
    gtk_box_pack_start(GTK_BOX(hbox), label, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(page), hbox, FALSE, FALSE, 0);

    g_free(update_interval);
    g_object_unref(G_OBJECT(sg));
    return palign;
}


static void
notebook_page_switched(GtkNotebook *notebook,
                       GtkNotebookPage *page,
                       guint page_num,
                       gpointer user_data)
{
    plugin_data *data = (plugin_data *) user_data;

    data->config_remember_tab = page_num;
}


static void
setup_units(xfceweather_dialog *dialog,
            const units_config *units)
{
    if (G_UNLIKELY(units == NULL))
        return;

    SET_COMBO_VALUE(dialog->combo_unit_temperature, units->temperature);
    combo_unit_temperature_set_tooltip(dialog->combo_unit_temperature);
    SET_COMBO_VALUE(dialog->combo_unit_pressure, units->pressure);
    combo_unit_pressure_set_tooltip(dialog->combo_unit_pressure);
    SET_COMBO_VALUE(dialog->combo_unit_windspeed, units->windspeed);
    combo_unit_windspeed_set_tooltip(dialog->combo_unit_windspeed);
    SET_COMBO_VALUE(dialog->combo_unit_precipitation, units->precipitation);
    combo_unit_precipitation_set_tooltip(dialog->combo_unit_precipitation);
    SET_COMBO_VALUE(dialog->combo_unit_altitude, units->altitude);
    combo_unit_altitude_set_tooltip(dialog->combo_unit_altitude);
    SET_COMBO_VALUE(dialog->combo_apparent_temperature,
                    units->apparent_temperature);
    combo_apparent_temperature_set_tooltip(dialog->combo_apparent_temperature);
}


void
setup_notebook_signals(xfceweather_dialog *dialog)
{
    /* location page */
    g_signal_connect(GTK_EDITABLE(dialog->text_loc_name), "changed",
                     G_CALLBACK(text_loc_name_changed), dialog);
    g_signal_connect(GTK_SPIN_BUTTON(dialog->spin_lat), "value-changed",
                     G_CALLBACK(spin_lat_value_changed), dialog);
    g_signal_connect(GTK_SPIN_BUTTON(dialog->spin_lon), "value-changed",
                     G_CALLBACK(spin_lon_value_changed), dialog);
    g_signal_connect(GTK_SPIN_BUTTON(dialog->spin_alt), "value-changed",
                     G_CALLBACK(spin_alt_value_changed), dialog);
    g_signal_connect(GTK_EDITABLE(dialog->text_timezone), "changed",
                     G_CALLBACK(text_timezone_changed), dialog);

    /* units page */
    g_signal_connect(dialog->combo_unit_temperature, "changed",
                     G_CALLBACK(combo_unit_temperature_changed), dialog);
    g_signal_connect(dialog->combo_unit_pressure, "changed",
                     G_CALLBACK(combo_unit_pressure_changed), dialog);
    g_signal_connect(dialog->combo_unit_windspeed, "changed",
                     G_CALLBACK(combo_unit_windspeed_changed), dialog);
    g_signal_connect(dialog->combo_unit_precipitation, "changed",
                     G_CALLBACK(combo_unit_precipitation_changed), dialog);
    g_signal_connect(dialog->combo_unit_altitude, "changed",
                     G_CALLBACK(combo_unit_altitude_changed), dialog);
    g_signal_connect(dialog->combo_apparent_temperature, "changed",
                     G_CALLBACK(combo_apparent_temperature_changed), dialog);

    /* appearance page */
    g_signal_connect(G_OBJECT(dialog->button_appearance_font),
                     "button_press_event",
                     G_CALLBACK(button_appearance_font_pressed),
                     dialog);
    g_signal_connect(dialog->button_appearance_font, "clicked",
                     G_CALLBACK(button_appearance_font_clicked), dialog);
    g_signal_connect(G_OBJECT(dialog->button_appearance_color),
                     "button-press-event",
                     G_CALLBACK(button_appearance_color_pressed),
                     dialog);
    g_signal_connect(dialog->button_appearance_color, "color-set",
                     G_CALLBACK(button_appearance_color_set), dialog);
    g_signal_connect(dialog->combo_icon_theme, "changed",
                     G_CALLBACK(combo_icon_theme_changed), dialog);
    g_signal_connect(dialog->button_icons_dir, "clicked",
                     G_CALLBACK(button_icons_dir_clicked), dialog);
    g_signal_connect(dialog->check_single_row, "toggled",
                     G_CALLBACK(check_single_row_toggled), dialog);
    g_signal_connect(dialog->combo_tooltip_style, "changed",
                     G_CALLBACK(combo_tooltip_style_changed), dialog);
    g_signal_connect(dialog->combo_forecast_layout, "changed",
                     G_CALLBACK(combo_forecast_layout_changed), dialog);
    g_signal_connect(GTK_SPIN_BUTTON(dialog->spin_forecast_days),
                     "value-changed",
                     G_CALLBACK(spin_forecast_days_value_changed), dialog);
    g_signal_connect(dialog->check_round_values, "toggled",
                     G_CALLBACK(check_round_values_toggled), dialog);

    /* scrollbox page */
    g_signal_connect(GTK_SPIN_BUTTON(dialog->spin_update_interval),
                     "value-changed",
                     G_CALLBACK(spin_update_interval_value_changed),
                     dialog);

    /* notebook widget */
    gtk_widget_show_all(GTK_WIDGET(dialog->notebook));
    gtk_notebook_set_current_page(GTK_NOTEBOOK(dialog->notebook),
                                  dialog->pd->config_remember_tab);
    g_signal_connect(GTK_NOTEBOOK(dialog->notebook), "switch-page",
                     G_CALLBACK(notebook_page_switched), dialog->pd);
}


xfceweather_dialog *
create_config_dialog(plugin_data *data,
                     GtkWidget *vbox)
{
    xfceweather_dialog *dialog;

    dialog = g_slice_new0(xfceweather_dialog);
    dialog->pd = (plugin_data *) data;
    dialog->dialog = gtk_widget_get_toplevel(vbox);

    dialog->notebook = gtk_notebook_new();
    gtk_notebook_append_page(GTK_NOTEBOOK(dialog->notebook),
                             create_location_page(dialog),
                             gtk_label_new_with_mnemonic(_("_Location")));
    gtk_notebook_append_page(GTK_NOTEBOOK(dialog->notebook),
                             create_units_page(dialog),
                             gtk_label_new_with_mnemonic(_("_Units")));
    gtk_notebook_append_page(GTK_NOTEBOOK(dialog->notebook),
                             create_appearance_page(dialog),
                             gtk_label_new_with_mnemonic(_("_Appearance")));
    gtk_notebook_append_page(GTK_NOTEBOOK(dialog->notebook),
                             create_interval_page(dialog),
                             gtk_label_new_with_mnemonic(_("_Interval")));
    setup_notebook_signals(dialog);
    gtk_box_pack_start(GTK_BOX(vbox), dialog->notebook, TRUE, TRUE, 0);
    gtk_widget_show(GTK_WIDGET(vbox));
    gtk_widget_hide(GTK_WIDGET(dialog->update_spinner));
#if !LIBXFCE4PANEL_CHECK_VERSION(4,9,0)
    gtk_widget_hide(GTK_WIDGET(dialog->check_single_row));
#endif
    gtk_window_set_icon_name(GTK_WINDOW(dialog->dialog), "xfce4-weather");
    
    /* automatically detect current location if it is yet unknown */
    if (!(dialog->pd->lat && dialog->pd->lon))
        start_auto_locate(dialog);

    return dialog;
}
