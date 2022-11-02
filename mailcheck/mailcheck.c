/* mate panel mail checker
 *	checks a mail spool file
 *	updates a status line in a mate panel
 *
 * 26Jan08 wb initial version
 * 27Jan08 wb added setup file
 * 28Jan08 wb added sound and beep
 * 12Jun09 wb if MAIL is emtpy, check LOGNAME
 * 26Jun12 wb migrated to mate for Fedora 17
 * 26Feb14 wb converted to mate 1.6.2 for Fedora 20
 * 04Jan22 wb play with -q
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/unistd.h>
#include <time.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#include <mate-panel-applet.h>

#include <gtk/gtklabel.h>
#include <gtk/gtkimage.h>
#include <gtk/gtkbox.h>
#include <gdk/gdkx.h>

#define VERSION		"04Jan22"

#define	BASE_NAME	"mailcheck"

#define	DEFAULT_INTERVAL	5

static int interval = 0;		/* time between mail checks */
static int debug = 0;			/* enable debug messages to the log file */
static char *home_dir = NULL;		/* user's home directory */
static FILE *log_file = NULL;		/* file for log messages */
static char *mail_name = NULL;		/* name of the mail spool file */
static char *sound_name = NULL;		/* name of the sound file for new messages */
static int do_beep = 0;			/* beep on new messages */
static off_t last_size = 0;		/* last size of the mail spool file */
static time_t last_mtime = 0;		/* last modification time of the mail spool file */
static char *setup_name = NULL;		/* name of the config file */
static gint timer_handle = 0;		/* handle to change the mate timer */

enum mail_state_enum {
	INIT_MAIL = 0,	/* nothing displayed yet */
	NO_MAIL,	/* mailbox is empty */
	OLD_MAIL,	/* mailbox has read mail */
	UNREAD_MAIL,	/* mailbox has unread mail */
	NEW_MAIL,	/* mailbox has new mail since the last check */
	NUM_MAIL_STATES
};

static const char *mail_state_name[ NUM_MAIL_STATES ] =
	{ NULL, "No mail", "Mail", "New mail", NULL };

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
exit_mailcheck(void)
{
	if (log_file != NULL) {
		fprintf(log_file, "Exiting at %s.\n", show_time());
		fclose(log_file);
		log_file = NULL;
	}

	exit(1);
}

/* Find the current state of the mail spool file */

static enum mail_state_enum
check_mail_state()
{
	enum mail_state_enum result;
	struct stat stat_buf;

	if (!mail_name) {
		exit_mailcheck();
	}

	result = NO_MAIL;

	if (stat(mail_name, &stat_buf) != 0) {
		/* mail file does not exist, so there is no mail */
		last_size = 0;
		last_mtime = 0;
	} else if (stat_buf.st_size > 0) {
		if (stat_buf.st_size != last_size || stat_buf.st_mtime != last_mtime) {
			result = NEW_MAIL;
		} else {
			result = (stat_buf.st_mtime >= stat_buf.st_atime? UNREAD_MAIL: OLD_MAIL);
		}
		last_size = stat_buf.st_size;
		last_mtime = stat_buf.st_mtime;
	}
	if (debug && log_file != NULL) {
		fprintf(log_file, "%s size %ld time %ld result %d\n", mail_name, (long)last_size, (long)last_mtime, result);
		fflush(log_file);
	}
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

	setup_file = fopen(setup_name, "r");

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
		} else if (strcmp(id, "mail") == 0) {
			if (len == 0) {
				if (log_file != NULL)
					fprintf(log_file, "Setup file '%s' has 'mail' without file name.\n", setup_name);
			} else {
				str = strdup(buf);
				if (str != NULL) {
					free(mail_name);
					mail_name = str;
					if (log_file != NULL)
						fprintf(log_file, "Set 'mail' to '%s'.\n", mail_name);
				}
			}
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
				if (log_file != NULL) fprintf(log_file, "Clear 'sound' file.\n");
			} else if (!ok) {
				if (log_file != NULL)
					fprintf(log_file, "Setup file '%s' has 'sound' file '%s' with special characters.\n",
						setup_name, buf);
			} else if (access(buf, R_OK) != 0) {
				if (log_file != NULL)
					fprintf(log_file, "Setup file '%s' lists missing 'sound' file '%s'.\n", setup_name, buf);
			} else {
				str = strdup(buf);
				if (str != NULL) {
					sound_name = str;
					if (log_file != NULL)
						fprintf(log_file, "Set 'sound' to '%s'.\n", sound_name);
				}
			}
		} else if (strcmp(id, "beep") == 0) {
			do_beep =
				((len == 0 ||
				  (isdigit(buf[0]) && atoi(buf) > 0) ||
				  (buf[0] == 'y' || buf[0] == 't'))? 1: 0);
			if (log_file != NULL)
				fprintf(log_file, "Set 'beep' to %d.\n", do_beep);
		} else if (strcmp(id, "interval") == 0) {
			if (len == 0 || !isdigit(buf[0])) {
				if (log_file != NULL)
					fprintf(log_file, "Setup file '%s' has 'interval' without numeric value.\n", setup_name);
			} else {
				interval = atoi(buf);
				if (interval < 1) interval = 1;
				if (interval > 1000) interval = 1000;
				if (log_file != NULL) fprintf(log_file, "Set 'interval' to %d seconds.\n", interval);
			}
		} else if (strcmp(id, "debug") == 0) {
			if (len == 0 || !isdigit(buf[0])) {
				if (log_file != NULL)
					fprintf(log_file, "Setup file '%s' has 'debug' without numeric value.\n", setup_name);
			} else {
				debug = atoi(buf);
				if (log_file != NULL) fprintf(log_file, "Set debug level to %d.\n", debug);
			}
		} else {
			if (log_file != NULL)
				fprintf(log_file, "Setup file '%s' has unknown option '%s'.\n", setup_name, id);
		}	
	}

	fclose(setup_file);

	if (log_file != NULL) {
		fprintf(log_file, "Read setup file '%s' at %s.\n", setup_name, show_time());
		fprintf(log_file, " mail '%s'\n", mail_name);
		fprintf(log_file, " interval %d seconds\n", interval);
		fprintf(log_file, " play sound '%s'\n", (sound_name? sound_name: "<none>"));
		fprintf(log_file, " beep '%d'\n", do_beep);
		fprintf(log_file, " debug level %d\n", debug);
		fflush(log_file);
	}
}

/* Update the status displayed in the panel */

static gboolean
open_window (GtkEventBox *event_box)
{
	static GtkWidget *last_label = NULL;
	static enum mail_state_enum last_mail_state = INIT_MAIL;
	enum mail_state_enum mail_state;

	mail_state = check_mail_state();

	if (debug && log_file != NULL) {
		fprintf(log_file, "old state %d %s new state %d %s at %s\n",
			last_mail_state, mail_state_name[ last_mail_state ],
			mail_state, mail_state_name[ mail_state ],
			show_time());
	}

	if (mail_state != last_mail_state) {
		if (last_label != NULL) {
			gtk_container_remove (GTK_CONTAINER (event_box), last_label);
			if (debug && log_file != NULL) {
				fprintf(log_file, "remove label for state %d\n", last_mail_state);
			}
		}

		if (mail_state == NEW_MAIL || (mail_state > NO_MAIL && last_mail_state == INIT_MAIL)) {
			if (debug && log_file != NULL) {
				fprintf(log_file, "new mail\n");
			}
			mail_state = UNREAD_MAIL;
			if (do_beep) {
				XBell( GDK_DISPLAY_XDISPLAY( gtk_widget_get_display( GTK_WIDGET( event_box ) ) ), 0 );
			}
			if (sound_name != NULL) {
				char *cmd;
				cmd = malloc(strlen(sound_name) + 20);
				if (cmd != NULL) {
					sprintf(cmd, "play -q '%s' &", sound_name);
					system(cmd);
					free(cmd);
				}
			}
		}

		last_mail_state = mail_state;
		last_label = (mail_state_name[ mail_state ]? gtk_label_new (mail_state_name[ mail_state ]): NULL);

		if (last_label != NULL) {
			gtk_container_add (GTK_CONTAINER (event_box), last_label);
			if (debug && log_file != NULL) {
				fprintf(log_file, "add label for state %d\n", last_mail_state);
				fflush(log_file);
			}
		}
		gtk_widget_show_all ( GTK_WIDGET(event_box) );
	}

	/* Example code to open a window with an image and text */
#if 0
	static int window_shown;
	static GtkWidget *window, *box, *image, *label;

	if (!window_shown) {
		window = gtk_window_new (GTK_WINDOW_TOPLEVEL);
		box = GTK_BOX (gtk_vbox_new (TRUE, 12));
		gtk_container_add (GTK_CONTAINER (window), box);

		image = GTK_IMAGE (gtk_image_new_from_file ("/usr/share/pixmaps/user_icon.png"));
		gtk_box_pack_start (GTK_BOX (box), image, TRUE, TRUE, 12);

		label = gtk_label_new ("Hello World");
		gtk_box_pack_start (GTK_BOX (box), label, TRUE, TRUE, 12);
		
		gtk_widget_show_all (window);
	}
	else
		gtk_widget_hide (GTK_WIDGET (window));

	window_shown = !window_shown;
#endif

	return TRUE;
}

/* Forward declaration */

static gint on_timer (gpointer data);

/* Handle a left click on the panel */
/*   Reload the setup file and update the panel */

static gboolean
on_button_press (GtkWidget      *event_box, 
		GdkEventButton  *event,
		gpointer	 data)
{
	int last_interval;

	/* Don't react to anything other than the left mouse button;
	   return FALSE so the event is passed to the default handler */

	if (event->button != 1)
		return FALSE;

	last_interval = interval;

	read_setup_file();

	if (interval != last_interval) {
		g_source_remove(timer_handle);
		timer_handle = g_timeout_add (interval * 1000, on_timer, event_box);
		if (debug && log_file != NULL) {
			fprintf(log_file, "Resetting timer from %d to %d seconds.\n", last_interval, interval);
		}
	}

	return open_window( GTK_EVENT_BOX(event_box) );
}

/* Handle a timeout */
/*   Update the panel */

static gint
on_timer (gpointer data)
{
	return open_window(data);
}

/* Main entry point of the applet */
/*   Initialize from the environment */
/*   Set up global variables */


static gboolean
mailcheck_applet_fill (MatePanelApplet *applet,
	const gchar *iid,
	gpointer data)
{
	GtkEventBox *event_box;
	int log_len;
	int setup_len;
	char *log_name;

	if (strcmp (iid, "MailCheckApplet") != 0) {
		return FALSE;
	}

	interval = DEFAULT_INTERVAL;

	home_dir = getenv("HOME");
	if (home_dir == NULL) {
		home_dir = "/tmp";
	}
	log_len = strlen(home_dir) + strlen(BASE_NAME) + 10;
	log_name = malloc(log_len);
	if (!log_name) {
		exit_mailcheck();
	}

	sprintf(log_name, "%s/.%s.log", home_dir, BASE_NAME);

	log_file = fopen(log_name, "w");

	if (!log_file) {
		exit_mailcheck();
	}

	fprintf(log_file, "Starting %s Version %s at %s...\n", BASE_NAME, VERSION, show_time());
	fflush(log_file);

	setup_len = strlen(home_dir) + strlen(BASE_NAME) + 10;
	setup_name = malloc(setup_len);
	if (!setup_name) {
		fprintf(log_file, "Could not allocate setup name.\n");
		exit_mailcheck();
	}

	sprintf(setup_name, "%s/.%src", home_dir, BASE_NAME);

	mail_name = getenv("MAIL");
	if (mail_name != NULL) mail_name = strdup(mail_name);

	if (!mail_name) {
		char *login_name;
		char *var_spool_mail = "/var/spool/mail";
		login_name = getenv("LOGNAME");
		if (login_name != NULL) {
			mail_name = malloc(strlen(var_spool_mail) + strlen(login_name) + 4);
			if (!mail_name) {
				fprintf(log_file, "Could not allocate mail file name.\n");
				exit_mailcheck();
			}
			sprintf(mail_name, "%s/%s", var_spool_mail, login_name);
		}
	}

	read_setup_file();

	if (!mail_name) {
		fprintf(log_file, "You must set MAIL to your mail file.\n");
		exit_mailcheck();
	}

	free(log_name);
	log_name = NULL;

	event_box = (GtkEventBox *) gtk_event_box_new ();

	open_window(event_box);

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
mailcheck_applet_factory (MatePanelApplet *applet,
			  const char  *iid,
			  gpointer     data)
{
	gboolean retval = FALSE;

	if (!strcmp (iid, "MailCheckApplet"))
		retval = mailcheck_applet_fill (applet, iid, data);

	return retval;
}


MATE_PANEL_APPLET_OUT_PROCESS_FACTORY ("MailCheckAppletFactory",
			PANEL_TYPE_APPLET,
			"MailCheckApplet",
			mailcheck_applet_factory,
			NULL);
#else

/* mate 2 */

MATE_PANEL_APPLET_MATECOMPONENT_FACTORY ("OAFIID:MailCheck_Factory",
			     PANEL_TYPE_APPLET,
			     "The Mail Check Applet",
			     "0",
			     mailcheck_applet_fill,
			     NULL);

#endif
