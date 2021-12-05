/* mate panel cpu temperature
 *
 * 26Jan08 wb initial version
 * 27Jan08 wb added setup file
 * 28Jan08 wb added sound and beep
 * 12Jun09 wb if MAIL is emtpy, check LOGNAME
 * 26Jun12 wb migrated to mate for Fedora 17
 * 26Feb14 wb converted to mate 1.6.2 for Fedora 20
 * 27Aug20 wb initial version, copied from mailcheck.c, read from sensors utility
 * 28Aug20 wb read from sys dev files
 * 01Sep20 wb skip small changes to reduce cpu time
 * 12Sep20 wb switch from fopen to open
 * 01Dec21 wb track time of config file
 * 05Dec21 wb track time of last time check of config file
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/unistd.h>
#include <time.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <ctype.h>

#include <mate-panel-applet.h>

#include <gtk/gtklabel.h>
#include <gtk/gtkimage.h>
#include <gtk/gtkbox.h>
#include <gdk/gdkx.h>

#define VERSION		"05Dec21"

#define	BASE_NAME	"temperature"

#define	DEFAULT_INTERVAL	5
#define	DEFAULT_WARNING_TEMPERATURE	90
#define	DEFAULT_WARNING_INTERVAL	5

static int interval = 0;		/* time between temperature checks */
static int debug = 0;			/* enable debug messages to the log file */
static char *home_dir = NULL;		/* user's home directory */
static FILE *log_file = NULL;		/* file for log messages */
static char *sound_name = NULL;		/* name of the sound file for new messages */
static int do_beep = 0;			/* beep on new messages */
static int warning_temperature = 0;	/* temperature to show a warning */
static int warning_interval = 0;	/* interval to repeat a warning */
static char *setup_name = NULL;		/* name of the config file */
static time_t setup_mtime = 0;		/* mtime of config file */
static time_t setup_check_time = 0;	/* time of last check of config file */
static gint timer_handle = 0;		/* handle to change the mate timer */

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
exit_temperature(void)
{
	if (log_file != NULL) {
		fprintf(log_file, "Exiting at %s.\n", show_time());
		fclose(log_file);
		log_file = NULL;
	}

	exit(1);
}

/* Find the current cpu temperature */

static int
check_temperature()
{
	enum check_temperature_enum {
		MAX_BUF = 80,
		HWMON_IND_POS = 44,
		TEMP_IND_POS = 50
	};
	enum check_temperature_source_enum { SENSORS_SOURCE, SYS_DEV_SOURCE, NO_SOURCE };
	FILE *f;
	char buf[ MAX_BUF ];
	int i;
	int temp;
	int result;
	struct stat stat_buf;
	static enum check_temperature_source_enum source = NO_SOURCE;
	static char *hwmon_path = NULL;
	static int temp_set = 0;
	static int temp_min_ind = 0;
	static int temp_max_ind = -1;

	result = 0;

	if (source == NO_SOURCE) {

		/* scan for the hwmon# item */

		strcpy(buf, "/sys/devices/platform/coretemp.0/hwmon/hwmon#/temp1_label");

		for (i = 0; i < 10; i++) {
			buf[ HWMON_IND_POS ] = (char) ('0' + i);
			if (debug && log_file != NULL) {
				fprintf(log_file, "hwmon scan, checking for '%s'\n", buf);
			}
			if (stat(buf, &stat_buf) == 0) {
				source = SYS_DEV_SOURCE;
				hwmon_path = malloc(strlen(buf) + 10);
				if (hwmon_path == NULL) {
					if (log_file != NULL) {
						fprintf(log_file, "could not allocate hwmon path\n");
					}
					exit_temperature();
				}
				strcpy(hwmon_path, buf);
				break;
			}
		}

		/* scan for the list of temp#_label items that map to CPU cores */

		if (source == SYS_DEV_SOURCE) {
			for (i = 1; i < 10; i++) {
				hwmon_path[ TEMP_IND_POS ] = (char) ('0' + i);
				if (debug && log_file != NULL) {
					fprintf(log_file, "temp scan, checking for '%s'\n", hwmon_path);
				}
				f = fopen(hwmon_path, "r");
				if (f == NULL) {
					/* no more items to try */
					break;
				}
				if (fgets(buf, MAX_BUF, f) != NULL && strncmp(buf, "Core", 4) == 0) {
					if (temp_set == 0) {
						temp_min_ind = i;
					}
					temp_max_ind = i;
					temp_set |= (1 << i);
					if (debug && log_file != NULL) {
						fprintf(log_file, "using temp %d with %s\n", i, buf);
					}
				}
				fclose(f);
			}

			/* change the end of the path from "label" to "input" to read the values */

			strcpy(&hwmon_path[ TEMP_IND_POS+2 ], "input");

			/* check that we found something */

			if (temp_set == 0) {
				if (debug && log_file != NULL) {
					fprintf(log_file, "did not find good temp item, reverting to sensors\n");
				}
				source = NO_SOURCE;
			}
		}

		/* fall back to using the sensors utility */

		if (source == NO_SOURCE) {
			source = SENSORS_SOURCE;
		}
	}

	/* read the core temperatures using sys dev files */

	if (source == SYS_DEV_SOURCE) {
		int fd;
		int len;
		for (i = temp_min_ind; i <= temp_max_ind; i++) {
			if (((1 << i) & temp_set) != 0) {
				hwmon_path[ TEMP_IND_POS ] = (char) ('0' + i);
				fd = open(hwmon_path, O_RDONLY);
				if (fd != -1) {
					len = read(fd, buf, MAX_BUF - 1);
					if (len > 0) {
						if (len > MAX_BUF - 1) len = MAX_BUF - 1;
						buf[ len ] = '\0';
						temp = atoi(buf) / 1000;
						if (result < temp) {
							result = temp;
						}
						if (debug && log_file != NULL) {
							fprintf(log_file, "temp ind %d value %d\n", i, temp);
						}
					}
					close(fd);
				}
			}
		}
		return result;
	}

	/* read the core temperatures using the sensors utility */

	f = popen("sensors", "r");

	if (f == NULL) {
		if (log_file != NULL) {
			fprintf(log_file, "popen sensors failed\n");
		}
		exit_temperature();
	}

	while (fgets(buf, MAX_BUF, f) != NULL) {

		buf[ MAX_BUF - 1 ] = '\0';

		if (debug && log_file != NULL) {
			fprintf(log_file, "sensors line: %s", buf);
		}

		i = 0;
		while (buf[i] == ' ') {
			i++;
		}
		if (buf[i] == '\0') {
			break;
		}
		/* parse "Core 0:        +59.0°C  (high = +86.0°C, crit = +100.0�" */
		if (buf[i] == 'C' && buf[i+1] == 'o' && buf[i+2] == 'r' && buf[i+3] == 'e') {
			i += 4;
			while (buf[i] == ' ') {
				i++;
			}
			while (buf[i] >= '0' && buf[i] <= '9') {
				i++;
			}
			if (buf[i] == ':') {
				i++;
			}
			while (buf[i] == ' ' || buf[i] == '\t') {
				i++;
			}
			if (buf[i] == '+') {
				i++;
			}
			temp = 0;
			while (buf[i] >= '0' && buf[i] <= '9' && temp < 1000) {
				temp = temp * 10 + buf[i] - '0';
				i++;
			}
			if (result < temp) {
				result = temp;
			}
			if (debug && log_file != NULL) {
				fprintf(log_file, "Got temp %d new max %d\n", temp, result);
			}
		}
	}

	fclose(f);

	return result;
}

/* Read the setup file */

static void
read_setup_file()
{
	FILE *setup_file;
	enum setup_enum { ID_LEN = 50, BUF_LEN = 1024 };
	char id[ ID_LEN ];
	char buf[ BUF_LEN ];
	int len;
	int ch;
	char *str;
	struct stat stat_buf;

	if (debug && log_file != NULL) {
		fprintf(log_file, "Reading setup file %s.\n", setup_name);
		fflush(log_file);
	}

	setup_mtime = 0;
	setup_check_time = time(NULL);

	setup_file = fopen(setup_name, "r");

	if (setup_file == NULL) {
		if (log_file != NULL) {
			fprintf(log_file, "Could not open setup file %s.\n", setup_name);
			fflush(log_file);
		}
		return;
	}

	if (fstat(fileno(setup_file), &stat_buf) == 0) {
		setup_mtime = stat_buf.st_mtim.tv_sec;
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
		} else if (strcmp(id, "sound") == 0) {
			int i;
			int ok;
			if (sound_name != NULL) {
				free(sound_name);
				sound_name = NULL;
			}
			ok = TRUE;
			for (i = 0; buf[i] != '\0'; i++) {
				if (buf[i] == '\\' || buf[i] == '\'') {
					ok = FALSE;
					break;
				}
			}
			if (len == 0) {
				if (debug && log_file != NULL) fprintf(log_file, "Clear 'sound' file.\n");
			} else if (!ok) {
				if (debug && log_file != NULL)
					fprintf(log_file, "Setup file '%s' has 'sound' file '%s' with special characters.\n",
						setup_name, buf);
			} else if (access(buf, R_OK) != 0) {
				if (debug && log_file != NULL)
					fprintf(log_file, "Setup file '%s' lists missing 'sound' file '%s'.\n", setup_name, buf);
			} else {
				str = strdup(buf);
				if (str != NULL) {
					sound_name = str;
					if (debug && log_file != NULL)
						fprintf(log_file, "Set 'sound' to '%s'.\n", sound_name);
				}
			}
		} else if (strcmp(id, "beep") == 0) {
			do_beep =
				((len == 0 ||
				  (isdigit(buf[0]) && atoi(buf) > 0) ||
				  (buf[0] == 'y' || buf[0] == 't'))? 1: 0);
			if (debug && log_file != NULL)
				fprintf(log_file, "Set 'beep' to %d.\n", do_beep);
		} else if (strcmp(id, "interval") == 0) {
			if (len == 0 || !isdigit(buf[0])) {
				if (debug && log_file != NULL)
					fprintf(log_file, "Setup file '%s' has 'interval' without numeric value.\n", setup_name);
			} else {
				interval = atoi(buf);
				if (interval < 1) interval = 1;
				if (interval > 1000) interval = 1000;
				if (debug && log_file != NULL) fprintf(log_file, "Set 'interval' to %d seconds.\n", interval);
			}
		} else if (strcmp(id, "warn") == 0) {
			if (len == 0 || !isdigit(buf[0])) {
				if (debug && log_file != NULL)
					fprintf(log_file, "Setup file '%s' has 'warn' without numeric value.\n", setup_name);
			} else {
				warning_temperature = atoi(buf);
				if (warning_temperature < 0) warning_temperature = 0;
				if (warning_temperature > 1000) warning_temperature = 1000;
				if (debug && log_file != NULL) fprintf(log_file, "Set 'warn' to %d degrees.\n", warning_temperature);
			}
		} else if (strcmp(id, "warninterval") == 0) {
			if (len == 0 || !isdigit(buf[0])) {
				if (debug && log_file != NULL)
					fprintf(log_file, "Setup file '%s' has 'warninterval' without numeric value.\n", setup_name);
			} else {
				warning_interval = atoi(buf);
				if (warning_interval < 0) warning_interval = 0;
				if (warning_interval > 1000) warning_interval = 1000;
				if (debug && log_file != NULL) fprintf(log_file, "Set 'warninterval' to %d seconds.\n", warning_interval);
			}
		} else if (strcmp(id, "debug") == 0) {
			if (len == 0 || !isdigit(buf[0])) {
				if (debug && log_file != NULL)
					fprintf(log_file, "Setup file '%s' has 'debug' without numeric value.\n", setup_name);
			} else {
				debug = atoi(buf);
				if (debug && log_file != NULL) fprintf(log_file, "Set debug level to %d.\n", debug);
			}
		} else {
			if (log_file != NULL)
				fprintf(log_file, "Warning: Setup file '%s' has unknown option '%s'.\n", setup_name, id);
		}
	}

	fclose(setup_file);

	if (log_file != NULL) {
		fprintf(log_file, "Read setup file '%s' at %s.\n", setup_name, show_time());
		fprintf(log_file, " interval %d seconds\n", interval);
		fprintf(log_file, " warn at %d degrees\n", warning_temperature);
		fprintf(log_file, " warn again after %d seconds\n", warning_interval);
		fprintf(log_file, " play sound '%s'\n", (sound_name? sound_name: "<none>"));
		fprintf(log_file, " beep '%d'\n", do_beep);
		fprintf(log_file, " debug level %d\n", debug);
		fflush(log_file);
	}
}

/* Update the status displayed in the panel */

static gboolean
open_window (GtkEventBox *event_box, gboolean force_update)
{
	static GtkWidget *last_label = NULL;
	static int last_temperature = 0;
	static time_t last_warning_time = 0;
	int temperature;
	enum open_window_enum { TEMP_BUF_LEN = 80 };
	char temp_buf[ TEMP_BUF_LEN ];

	temperature = check_temperature();

	if (debug && log_file != NULL) {
		fprintf(log_file, "old temp %d new temp %d at %s\n",
			last_temperature, temperature, show_time());
		fflush(log_file);
	}

	if (temperature >= warning_temperature && time(NULL) >= last_warning_time + warning_interval) {
		last_warning_time = time(NULL);
		if (debug && log_file != NULL) {
			fprintf(log_file, "high temp %d at %ld, last temp %d\n", temperature, last_warning_time, last_temperature);
		}
		if (do_beep) {
			XBell( GDK_DISPLAY_XDISPLAY( gtk_widget_get_display( GTK_WIDGET( event_box ) ) ), 0 );
		}
		if (sound_name != NULL) {
			char *cmd;
			cmd = malloc(strlen(sound_name) + 20);
			if (cmd != NULL) {
				sprintf(cmd, "play '%s' &", sound_name);
				system(cmd);
				free(cmd);
			}
		}
	}

	if (temperature != last_temperature &&
	    (force_update ||
	     abs(temperature - last_temperature) > 2 ||
	     temperature >= warning_temperature ||
	     last_temperature >= warning_temperature)) {
		if (last_label != NULL) {
			gtk_container_remove (GTK_CONTAINER (event_box), last_label);
			if (debug && log_file != NULL) {
				fprintf(log_file, "remove label for state %d\n", last_temperature);
			}
		}

		last_temperature = temperature;
		if (temperature > 0) {
			sprintf(temp_buf, "Temp %d", temperature);
			last_label = gtk_label_new (temp_buf);
			gtk_container_add (GTK_CONTAINER (event_box), last_label);
			if (debug && log_file != NULL) {
				fprintf(log_file, "add label for temp %d\n", last_temperature);
				fflush(log_file);
			}
		} else {
			last_label = NULL;
		}

		gtk_widget_show_all ( GTK_WIDGET(event_box) );
	}

	return TRUE;
}

/* Forward declaration */

static gint on_timer (gpointer data);

/* Handle a left click on the panel */
/*   Reload the setup file (if needed) and update the panel */

static gboolean
on_button_press (GtkWidget      *event_box,
		GdkEventButton *event,
		gpointer	 data)
{
	int last_interval;
	time_t cur_time;
	struct stat stat_buf;

	/* Don't react to anything other than the left mouse button;
	   return FALSE so the event is passed to the default handler */

	if (event->button != 1)
		return FALSE;

	/* Check if the setup file has changed */

	cur_time = time(NULL);

	if (cur_time - setup_check_time < 5) {
		if (debug && log_file != NULL) {
			fprintf(log_file, "%ld secs since last setup file check, not rechecking.\n", cur_time - setup_check_time);
		}
	} else {
		setup_check_time = cur_time;
		if (setup_name != NULL &&
		    stat(setup_name, &stat_buf) == 0 &&
		    stat_buf.st_mtim.tv_sec == setup_mtime) {
			if (debug && log_file != NULL) {
				fprintf(log_file, "Setup file unchanged, not reloading.\n");
			}
		} else {
			last_interval = interval;

			read_setup_file();

			if (interval != last_interval) {
				g_source_remove(timer_handle);
				timer_handle = g_timeout_add (interval * 1000, on_timer, event_box);
				if (debug && log_file != NULL) {
					fprintf(log_file, "Resetting timer from %d to %d seconds.\n", last_interval, interval);
				}
			}
		}
	}

	return open_window( GTK_EVENT_BOX(event_box), /* force update */ TRUE );
}

/* Handle a timeout */
/*   Update the panel */

static gint
on_timer (gpointer data)
{
	return open_window(data, /* force update */ FALSE);
}

/* Main entry point of the applet */
/*   Initialize from the environment */
/*   Set up global variables */


static gboolean
temperature_applet_fill (MatePanelApplet *applet,
	const gchar *iid,
	gpointer data)
{
	GtkEventBox *event_box;
	int log_len;
	int setup_len;
	char *log_name;

	if (strcmp (iid, "TemperatureApplet") != 0) {
		return FALSE;
	}

	interval = DEFAULT_INTERVAL;

	warning_temperature = DEFAULT_WARNING_TEMPERATURE;

	warning_interval = DEFAULT_WARNING_INTERVAL;

	home_dir = getenv("HOME");
	if (home_dir == NULL) {
		home_dir = "/tmp";
	}
	log_len = strlen(home_dir) + strlen(BASE_NAME) + 10;
	log_name = malloc(log_len);
	if (!log_name) {
		exit_temperature();
	}

	sprintf(log_name, "%s/.%s.log", home_dir, BASE_NAME);

	log_file = fopen(log_name, "w");

	if (!log_file) {
		exit_temperature();
	}

	fprintf(log_file, "Starting %s Version %s at %s...\n", BASE_NAME, VERSION, show_time());
	fflush(log_file);

	setup_len = strlen(home_dir) + strlen(BASE_NAME) + 10;
	setup_name = malloc(setup_len);
	if (!setup_name) {
		fprintf(log_file, "Could not allocate setup name.\n");
		exit_temperature();
	}

	sprintf(setup_name, "%s/.%src", home_dir, BASE_NAME);

	read_setup_file();

	free(log_name);
	log_name = NULL;

	event_box = (GtkEventBox *) gtk_event_box_new ();

	open_window(event_box, /* force update */ TRUE);

	gtk_container_add (GTK_CONTAINER (applet), GTK_WIDGET (event_box) );
	gtk_widget_show_all (GTK_WIDGET (applet));

	g_signal_connect (G_OBJECT (event_box),
			"button_press_event",
			G_CALLBACK (on_button_press),
			NULL);

	timer_handle = g_timeout_add (interval * 1000, on_timer, event_box);

	return TRUE;
}

/* Factory to interface with the server */

#if 1

/* mate 4 */

static gboolean
temperature_applet_factory (MatePanelApplet *applet,
			  const char  *iid,
			  gpointer     data)
{
	gboolean retval = FALSE;

	if (!strcmp (iid, "TemperatureApplet"))
		retval = temperature_applet_fill (applet, iid, data);

	return retval;
}


MATE_PANEL_APPLET_OUT_PROCESS_FACTORY ("TemperatureAppletFactory",
			PANEL_TYPE_APPLET,
			"TemperatureApplet",
			temperature_applet_factory,
			NULL);
#else

/* mate 2 */

MATE_PANEL_APPLET_MATECOMPONENT_FACTORY ("OAFIID:Temperature_Factory",
			     PANEL_TYPE_APPLET,
			     "The Temperature Check Applet",
			     "0",
			     temperature_applet_fill,
			     NULL);

#endif
