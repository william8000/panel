/* gnome panel backlight control
 *
 * 26Jan10 wb initial version
 * 03Aug10 wb added pointer grab
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/unistd.h>
#include <time.h>
#include <string.h>
#include <stdlib.h>

#include <panel-applet.h>
#include <gtk/gtklabel.h>
#include <gtk/gtkimage.h>
#include <gtk/gtkbox.h>
#include <gdk/gdkx.h>

#define VERSION		"03Aug10"

#define	BASE_NAME	"backlight"

#define	BACKLIGHT_FILE	"/sys/devices/virtual/backlight/acpi_video0/brightness"

#define	MAX_BACKLIGHT_LEVEL	15

#define PANEL_APPLET_VERTICAL(p)	 (((p) == PANEL_APPLET_ORIENT_LEFT) || ((p) == PANEL_APPLET_ORIENT_RIGHT))

static int debug = 0;			/* enable debug messages to the log file */
static char *home_dir = NULL;		/* user's home directory */
static char *setup_name = NULL;		/* name of the config file */
static char *status_name = NULL;	/* name of thes status file */
static FILE *log_file = NULL;		/* file for log messages */
static int backlight_level = -1;	/* current backlight level */
static int backlight_level_100 = -1;	/* current backlight level, scaled from 0 to 100 */
static int backlight_level_100_status = -1;	/* level saved in the status file */
/* static PanelApplet *backlight_applet = NULL;	*/ /* applet */
static GtkWidget *backlight_slider = NULL;	/* slider */
static GtkWidget *backlight_btn_minus = NULL;	/* minus button of slider */
static GtkWidget *backlight_btn_plus = NULL;	/* plus button of slider */
static GtkWidget *backlight_popup = NULL;	/* popup that contains the slider */
static GtkEventBox *backlight_event_box = NULL;	/* area inside panel */
static int backlight_popped = FALSE;		/* TRUE if the slider is showing */
static gboolean backlight_call_worked = TRUE;	/* TRUE if last attempt to set the brightness worked */

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
			fclose(status_file);
			backlight_level_100_status = backlight_level_100;
			if (debug && log_file != NULL) {
				fprintf(log_file, "Updated status to %d.\n", backlight_level_100);
			}
		}
	}
}

/* Get the backlight level */

static int
get_backlight_level()
{
	FILE *backlight_file;
	enum backlight_line_len_enum { BACKLIGHT_LINE_LEN = 80 };
	char backlight_line[ BACKLIGHT_LINE_LEN ];

	backlight_level = -1;

	backlight_file = fopen(BACKLIGHT_FILE, "r");

	if (!backlight_file) {
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
			if (backlight_level > MAX_BACKLIGHT_LEVEL) backlight_level = MAX_BACKLIGHT_LEVEL;
		}

		fclose(backlight_file);
	}

	if (debug && log_file != NULL) {
		fprintf(log_file, "read backlight level %d at %s\n", backlight_level, show_time());
	}

	if (backlight_level_100 < 0) {
		backlight_level_100 = ((backlight_level + 1) * 100) / (MAX_BACKLIGHT_LEVEL + 1);
	}

	return backlight_level;
}

/* Set the brightness */

static gboolean
backlight_applet_set_brightness (PanelApplet *applet)
{
	FILE *backlight_file;
	int new_backlight_level;
	gboolean ok;

	if (debug && log_file) {
		fprintf(log_file, "Request to set brightness to %d%%\n", backlight_level_100);
		fflush(log_file);
	}

	backlight_file = fopen(BACKLIGHT_FILE, "w");
	if (!backlight_file) {
		ok = FALSE;
		if (debug && log_file) {
			fprintf(log_file, "Could not open '%s' for writing.\n", BACKLIGHT_FILE);
		}
	} else {
		if (backlight_level_100 < 0) backlight_level = 0;
		if (backlight_level_100 > 100) backlight_level = 100;
		new_backlight_level = ((backlight_level_100 * (MAX_BACKLIGHT_LEVEL + 1)) / 100) - 1;
		if (new_backlight_level < 0) new_backlight_level = 0;
		if (new_backlight_level > MAX_BACKLIGHT_LEVEL) new_backlight_level = MAX_BACKLIGHT_LEVEL;

		ok = (fprintf(backlight_file, "%d", new_backlight_level));

		if (!ok && log_file) {
			fprintf(log_file, "Error writing to '%s'.\n", BACKLIGHT_FILE);
		}

		fclose(backlight_file);
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
	int i, len;
	int ch;
	char *str;

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
					fprintf(log_file, "Setup file '%s' has 'debug' without numeric value.\n", name);
				}
			} else {
				backlight_level_100 = atoi(buf);
				if (backlight_level_100 < 0) backlight_level_100 = 0;
				if (backlight_level_100 > 100) backlight_level_100 = 100;
				if (log_file != NULL) fprintf(log_file, "Set initial level to %d%%.\n", backlight_level_100);
			}
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
		fprintf(log_file, " debug level %d\n", debug);
		fflush(log_file);
	}
}

/* Set the tooltip */

static void
backlight_applet_update_tooltip (PanelApplet *applet)
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
backlight_applet_draw_cb (PanelApplet *applet)
{
	static GtkWidget *last_label = NULL;
	static int last_backlight_level_100 = -2;
	char backlight_message[ 80 ];
	static GtkWidget *window = NULL;

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
			sprintf(backlight_message, "Backlight %d%%", last_backlight_level_100);
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
}

/* Update the brightness */

static void
backlight_applet_update_popup_level (PanelApplet *applet)
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
backlight_applet_plus_cb (GtkWidget *w, PanelApplet *applet)
{
	backlight_level_100 += 5;
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
backlight_applet_minus_cb (GtkWidget *w, PanelApplet *applet)
{
	backlight_level_100 -= 5;
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
backlight_applet_slide_cb (GtkWidget *w, PanelApplet *applet)
{
	backlight_level_100 = gtk_range_get_value (GTK_RANGE(backlight_slider));
	backlight_call_worked = backlight_applet_set_brightness (applet);
	backlight_applet_update_popup_level (applet);
	backlight_applet_draw_cb (applet);
	return TRUE;
}

/* Create the popup */

static void
backlight_applet_create_popup (PanelApplet *applet)
{
	GtkWidget *box, *frame;
	gint orientation = panel_applet_get_orient (PANEL_APPLET (PANEL_APPLET (applet)));

	if (backlight_popup) {
		return;
	}

	/* slider */
	if (PANEL_APPLET_VERTICAL(orientation)) {
		backlight_slider = gtk_hscale_new_with_range (0, 100, 1);
		gtk_widget_set_size_request (backlight_slider, 100, -1);
	} else {
		backlight_slider = gtk_vscale_new_with_range (0, 100, 1);
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
	if (PANEL_APPLET_VERTICAL(orientation)) {
		box = gtk_hbox_new (FALSE, 1);
	} else {
		box = gtk_vbox_new (FALSE, 1);
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
}

/* Update the status displayed in the panel */

static gboolean
open_window (PanelApplet *applet)
{
	static GtkWidget *last_label = NULL;
	static int last_backlight_level = -2;
	int backlight_level;
	char backlight_message[ 80 ];
	static GtkWidget *window = NULL;

	get_backlight_level();

	backlight_applet_draw_cb(applet);

	if (backlight_popped) {

		gdk_keyboard_ungrab (GDK_CURRENT_TIME);
		gdk_pointer_ungrab (GDK_CURRENT_TIME);
		gtk_grab_remove (GTK_WIDGET(applet));
		gtk_widget_set_state (GTK_WIDGET(applet), GTK_STATE_NORMAL);

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
		orientation = panel_applet_get_orient (PANEL_APPLET (PANEL_APPLET (applet)));
		gdk_window_get_origin (gtk_widget_get_window (GTK_WIDGET(applet)), &x, &y);

		gtk_widget_get_allocation (GTK_WIDGET (applet), &allocation);
		gtk_widget_get_allocation (GTK_WIDGET (backlight_popup), &popup_allocation);
		switch (orientation) {
		case PANEL_APPLET_ORIENT_DOWN:
			x += allocation.x + allocation.width/2;
			y += allocation.y + allocation.height;
			x -= popup_allocation.width/2;
			break;
		case PANEL_APPLET_ORIENT_UP:
			x += allocation.x + allocation.width/2;
			y += allocation.y;
			x -= popup_allocation.width/2;
			y -= popup_allocation.height;
			break;
		case PANEL_APPLET_ORIENT_RIGHT:
			y += allocation.y + allocation.height/2;
			x += allocation.x + allocation.width;
			y -= popup_allocation.height/2;
			break;
		case PANEL_APPLET_ORIENT_LEFT:
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
on_button_press (PanelApplet   *applet, 
		GdkEventButton *event,
		gpointer	 data)
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
backlight_applet_fill (PanelApplet *applet,
	const gchar *iid,
	gpointer data)
{
	int log_len;
	int setup_len;
	int status_len;
	char *log_name;

	/* backlight_applet = applet; */

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

	if (strcmp(iid, "OAFIID:Backlight") != 0) {
		fprintf(log_file, "Backlight not starting, iid is %s\n", iid);
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


	get_backlight_level();

	backlight_applet_draw_cb(applet);

	return TRUE;
}

/* Factory to interface with the server */

PANEL_APPLET_BONOBO_FACTORY ("OAFIID:Backlight_Factory",
			     PANEL_TYPE_APPLET,
			     "The Backlight Applet",
			     "0",
			     backlight_applet_fill,
			     NULL);