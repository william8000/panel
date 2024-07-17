/* mate panel backlight control
 *
 * 26Jan10 wb initial version
 * 03Aug10 wb added pointer grab
 * 26Jun12 wb migrated from gnome2 to mate for Fedora 17
 * 25Feb14 wb converted to mate 1.6.2 for Fedora 20
 * 26Feb14 wb use dbus using https://github.com/mate-desktop/mate-power-manager/tree/master/applets/brightness
 * 09Nov22 wb add unicode option
 * 22Nov22 wb clean up for mate 1.26 for Fedora 36, reduce button increments from 5 to 1, fix initialization
 *
 * The dbus interface needs to be able to sudo /usr/sbin/mate-power-backlight-helper
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/unistd.h>
#include <time.h>
#include <ctype.h>
#include <string.h>
#include <stdlib.h>

#include <mate-panel-applet.h>

#include <gtk/gtklabel.h>
#include <gtk/gtkimage.h>
#include <gtk/gtkbox.h>
#include <gdk/gdkx.h>

#include <dbus/dbus-glib.h>

#define VERSION		"22Nov22"

#define	BASE_NAME	"backlight"

#define	BACKLIGHT_FILE	"/sys/devices/virtual/backlight/acpi_video0/brightness"
#define	MAX_BACKLIGHT_LEVEL	15
enum backlight_source_enum {
	BACKLIGHT_SOURCE_UNKNOWN,
	BACKLIGHT_SOURCE_SYS_FILE,
	BACKLIGHT_SOURCE_XBACKLIGHT,
	BACKLIGHT_SOURCE_DBUS,
	NUM_BACKLIGHT_SOURCES
};
static char *backlight_source_names[] = { "UNKNOWN", "SYS_FILE", "XBACKLIGHT", "DBUS", "ERR" };

#define MATE_PANEL_APPLET_VERTICAL(p)	 (((p) == MATE_PANEL_APPLET_ORIENT_LEFT) || ((p) == MATE_PANEL_APPLET_ORIENT_RIGHT))

static int debug = 1;			/* enable debug messages to the log file */
static char *home_dir = NULL;		/* user's home directory */
static char *setup_name = NULL;		/* name of the config file */
static char *status_name = NULL;	/* name of thes status file */
static FILE *log_file = NULL;		/* file for log messages */
static int backlight_level = -1;	/* current backlight level */
static int max_backlight_level = MAX_BACKLIGHT_LEVEL; /* maximum value of raw backlight_level */
static int backlight_level_100 = -1;	/* current backlight level, scaled from 0 to 100 */
static int backlight_level_100_status = -1;	/* level saved in the status file */
static enum backlight_source_enum backlight_source = BACKLIGHT_SOURCE_UNKNOWN; /* source of the backlight information */
static GtkWidget *backlight_slider = NULL;	/* slider */
static GtkWidget *backlight_btn_minus = NULL;	/* minus button of slider */
static GtkWidget *backlight_btn_plus = NULL;	/* plus button of slider */
static GtkWidget *backlight_popup = NULL;	/* popup that contains the slider */
static GtkEventBox *backlight_event_box = NULL;	/* area inside panel */
static int backlight_popped = FALSE;		/* TRUE if the slider is showing */
static gboolean backlight_call_worked = TRUE;	/* TRUE if last attempt to set the brightness worked */
static int do_unicode = 1;			/* show unicode instead of text */
static DBusGConnection *connection = NULL;	/* connection to the power manager */
static DBusGProxy *proxy = NULL;		/* proxy to the power manager */

/* Return a time stamp */

static char *show_time(void)
{
	enum time_buf_enum { TIME_BUF_LEN = 100 };
	static char buf[ TIME_BUF_LEN ];
	struct tm *tm_ptr;
	time_t time_val;

	time_val = time(NULL);
	tm_ptr = localtime(&time_val);

	buf[0] = '\0';

	strftime(buf, TIME_BUF_LEN, "%e %b %Y %H:%M:%S", tm_ptr);

	buf[ TIME_BUF_LEN - 1 ] = '\0';

	return buf;
}

/* Exit after a serious error */

static void
exit_backlight(void)
{
	if (log_file != NULL) {
		fprintf(log_file, "Exiting at %s.\n", show_time());
		fclose(log_file);
		log_file = NULL;
	}

	exit(1);
}

/* Write the backlight status file */

static void
write_backlight_status()
{
	FILE *status_file;

	if (backlight_level_100_status == backlight_level_100) {
		if (debug && log_file != NULL) {
			fprintf(log_file, "Not writing status, level %d unchanged.\n", backlight_level_100);
		}
	} else {
		status_file = fopen(status_name, "w");
		if (status_file == NULL) {
			if (log_file != NULL) {
				fprintf(log_file, "Could not update status file '%s'\n", status_name);
			}
		} else {
			fprintf(status_file, "level %d\n", backlight_level_100);
			fprintf(status_file, "maxrawlevel %d\n", max_backlight_level);
			fclose(status_file);
			backlight_level_100_status = backlight_level_100;
			if (debug && log_file != NULL) {
				fprintf(log_file, "Updated status to %d.\n", backlight_level_100);
			}
		}
	}
}

/**
 * backlight_applet_get_brightness_with_dbus:
 * Return value: Success value, or zero for failure
 **/
static gboolean
backlight_applet_get_brightness_with_dbus (MatePanelApplet *applet, int *brightness)
{
	GError  *error = NULL;
	gboolean ret;
	guint policy_brightness;

	if (proxy == NULL) {
		if (log_file != NULL) {
			fprintf(log_file, "backlight_applet_get_brightness_with_dbus, error, not connected\n");
		}
		return FALSE;
	}

	ret = dbus_g_proxy_call (proxy, "GetBrightness", &error,
				 G_TYPE_INVALID,
				 G_TYPE_UINT, &policy_brightness,
				 G_TYPE_INVALID);

	if (error) {
		if (log_file != NULL) {
			fprintf(log_file, "backlight_applet_get_brightness_with_dbus, error, %s\n", error->message);
		}
		g_error_free (error);
	}

	if (ret) {
		*brightness = policy_brightness;
		if (debug && log_file != NULL) {
			fprintf(log_file, "backlight_applet_get_brightness_with_dbus, got %d\n", policy_brightness);
		}
	} else {
		*brightness = 0;
		if (log_file != NULL) {
			fprintf(log_file, "backlight_applet_get_brightness_with_dbus, GetBrightness failed!\n");
		}
	}

	return ret;
}

/**
 * backlight_applet_set_brightness_with_dbus:
 * Return value: Success value, or zero for failure
 **/
static gboolean
backlight_applet_set_brightness_with_dbus (MatePanelApplet *applet, int brightness)
{
	GError  *error = NULL;
	gboolean ret;

	if (debug && log_file) {
		fprintf(log_file, "backlight_applet_set_brightness_with_dbus, brightness %d\n", brightness);
	}

	if (proxy == NULL) {
		if (log_file != NULL) {
			fprintf(log_file, "backlight_applet_set_brightness_with_dbus, error, not connected\n");
		}
		return FALSE;
	}

	ret = dbus_g_proxy_call (proxy, "SetBrightness", &error,
				 G_TYPE_UINT, (guint) brightness,
				 G_TYPE_INVALID,
				 G_TYPE_INVALID);

	if (error) {
		if (log_file != NULL) {
			fprintf(log_file, "backlight_applet_set_brightness_with_dbus, error, %s\n", error->message);
		}
		g_error_free (error);
	}

	if (!ret) {
		if (log_file != NULL) {
			fprintf(log_file, "backlight_applet_set_brightness_with_dbus, error, SetBrightness failed!\n");
		}
	}

	return ret;
}

/* Get the backlight level */

static int
get_backlight_level(MatePanelApplet *applet)
{
	FILE *backlight_file;
	enum backlight_line_len_enum { BACKLIGHT_LINE_LEN = 80 };
	char backlight_line[ BACKLIGHT_LINE_LEN ];

	backlight_level = -1;
	max_backlight_level = MAX_BACKLIGHT_LEVEL;
	backlight_source = BACKLIGHT_SOURCE_UNKNOWN;
	backlight_file = NULL;

	if (backlight_applet_get_brightness_with_dbus(applet, &backlight_level)) {
		max_backlight_level = 100;
		backlight_source = BACKLIGHT_SOURCE_DBUS;
	}

	if (backlight_level < 0 && backlight_file == NULL) {
		backlight_file = fopen(BACKLIGHT_FILE, "r");
		max_backlight_level = MAX_BACKLIGHT_LEVEL;
		backlight_source = BACKLIGHT_SOURCE_SYS_FILE;
	}

	if (backlight_level < 0 && backlight_file == NULL) {
		backlight_file = popen("xbacklight -get", "r");
		max_backlight_level = 100;
		backlight_source = BACKLIGHT_SOURCE_XBACKLIGHT;
	}

	if (backlight_level >= 0) {

		/* nothing to do */

	} else if (!backlight_file) {

		if (log_file != NULL) {
			fprintf(log_file, "Could not read backlight file '%s'.\n", BACKLIGHT_FILE);
		}

	} else {

		backlight_line[0] = '\0';

		if (fgets(backlight_line, BACKLIGHT_LINE_LEN, backlight_file) == NULL || backlight_line[0] == '\0') {
			if (log_file != NULL) {
				fprintf(log_file, "Error reading from backlight file '%s'.\n", BACKLIGHT_FILE);
			}
		} else {
			backlight_level = atoi(backlight_line);
			if (backlight_level < 0) backlight_level = 0;
			if (backlight_level > max_backlight_level) backlight_level = max_backlight_level;
		}

		if (backlight_source == BACKLIGHT_SOURCE_XBACKLIGHT) {
			pclose(backlight_file);
		} else if (backlight_source == BACKLIGHT_SOURCE_SYS_FILE) {
			fclose(backlight_file);
		} else {
			if (log_file != NULL) {
				fprintf(log_file, "get_backlight_level, warning, unexpected backlight source %d\n", backlight_source);
			}
		}
	}

	if (debug && log_file != NULL) {
		fprintf(log_file,
		  "read backlight level %d of %d from source %d %s at %s\n",
		  backlight_level, max_backlight_level, backlight_source, backlight_source_names[ backlight_source ], show_time());
	}

	if (backlight_level_100 < 0) {
		if (max_backlight_level == 100) {
			backlight_level_100 = backlight_level;
		} else {
			backlight_level_100 = ((backlight_level + 1) * 100) / (max_backlight_level + 1);
		}
	}

	return backlight_level;
}

/* Set the brightness */

static gboolean
backlight_applet_set_brightness (MatePanelApplet *applet)
{
	FILE *backlight_file;
	int new_backlight_level;
	gboolean ok = TRUE;

	if (debug && log_file) {
		fprintf(log_file, "Request to set brightness to %d%% of %d\n", backlight_level_100, max_backlight_level);
		fflush(log_file);
	}

	if (backlight_level_100 < 0) backlight_level_100 = 0;
	if (backlight_level_100 > 100) backlight_level_100 = 100;
	if (max_backlight_level == 100) {
		new_backlight_level = backlight_level_100;
	} else {
		new_backlight_level = ((backlight_level_100 * (max_backlight_level + 1)) / 100) - 1;
	}
	if (new_backlight_level < 0) new_backlight_level = 0;
	if (new_backlight_level > max_backlight_level) new_backlight_level = max_backlight_level;

	if (backlight_source == BACKLIGHT_SOURCE_UNKNOWN || backlight_source == BACKLIGHT_SOURCE_DBUS) {
		if (backlight_applet_set_brightness_with_dbus(applet, new_backlight_level)) {
			backlight_source = BACKLIGHT_SOURCE_DBUS;
		}
	}

	if (backlight_source == BACKLIGHT_SOURCE_UNKNOWN) {
		backlight_source = ((access(BACKLIGHT_FILE, R_OK) == 0)? BACKLIGHT_SOURCE_SYS_FILE: BACKLIGHT_SOURCE_XBACKLIGHT);
	}

	if (backlight_source == BACKLIGHT_SOURCE_SYS_FILE) {
		backlight_file = fopen(BACKLIGHT_FILE, "w");
		if (!backlight_file) {
			ok = FALSE;
			if (debug && log_file) {
				fprintf(log_file, "Could not open '%s' for writing.\n", BACKLIGHT_FILE);
			}
		} else {
			ok = (fprintf(backlight_file, "%d", new_backlight_level));

			if (!ok && log_file) {
				fprintf(log_file, "Error writing to '%s'.\n", BACKLIGHT_FILE);
			}

			fclose(backlight_file);
		}
	} else if (backlight_source == BACKLIGHT_SOURCE_XBACKLIGHT) {
		char line[ 100 ];
		int i;
		sprintf(line, "xbacklight -time 0 -set %d", new_backlight_level);
		i = system(line);
		if (log_file != NULL) {
			if (debug) {
				fprintf(log_file, "Set command '%s' returned %d\n", line, i);
			}
			if (i != 0) {
				fprintf(log_file, "Could not update brightness. '%s' returned %d\n", line, i);
			}
		}
	}

	return ok;
}

/* Read the setup file */

static void
read_setup_file(const char *name)
{
	FILE *setup_file;
	enum setup_enum { ID_LEN = 50, BUF_LEN = 1024 };
	char id[ ID_LEN ];
	char buf[ BUF_LEN ];
	int len;
	int ch;

	setup_file = fopen(name, "r");

	if (setup_file == NULL) {
		if (log_file != NULL) {
			fprintf(log_file, "Could not open setup file %s.\n", setup_name);
		}
		return;
	}

	ch = fgetc(setup_file);

	while (ch != EOF) {
		while (ch == ' ' || ch == '\t') ch = fgetc(setup_file);

		len = 0;
		while (ch != ' ' && ch != '\t' && ch != '\n' && ch != EOF) {
			if (len < ID_LEN-1) id[ len++ ] = (char) ch;
			ch = fgetc(setup_file);
		}
		id[ len ] = '\0';

		while (ch == ' ' || ch == '\t') ch = fgetc(setup_file);

		len = 0;
		while (ch != '\n' && ch != EOF) {
			if (len < BUF_LEN-1) buf[ len++ ] = (char) ch;
			ch = fgetc(setup_file);
		}
		buf[ len ] = '\0';

		if (ch == '\n') ch = fgetc(setup_file);

		if (id[0] == '\0') {
			/* ignore blank lines */
		} else if (id[0] == '#') {
			/* ignore comments */
		} else if (strcmp(id, "level") == 0) {
			if (len == 0 || !isdigit(buf[0])) {
				if (log_file != NULL) {
					fprintf(log_file, "Setup file '%s' has 'level' without numeric value.\n", name);
				}
			} else {
				backlight_level_100 = atoi(buf);
				if (backlight_level_100 < 0) backlight_level_100 = 0;
				if (backlight_level_100 > 100) backlight_level_100 = 100;
				if (log_file != NULL) fprintf(log_file, "Set initial level to %d%%.\n", backlight_level_100);
			}
		} else if (strcmp(id, "maxrawlevel") == 0) {
			if (len == 0 || !isdigit(buf[0])) {
				if (log_file != NULL) {
					fprintf(log_file, "Setup file '%s' has 'maxrawlevel' without numeric value.\n", name);
				}
			} else {
				max_backlight_level = atoi(buf);
				if (max_backlight_level <= 0) max_backlight_level = MAX_BACKLIGHT_LEVEL;
				if (log_file != NULL) fprintf(log_file, "Set max raw level to %d.\n", max_backlight_level);
			}
		} else if (strcmp(id, "unicode") == 0) {
			do_unicode = ((len == 0 || (buf[0] >= '1' && buf[0] <= '9') || buf[0] == 'y' || buf[0] == 't')? 1: 0);
		} else if (strcmp(id, "debug") == 0) {
			if (len == 0 || !isdigit(buf[0])) {
				if (log_file != NULL) {
					fprintf(log_file, "Setup file '%s' has 'debug' without numeric value.\n", name);
				}
			} else {
				debug = atoi(buf);
				if (log_file != NULL) fprintf(log_file, "Set debug level to %d.\n", debug);
			}
		} else {
			if (log_file != NULL)
				fprintf(log_file, "Setup file '%s' has unknown option '%s'.\n", name, id);
		}
	}

	fclose(setup_file);

	if (log_file != NULL && (debug || strcmp(name, setup_name) == 0)) {
		fprintf(log_file, "Read setup file '%s' at %s.\n", name, show_time());
		fprintf(log_file, " backlight level %d\n", backlight_level_100);
		fprintf(log_file, " max raw level %d\n", max_backlight_level);
		fprintf(log_file, " unicode %d\n", do_unicode);
		fprintf(log_file, " debug level %d\n", debug);
		fflush(log_file);
	}
}

/* Set the tooltip */

static void
backlight_applet_update_tooltip (MatePanelApplet *applet)
{
	gchar *buf = NULL;
	if (!backlight_popped) {
		gtk_widget_set_tooltip_text (GTK_WIDGET(applet), NULL);
	} else {
		buf = g_strdup_printf ("LCD brightness : %d%%", backlight_level_100);
		gtk_widget_set_tooltip_text (GTK_WIDGET(applet), buf);
	}
	g_free (buf);
}

/* Draw the applet content (background + icon) */

static gboolean
backlight_applet_draw_cb (MatePanelApplet *applet)
{
	static GtkWidget *last_label = NULL;
	static int last_backlight_level_100 = -2;
	char backlight_message[ 80 ];

	if (debug && log_file != NULL) {
		fprintf(log_file, "draw cb, old level %d new level %d at %s\n",
			last_backlight_level_100, backlight_level_100,
			show_time());
	}

	if (backlight_level_100 != last_backlight_level_100) {
		if (last_label != NULL) {
			gtk_container_remove (GTK_CONTAINER (backlight_event_box), last_label);
			last_label = NULL;
			if (debug && log_file != NULL) {
				fprintf(log_file, "remove label for level %d\n", last_backlight_level_100);
			}
		}

		last_backlight_level_100 = backlight_level_100;

		if (last_backlight_level_100 >= 0) {
			sprintf(backlight_message,
				"%s %d%%",
				(do_unicode? "\xE2\x98\x80": "Backlight"),
				last_backlight_level_100);
		} else {
			sprintf(backlight_message, "Backlight unknown");
		}

		if (last_backlight_level_100 >= 0) {
			last_label = gtk_label_new (backlight_message);
			gtk_container_add (GTK_CONTAINER (backlight_event_box), last_label);
			if (debug && log_file != NULL) {
				fprintf(log_file, "add label for level %d\n", last_backlight_level_100);
				fflush(log_file);
			}
		}
		gtk_widget_show_all ( GTK_WIDGET(backlight_event_box) );
	}
	return TRUE;
}

/* Update the brightness */

static void
backlight_applet_update_popup_level (MatePanelApplet *applet)
{
	if (backlight_popup != NULL) {
		gtk_widget_set_sensitive (backlight_btn_plus, backlight_level_100 < 100);
		gtk_widget_set_sensitive (backlight_btn_minus, backlight_level_100 > 0);
		gtk_range_set_value (GTK_RANGE(backlight_slider), (guint) backlight_level_100);
	}
	backlight_applet_update_tooltip (applet);
}

/* callback for the plus button */

static gboolean
backlight_applet_plus_cb (GtkWidget *w, MatePanelApplet *applet)
{
	backlight_level_100 += 1;
	if (backlight_level_100 > 100) {
		backlight_level_100 = 100;
	}
	backlight_call_worked = backlight_applet_set_brightness (applet);
	backlight_applet_update_popup_level (applet);
	backlight_applet_draw_cb (applet);
	return TRUE;
}

/* callback for the minus button */

static gboolean
backlight_applet_minus_cb (GtkWidget *w, MatePanelApplet *applet)
{
	backlight_level_100 -= 1;
	if (backlight_level_100 < 0) {
		backlight_level_100 = 0;
	}
	backlight_call_worked = backlight_applet_set_brightness (applet);
	backlight_applet_update_popup_level (applet);
	backlight_applet_draw_cb (applet);
	return TRUE;
}

/* callback for the slider */

static gboolean
backlight_applet_slide_cb (GtkWidget *w, MatePanelApplet *applet)
{
	backlight_level_100 = gtk_range_get_value (GTK_RANGE(backlight_slider));
	backlight_call_worked = backlight_applet_set_brightness (applet);
	backlight_applet_update_popup_level (applet);
	backlight_applet_draw_cb (applet);
	return TRUE;
}

#if 0
/* callback for key presses */

static gboolean
backlight_applet_key_press_cb (GtkWidget *popup, GdkEventKey *event, MatePanelApplet *applet)
{
	switch (event->keyval) {
	case GDK_KEY_KP_Enter:
	case GDK_KEY_ISO_Enter:
	case GDK_KEY_3270_Enter:
	case GDK_KEY_Return:
	case GDK_KEY_space:
	case GDK_KEY_KP_Space:
	case GDK_KEY_Escape:
		/* if yet popped, hide */
		if (backlight_popped) {
			gtk_widget_hide (backlight_popup);
			backlight_popped = FALSE;
			backlight_applet_draw_cb (applet);
			backlight_applet_update_tooltip (applet);
			return TRUE;
		} else {
			return FALSE;
		}
		break;
	case GDK_KEY_Page_Up:
		backlight_applet_plus_cb (NULL, applet);
		return TRUE;
	case GDK_KEY_Left:
	case GDK_KEY_Up:
		backlight_applet_plus_cb (NULL, applet);
		return TRUE;
	case GDK_KEY_Page_Down:
		backlight_applet_minus_cb (NULL, applet);
		return TRUE;
	case GDK_KEY_Right:
	case GDK_KEY_Down:
		backlight_applet_minus_cb (NULL, applet);
		return TRUE;
	default:
		break;
	}

	return FALSE;
}
#endif

/* Create the popup */

static void
backlight_applet_create_popup (MatePanelApplet *applet)
{
	GtkWidget *box, *frame;
	gint orientation = mate_panel_applet_get_orient (MATE_PANEL_APPLET (MATE_PANEL_APPLET (applet)));

	if (backlight_popup) {
		return;
	}

	/* slider */
	if (MATE_PANEL_APPLET_VERTICAL(orientation)) {
		backlight_slider = gtk_scale_new_with_range (GTK_ORIENTATION_HORIZONTAL, 0, 100, 1);
		gtk_widget_set_size_request (backlight_slider, 100, -1);
	} else {
		backlight_slider = gtk_scale_new_with_range (GTK_ORIENTATION_VERTICAL, 0, 100, 1);
		gtk_widget_set_size_request (backlight_slider, -1, 100);
	}

	gtk_range_set_inverted (GTK_RANGE(backlight_slider), TRUE);
	gtk_scale_set_draw_value (GTK_SCALE(backlight_slider), FALSE);
	gtk_range_set_value (GTK_RANGE(backlight_slider), ((backlight_level + 1) * 100) / (MAX_BACKLIGHT_LEVEL + 1));
	g_signal_connect (G_OBJECT(backlight_slider), "value-changed", G_CALLBACK(backlight_applet_slide_cb), applet);

	/* minus button */
	backlight_btn_minus = gtk_button_new_with_label ("\342\210\222"); /* U+2212 MINUS SIGN */
	gtk_button_set_relief (GTK_BUTTON(backlight_btn_minus), GTK_RELIEF_NONE);
	g_signal_connect (G_OBJECT(backlight_btn_minus), "pressed", G_CALLBACK(backlight_applet_minus_cb), applet);

	/* plus button */
	backlight_btn_plus = gtk_button_new_with_label ("+");
	gtk_button_set_relief (GTK_BUTTON(backlight_btn_plus), GTK_RELIEF_NONE);
	g_signal_connect (G_OBJECT(backlight_btn_plus), "pressed", G_CALLBACK(backlight_applet_plus_cb), applet);

	/* box */
	if (MATE_PANEL_APPLET_VERTICAL(orientation)) {
		box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 1);
	} else {
		box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 1);
	}
	gtk_box_pack_start (GTK_BOX(box), backlight_btn_plus, FALSE, FALSE, 0);
	gtk_box_pack_start (GTK_BOX(box), backlight_slider, TRUE, TRUE, 0);
	gtk_box_pack_start (GTK_BOX(box), backlight_btn_minus, FALSE, FALSE, 0);

	/* frame */
	frame = gtk_frame_new (NULL);
	gtk_frame_set_shadow_type (GTK_FRAME(frame), GTK_SHADOW_OUT);
	gtk_container_add (GTK_CONTAINER(frame), box);

	/* window */
	backlight_popup = gtk_window_new (GTK_WINDOW_POPUP);
	gtk_window_set_type_hint (GTK_WINDOW(backlight_popup), GDK_WINDOW_TYPE_HINT_UTILITY);
	gtk_widget_set_parent (backlight_popup, GTK_WIDGET(applet));
	gtk_container_add (GTK_CONTAINER(backlight_popup), frame);

#if 0
	/* window events */
	g_signal_connect (G_OBJECT(backlight_popup), "key-press-event",
			  G_CALLBACK(backlight_applet_key_press_cb), applet);
#endif
}

/* Update the status displayed in the panel */

static gboolean
open_window (MatePanelApplet *applet)
{
	get_backlight_level(applet);

	backlight_applet_draw_cb(applet);

	if (backlight_popped) {

		/* gdk_keyboard_ungrab (GDK_CURRENT_TIME); */
		/* gdk_pointer_ungrab (GDK_CURRENT_TIME); */
		/* gtk_grab_remove (GTK_WIDGET(applet)); */
		/* gtk_widget_set_state (GTK_WIDGET(applet), GTK_STATE_NORMAL); */

		gtk_widget_hide (backlight_popup);

		backlight_popped = FALSE;

		backlight_applet_draw_cb (applet);
		backlight_applet_update_tooltip (applet);

		write_backlight_status();

	} else {

		GtkAllocation allocation, popup_allocation;
		gint orientation, x, y;

		/* reset the level on each pop-up in case another application changed it */
		if (backlight_level_100 >= 0) {
			backlight_applet_set_brightness(applet);
		}

		if (backlight_popup == NULL) {
			backlight_applet_create_popup (applet);
		}

		backlight_applet_update_popup_level(applet);
		gtk_widget_show_all (backlight_popup);
		backlight_popped = TRUE;

		/* move the window near the applet */
		orientation = mate_panel_applet_get_orient (MATE_PANEL_APPLET (MATE_PANEL_APPLET (applet)));
		gdk_window_get_origin (gtk_widget_get_window (GTK_WIDGET(applet)), &x, &y);

		gtk_widget_get_allocation (GTK_WIDGET (applet), &allocation);
		gtk_widget_get_allocation (GTK_WIDGET (backlight_popup), &popup_allocation);
		switch (orientation) {
		case MATE_PANEL_APPLET_ORIENT_DOWN:
			x += allocation.x + allocation.width/2;
			y += allocation.y + allocation.height;
			x -= popup_allocation.width/2;
			break;
		case MATE_PANEL_APPLET_ORIENT_UP:
			x += allocation.x + allocation.width/2;
			y += allocation.y;
			x -= popup_allocation.width/2;
			y -= popup_allocation.height;
			break;
		case MATE_PANEL_APPLET_ORIENT_RIGHT:
			y += allocation.y + allocation.height/2;
			x += allocation.x + allocation.width;
			y -= popup_allocation.height/2;
			break;
		case MATE_PANEL_APPLET_ORIENT_LEFT:
			y += allocation.y + allocation.height/2;
			x += allocation.x;
			x -= popup_allocation.width;
			y -= popup_allocation.height/2;
			break;
		default:
			g_assert_not_reached ();
		}

		gtk_window_move (GTK_WINDOW (backlight_popup), x, y);

		/* grab input */

		/* focus not needed */
		/* gtk_widget_grab_focus (GTK_WIDGET(applet)); */
		/* gtk_grab_add (GTK_WIDGET(applet)); */
		/* takes control of the pointer and does not let the slider get anything */
		/* gdk_pointer_grab (gtk_widget_get_window (GTK_WIDGET(applet)), TRUE,
				  GDK_BUTTON_PRESS_MASK |
				  GDK_BUTTON_RELEASE_MASK |
				  GDK_POINTER_MOTION_MASK,
				  NULL, NULL, GDK_CURRENT_TIME); */
		/* keyboard grab not needed */
		/* gdk_keyboard_grab (gtk_widget_get_window (GTK_WIDGET(applet)), TRUE, GDK_CURRENT_TIME); */
		/* makes the text blue when selected */
		/* gtk_widget_set_state (GTK_WIDGET(applet), GTK_STATE_SELECTED); */
	}

	return TRUE;
}

/* Handle a left click on the panel */
/*   Reload the setup file and update the panel */

static gboolean
on_button_press (MatePanelApplet *applet,
		GdkEventButton   *event,
		gpointer	  data)
{
	/* Don't react to anything other than the left mouse button;
	   return FALSE so the event is passed to the default handler */

	if (event->button != 1)
		return FALSE;

	read_setup_file(setup_name);

	return open_window(applet);
}

/* Main entry point of the applet */
/*   Initialize from the environment */
/*   Set up global variables */

static gboolean
backlight_applet_fill (MatePanelApplet *applet,
	const gchar *iid,
	gpointer data)
{
	int log_len;
	int setup_len;
	int status_len;
	char *log_name;
	GError *gerror;

	home_dir = getenv("HOME");
	if (home_dir == NULL) {
		home_dir = "/tmp";
	}
	log_len = strlen(home_dir) + strlen(BASE_NAME) + 10;
	log_name = malloc(log_len);
	if (!log_name) {
		exit_backlight();
	}

	sprintf(log_name, "%s/.%s.log", home_dir, BASE_NAME);

	log_file = fopen(log_name, "w");

	if (!log_file) {
		exit_backlight();
	}

	if (strcmp(iid, "BacklightApplet") != 0) {
		fprintf(log_file, "Backlight not starting, iid is %s\n", iid);
		fflush(log_file);
		return FALSE;
	}

	fprintf(log_file, "Starting %s Version %s at %s...\n", BASE_NAME, VERSION, show_time());
	fflush(log_file);

	setup_len = strlen(home_dir) + strlen(BASE_NAME) + 10;
	setup_name = malloc(setup_len);
	if (!setup_name) {
		fprintf(log_file, "Could not allocate setup name.\n");
		exit_backlight();
	}

	sprintf(setup_name, "%s/.%src", home_dir, BASE_NAME);

	read_setup_file(setup_name);

	status_len = strlen(home_dir) + strlen(BASE_NAME) + 10;
	status_name = malloc(status_len);
	if (!status_name) {
		fprintf(log_file, "Could not allocate status name.\n");
		exit_backlight();
	}

	sprintf(status_name, "%s/.%s.stat", home_dir, BASE_NAME);

	read_setup_file(status_name);

	backlight_level_100_status = backlight_level_100;

	free(log_name);
	log_name = NULL;

	/* create a dbus connection */

#define GPM_DBUS_SERVICE		"org.mate.PowerManager"
#define GPM_DBUS_PATH_BACKLIGHT		"/org/mate/PowerManager/Backlight"
#define GPM_DBUS_INTERFACE_BACKLIGHT	"org.mate.PowerManager.Backlight"

	gerror = NULL;
	connection = dbus_g_bus_get(DBUS_BUS_SESSION, &gerror);
	if (connection == NULL) {
		fprintf(log_file, "Failed to open connection to bus: %s\n", gerror->message);
		g_error_free (gerror);
	}

	proxy = NULL;

	if (connection) {
		proxy = dbus_g_proxy_new_for_name (connection,
						   GPM_DBUS_SERVICE,
						   GPM_DBUS_PATH_BACKLIGHT,
						   GPM_DBUS_INTERFACE_BACKLIGHT);
	}

	/* restore the saved brightness */

	if (debug && log_file) {
		fprintf(log_file, "Before initial set brightness, level %d\n", backlight_level_100);
	}

	if (backlight_level_100 >= 0) {
		backlight_applet_set_brightness(applet);
	}

	backlight_event_box = (GtkEventBox *) gtk_event_box_new ();

	/* open_window(backlight_event_box); */

	gtk_container_add (GTK_CONTAINER (applet), GTK_WIDGET (backlight_event_box) );
	gtk_widget_show_all (GTK_WIDGET (applet));

	g_signal_connect (G_OBJECT (applet),
			"button_press_event",
			G_CALLBACK (on_button_press),
			NULL);


	get_backlight_level(applet);

	backlight_applet_draw_cb(applet);

	return TRUE;
}

/* Factory to interface with the server */

#if 1

/* mate 4 */

static gboolean
backlight_applet_factory (MatePanelApplet *applet,
			  const char  *iid,
			  gpointer     data)
{
	gboolean retval = FALSE;

	if (!strcmp (iid, "BacklightApplet"))
		retval = backlight_applet_fill (applet, iid, data);

	return retval;
}


MATE_PANEL_APPLET_OUT_PROCESS_FACTORY ("BacklightAppletFactory",
			PANEL_TYPE_APPLET,
			"BacklightApplet",
			backlight_applet_factory,
			NULL);
#else

/* mate 2 */

MATE_PANEL_APPLET_MATECOMPONENT_FACTORY ("OAFIID:Backlight_Factory",
			     PANEL_TYPE_APPLET,
			     "The Backlight Applet",
			     "0",
			     backlight_applet_fill,
			     NULL);
#endif
