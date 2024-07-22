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
 * 17Oct22 wb support more than 9 cores, support thinkpad_hwmon
 * 22Oct22 wb add tempinterval to repaint for small temperature changes, show fan speed
 * 25Oct22 wb use openat()
 * 05Nov22 wb get GPU temperature
 * 09Nov22 wb add unicode option
 * 17Jul24 wb add nvme ssd
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

#define VERSION		"17Jul24"

#define BASE_NAME	"temperature"

#define DEFAULT_INTERVAL		3
#define DEFAULT_TEMPERATURE_INTERVAL	(2 * DEFAULT_INTERVAL)
#define DEFAULT_WARNING_TEMPERATURE	90
#define MAX_WARNING_TEMPERATURE		1000
#define DEFAULT_WARNING_INTERVAL	(5 * DEFAULT_INTERVAL)
#define DEFAULT_FAN_CHECK_INTERVAL	((3 * DEFAULT_TEMPERATURE_INTERVAL) / 2)
#define MAX_INTERVAL			1000

static int interval = 0;		/* time between temperature checks */
static int debug = 0;			/* enable debug messages to the log file */
static char *home_dir = NULL;		/* user's home directory */
static FILE *log_file = NULL;		/* file for log messages */
static char *sound_name = NULL;		/* name of the sound file for new messages */
static int do_beep = 0;			/* beep on new messages */
static int do_unicode = 1;		/* show unicode instead of text */
static int temperature_interval = 0;	/* interval to update temperature if it only changed a little */
static int fan_check_interval = 0;	/* interval to check fan */
static int gpu_temp_interval = 0;	/* interval to check gpu */
static int ssd_temp_interval = 0;	/* interval to check ssd */
static int ssd_hide_temperature = 0;	/* hide low ssd temperatures */
static int warning_temperature = 0;	/* temperature to show a warning */
static int warning_interval = 0;	/* interval to repeat a warning */
static char *setup_name = NULL;		/* name of the config file */
static time_t setup_mtime = 0;		/* mtime of config file */
static time_t setup_check_time = 0;	/* time of last check of config file */
static gint timer_handle = 0;		/* handle to change the mate timer */
static const char *temp_text = NULL;	/* text to show temperature */
static const char *gpu_text = NULL;	/* text to show gpu */
static const char *ssd_text = NULL;	/* test to show ssd */
static const char *fan_text = NULL;	/* text to show fan */

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

/* Constants */

enum check_temperature_enum {
	MAX_BUF = 80,
	HWMON_IND_POS = 44,
	TEMP_IND_POS = 50,
	DEV_NVME_HWMON_IND_POS = 27,
	DEV_NVME_HWMON_TEMP_IND_POS = DEV_NVME_HWMON_IND_POS + 6,
	THINKPAD_HWMON_IND_POS = 48,
	THINKPAD_TEMP_IND_POS = THINKPAD_HWMON_IND_POS + 6
};

/* Find the current fan speed */

static const char *hwmon_fan_speed_path = NULL;
static int hwmon_fan_speed_dir_fd = -1;

static int
check_fan_speed()
{
	int result = -1;

	if (hwmon_fan_speed_path != NULL) {
		int fd = openat(hwmon_fan_speed_dir_fd, hwmon_fan_speed_path, O_RDONLY);
		if (fd != -1) {
			char buf[ MAX_BUF ];
			int len = read(fd, buf, MAX_BUF - 1);
			if (len > 0) {
				if (len > MAX_BUF - 1) len = MAX_BUF - 1;
				buf[ len ] = '\0';
				result = (atoi(buf) + 50) / 100;
				if (debug && log_file != NULL) {
					fprintf(log_file, "hwmon CPU fan speed %d\n", result);
				}
			}
			close(fd);
		}
	}
	return result;
}

/* Find the current gpu temperature */

static char *hwmon_gpu_temp_path = NULL;
static int hwmon_gpu_temp_dir_fd = -1;

static int
check_gpu_temp()
{
	int result = 0;

	if (hwmon_gpu_temp_path != NULL) {
		int fd = openat(hwmon_gpu_temp_dir_fd, hwmon_gpu_temp_path, O_RDONLY);
		if (fd != -1) {
			char buf[ MAX_BUF ];
			int len = read(fd, buf, MAX_BUF - 1);
			if (len > 0) {
				if (len > MAX_BUF - 1) len = MAX_BUF - 1;
				buf[ len ] = '\0';
				result = atoi(buf) / 1000;
				if (debug && log_file != NULL) {
					fprintf(log_file, "hwmon GPU temp %d\n", result);
				}
			}
			close(fd);
		}
		if (result <= 0) {
			if (debug && log_file != NULL) {
				fprintf(log_file, "hwmon GPU temp N/A\n");
			}
		}
	}
	return result;
}

/* Find the current ssd temperature */

static char *hwmon_ssd_temp_path = NULL;
static int hwmon_ssd_temp_dir_fd = -1;

static int
check_ssd_temp()
{
	int result = 0;

	if (hwmon_ssd_temp_path != NULL) {
		int fd = openat(hwmon_ssd_temp_dir_fd, hwmon_ssd_temp_path, O_RDONLY);
		if (fd != -1) {
			char buf[ MAX_BUF ];
			int len = read(fd, buf, MAX_BUF - 1);
			if (len > 0) {
				if (len > MAX_BUF - 1) len = MAX_BUF - 1;
				buf[ len ] = '\0';
				result = atoi(buf) / 1000;
				if (debug && log_file != NULL) {
					fprintf(log_file, "hwmon ssd temp %d\n", result);
				}
			}
			close(fd);
		}
		if (result <= 0) {
			if (debug && log_file != NULL) {
				fprintf(log_file, "hwmon ssd temp N/A\n");
			}
		}
	}
	return result;
}

/* Find the current cpu temperature */

static int
check_temperature()
{
	enum check_temperature_source_enum { SENSORS_SOURCE, SYS_DEV_SOURCE, THINKPAD_SOURCE, NO_SOURCE };
	FILE *f;
	char buf[ MAX_BUF ];
	int i;
	int temp;
	int result;
	char *path;
	char *ssd_hwmon_path;
	int ssd_hwmon_dir_fd;
	struct stat stat_buf;
	static enum check_temperature_source_enum source = NO_SOURCE;
	static char *hwmon_path = NULL;
	static char *hwmon_path2 = NULL;
	static int generic_hwmon_dir_fd = -1;
	static int thinkpad_hwmon_dir_fd = -1;
	static uint64_t temp_set = 0;
	static int temp_min_ind = 0;
	static int temp_max_ind = -1;
	static const char *temperature_source_names[] = { "sensors", "generic hwmon", "thinkpad hwmon", "no" };

	result = 0;

	/* one-time initialization */

	if (source == NO_SOURCE) {

		/* scan for the thinkpad item */

		strcpy(buf, "/sys/devices/platform/thinkpad_hwmon/hwmon/hwmon#/temp1_label");

		for (i = 0; i < 10; i++) {
			buf[ THINKPAD_HWMON_IND_POS ] = (char) ('0' + i);
			if (debug && log_file != NULL) {
				fprintf(log_file, "hwmon scan, checking for '%s'\n", buf);
			}
			if (stat(buf, &stat_buf) == 0) {
				source = THINKPAD_SOURCE;
				hwmon_path = malloc(strlen(buf) + 10);
				if (hwmon_path == NULL) {
					if (log_file != NULL) {
						fprintf(log_file, "could not allocate hwmon path\n");
					}
					exit_temperature();
				}
				strcpy(hwmon_path, buf);
				buf[ THINKPAD_HWMON_IND_POS + 1 ] = '\0';
				thinkpad_hwmon_dir_fd = open(buf, O_DIRECTORY | __O_PATH);
				if (thinkpad_hwmon_dir_fd == -1) {
					if (log_file != NULL) {
						fprintf(log_file, "path open of fan dir %s failed\n", buf);
					}
					exit_temperature();
				}
				hwmon_fan_speed_dir_fd = thinkpad_hwmon_dir_fd;
				hwmon_gpu_temp_dir_fd = thinkpad_hwmon_dir_fd;
				break;
			}
		}

		/* scan temp#_label with the CPU item */

		if (source == THINKPAD_SOURCE) {
			int thinkpad_cpu_ind = -1;
			int thinkpad_gpu_ind = -1;
			for (i = 1; i < 10; i++) {
				hwmon_path[ THINKPAD_TEMP_IND_POS ] = (char) ('0' + i);
				if (debug && log_file != NULL) {
					fprintf(log_file, "temp scan, checking for '%s'\n", hwmon_path);
				}
				f = fopen(hwmon_path, "r");
				if (f == NULL) {
					continue;
				}
				if (fgets(buf, MAX_BUF, f) != NULL) {
					if (thinkpad_cpu_ind < 0 && strncmp(buf, "CPU", 3) == 0) {
						thinkpad_cpu_ind = i;
						if (debug && log_file != NULL) {
							fprintf(log_file, "using CPU temp %d with %s\n", i, buf);
						}
					} else if (thinkpad_gpu_ind < 0 && strncmp(buf, "GPU", 3) == 0) {
						thinkpad_gpu_ind = i;
						if (debug && log_file != NULL) {
							fprintf(log_file, "using GPU temp %d with %s\n", i, buf);
						}
					}
				}
				fclose(f);
				if (thinkpad_cpu_ind >= 0 && thinkpad_gpu_ind >= 0) {
					break;
				}
			}
			if (thinkpad_cpu_ind > 0) {
				/* found the thinkpad CPU item */
				/* change the end of the path from "label" to "input" to read the values */
				hwmon_path[ THINKPAD_TEMP_IND_POS ] = (char) ('0' + thinkpad_cpu_ind);
				strcpy(&hwmon_path[ THINKPAD_TEMP_IND_POS+2 ], "input");
				/* fans don't have labels. the first seems to be the cpu. */
				hwmon_fan_speed_path = "fan1_input";
				if (debug && log_file != NULL) {
					fprintf(log_file, "using fan %s\n", hwmon_fan_speed_path);
				}
				if (check_fan_speed() < 0) {
					hwmon_fan_speed_path = NULL;
				}
				if (thinkpad_gpu_ind > 0) {
					hwmon_gpu_temp_path = (char *) malloc(20);
					if (hwmon_gpu_temp_path != NULL) {
						strcpy(hwmon_gpu_temp_path, "temp#_input");
						hwmon_gpu_temp_path[ 4 ] = (char) ('0' + thinkpad_gpu_ind);
						if (debug && log_file != NULL) {
							fprintf(log_file, "using gpu temp %s\n", hwmon_gpu_temp_path);
						}
					}
				}
			}

			/* check that we found something */

			if (thinkpad_cpu_ind <= 0) {
				if (debug && log_file != NULL) {
					fprintf(log_file, "did not find good thinkpad CPU temp item, reverting to generic hwmon\n");
				}
				source = NO_SOURCE;
			}
		}

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
					hwmon_path2 = malloc(strlen(buf) + 10);
					if (hwmon_path == NULL) {
						if (log_file != NULL) {
							fprintf(log_file, "could not allocate hwmon path\n");
						}
						exit_temperature();
					}
					strcpy(hwmon_path, buf);
					strcpy(hwmon_path2, buf);
					strcpy(&hwmon_path2[TEMP_IND_POS+1], &buf[TEMP_IND_POS]);
					buf[ HWMON_IND_POS + 1 ] = '\0';
					generic_hwmon_dir_fd = open(buf, O_DIRECTORY | __O_PATH);
					if (generic_hwmon_dir_fd == -1) {
						if (log_file != NULL) {
							fprintf(log_file, "path open of fan dir %s failed\n", buf);
						}
						exit_temperature();
					}
					break;
				}
			}
		}

		/* scan for the list of temp#_label items that map to CPU cores */

		if (source == SYS_DEV_SOURCE) {
			for (i = 1; i < 64; i++) {
				if (i < 10) {
					path = hwmon_path;
					path[ TEMP_IND_POS ] = (char) ('0' + i);
				} else {
					path = hwmon_path2;
					path[ TEMP_IND_POS ] = (char) ('0' + i/10);
					path[ TEMP_IND_POS+1 ] = (char) ('0' + i%10);
				}
				if (debug && log_file != NULL) {
					fprintf(log_file, "temp scan, checking for '%s'\n", path);
				}
				f = fopen(path, "r");
				if (f == NULL) {
					continue;
				}
				if (fgets(buf, MAX_BUF, f) != NULL && strncmp(buf, "Core", 4) == 0) {
					if (temp_set == 0) {
						temp_min_ind = i;
					}
					temp_max_ind = i;
					temp_set |= (1ull << i);
					if (debug && log_file != NULL) {
						fprintf(log_file, "using temp %d with %s\n", i, buf);
					}
				}
				fclose(f);
			}

			/* change the end of the path from "label" to "input" to read the values */

			strcpy(&hwmon_path[ TEMP_IND_POS+2 ], "input");
			strcpy(&hwmon_path2[ TEMP_IND_POS+3 ], "input");

			/* check that we found something */

			if (temp_set == 0) {
				if (debug && log_file != NULL) {
					fprintf(log_file, "did not find good temp item, reverting to sensors\n");
				}
				source = NO_SOURCE;
			}
		}

		/* scan for the ssd temperature */

		ssd_hwmon_path = NULL;
		ssd_hwmon_dir_fd = -1;

		strcpy(buf, "/sys/class/nvme/nvme0/hwmon#/temp1_label");

		for (i = 0; i < 10; i++) {
			buf[ DEV_NVME_HWMON_IND_POS ] = (char) ('0' + i);
			if (debug && log_file != NULL) {
				fprintf(log_file, "nvme hwmon scan, checking for '%s'\n", buf);
			}
			if (stat(buf, &stat_buf) == 0) {
				ssd_hwmon_path = malloc(strlen(buf) + 10);
				if (ssd_hwmon_path == NULL) {
					if (log_file != NULL) {
						fprintf(log_file, "could not allocate ssd hwmon path\n");
					}
					exit_temperature();
				}
				strcpy(ssd_hwmon_path, buf);
				buf[ DEV_NVME_HWMON_IND_POS + 1 ] = '\0';
				ssd_hwmon_dir_fd = open(buf, O_DIRECTORY | __O_PATH);
				if (ssd_hwmon_dir_fd == -1) {
					if (log_file != NULL) {
						fprintf(log_file, "path open of ssd hwmon dir %s failed\n", buf);
					}
					exit_temperature();
				}
				break;
			}
		}

		/* scan temp#_label with the Composite item */

		if (ssd_hwmon_path != NULL && ssd_hwmon_dir_fd != -1) {
			int ssd_ind = 0;
			int ssd_sensor_ind = 0;
			for (i = 1; i < 10; i++) {
				ssd_hwmon_path[ DEV_NVME_HWMON_TEMP_IND_POS ] = (char) ('0' + i);
				if (debug && log_file != NULL) {
					fprintf(log_file, "temp scan, checking for '%s'\n", ssd_hwmon_path);
				}
				f = fopen(ssd_hwmon_path, "r");
				if (f == NULL) {
					continue;
				}
				if (fgets(buf, MAX_BUF, f) != NULL) {
					if (strncmp(buf, "Comp", 4) == 0) {
						ssd_ind = i;
						if (debug && log_file != NULL) {
							fprintf(log_file, "found ssd composite %d with %s\n", i, buf);
						}
						/* break; */ /* XXX */
					} else if (ssd_sensor_ind <= 0 && strncmp(buf, "Sens", 4) == 0) {
						ssd_sensor_ind = i;
						if (debug && log_file != NULL) {
							fprintf(log_file, "found ssd sensor %d with %s\n", i, buf);
						}
					}
				}
				fclose(f);
			}
			if (ssd_ind <= 0) {
				ssd_ind = ssd_sensor_ind;
			}
			if (ssd_ind > 0) {
				/* found the ssd temperature item */
				/* change the end of the path from "label" to "input" to read the values */
				ssd_hwmon_path[ DEV_NVME_HWMON_TEMP_IND_POS ] = (char) ('0' + ssd_ind);
				strcpy(&ssd_hwmon_path[ DEV_NVME_HWMON_TEMP_IND_POS+2 ], "input");
				if (debug && log_file != NULL) {
					fprintf(log_file, "using ssd %s\n", ssd_hwmon_path);
				}
				hwmon_ssd_temp_path = ssd_hwmon_path;
				if (check_ssd_temp() < 0) {
					hwmon_ssd_temp_path = NULL;
				}
				if (ssd_ind > 0) {
					hwmon_ssd_temp_path = (char *) malloc(20);
					if (hwmon_ssd_temp_path != NULL) {
						strcpy(hwmon_ssd_temp_path, "temp#_input");
						hwmon_ssd_temp_path[ 4 ] = (char) ('0' + ssd_ind);
						if (debug && log_file != NULL) {
							fprintf(log_file, "using ssd temp %s\n", hwmon_ssd_temp_path);
						}
					}
				}
			}

			/* check that we found something */

			if (ssd_ind <= 0) {
				if (debug && log_file != NULL) {
					fprintf(log_file, "did not find good hmon ssd temp item\n");
				}
			}
		}

		hwmon_ssd_temp_dir_fd = ssd_hwmon_dir_fd;

		/* fall back to using the sensors utility */

		if (source == NO_SOURCE) {
			source = SENSORS_SOURCE;
		}

		/* log the source */

		if (log_file != NULL) {
			fprintf(log_file, " cpu temp using %s source\n", temperature_source_names[ source ]);
			fflush(log_file);
		}
	}

	/* read the core temperatures using sys dev files */

	if (source == THINKPAD_SOURCE) {
		int fd = openat(thinkpad_hwmon_dir_fd, &hwmon_path[ THINKPAD_HWMON_IND_POS + 2 ], O_RDONLY);
		if (fd != -1) {
			int len = read(fd, buf, MAX_BUF - 1);
			if (len > 0) {
				if (len > MAX_BUF - 1) len = MAX_BUF - 1;
				buf[ len ] = '\0';
				result = atoi(buf) / 1000;
				if (debug && log_file != NULL) {
					fprintf(log_file, "read thinkpad CPU temp value %d\n", result);
				}
			}
			close(fd);
		}
		return result;
	}

	if (source == SYS_DEV_SOURCE) {
		int fd;
		int len;
		for (i = temp_min_ind; i <= temp_max_ind; i++) {
			if (((1ull << i) & temp_set) != 0) {
				if (i < 10) {
					path = hwmon_path;
					hwmon_path[ TEMP_IND_POS ] = (char) ('0' + i);
				} else {
					path = hwmon_path2;
					path[ TEMP_IND_POS ] = (char) ('0' + i/10);
					path[ TEMP_IND_POS+1 ] = (char) ('0' + i%10);
				}
				fd = openat(generic_hwmon_dir_fd, &path[ HWMON_IND_POS + 2 ], O_RDONLY);
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

	pclose(f);

	return result;
}

/* Read an interval */
/*   return TRUE and read the value if id matches the name */
/*   return FALSE otherwise */

static gboolean
check_read_interval(const char *setup_name, const char *id, const char *interval_name, int *interval_ptr, int min_val, int max_val, const char *units, const char *buf, int len)
{
	if (strcmp(id, interval_name) != 0) {
		return FALSE;
	}
	if (len == 0 || !isdigit(buf[0])) {
		if (log_file != NULL) {
			fprintf(log_file, "Setup file '%s' has '%s' without numeric value.\n", setup_name, id);
		}
	} else {
		*interval_ptr = atoi(buf);
		if (*interval_ptr < min_val) *interval_ptr = min_val;
		if (*interval_ptr > max_val) *interval_ptr = max_val;
		if (debug && log_file != NULL) fprintf(log_file, "Set '%s' to %d %s.\n", id, *interval_ptr, units);
	}
	return TRUE;
}

/* Read a boolean */

static gboolean
check_read_boolean(const char *setup_name, const char *id, const char *flag_name, int *flag_ptr, const char *buf, int len)
{
	if (strcmp(id, flag_name) != 0) {
		return FALSE;
	}

	*flag_ptr =
		((len == 0 ||
		  (isdigit(buf[0]) && atoi(buf) > 0) ||
		  (buf[0] == 'y' || buf[0] == 't'))? 1: 0);

	if (*flag_ptr == 0 && buf[0] != '0' && buf[0] != 'n' && buf[0] != 'f') {
		if (log_file != NULL) {
			fprintf(log_file, "Setup file '%s' has '%s' without boolean value.\n", setup_name, id);
		}
	}

	if (debug && log_file != NULL) {
		fprintf(log_file, "Set '%s' to %d.\n", flag_name, *flag_ptr);
	}

	return TRUE;
}

/* Update settings */
/*   All settings except temp are responsible for their leading space */
/*   to simplify formatting with optional settings */

static void
update_settings()
{
	temp_text = (do_unicode? "\xF0\x9F\x8C\xA1": "Temp");
	gpu_text = (do_unicode? " \xF0\x9F\x8E\xA8": " G");
	ssd_text = (do_unicode? " \xF0\x9F\x96\xB4": " H");
	fan_text = (do_unicode? " \xE2\x9D\x83": " Fan");
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
		update_settings();
		return;
	}

	temperature_interval = fan_check_interval = gpu_temp_interval = ssd_temp_interval = warning_interval = -1;
	ssd_hide_temperature = -1;

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
		} else if (check_read_boolean(setup_name, id, "beep", &do_beep, buf, len)) {
			;
		} else if (check_read_boolean(setup_name, id, "unicode", &do_unicode, buf, len)) {
			;
		} else if (check_read_interval(setup_name, id, "interval", &interval, 1, MAX_INTERVAL, "seconds", buf, len)) {
			if (interval < 1) interval = 1;
		} else if (check_read_interval(setup_name, id, "tempinterval", &temperature_interval, 0, MAX_INTERVAL, "seconds", buf, len)) {
			;
		} else if (check_read_interval(setup_name, id, "gpuinterval", &gpu_temp_interval, 0, MAX_INTERVAL, "seconds", buf, len)) {
			;
		} else if (check_read_interval(setup_name, id, "ssdinterval", &ssd_temp_interval, 0, MAX_INTERVAL, "seconds", buf, len)) {
			;
		} else if (check_read_interval(setup_name, id, "ssdhidetemp", &ssd_hide_temperature, 0, MAX_WARNING_TEMPERATURE, "degrees", buf, len)) {
			;
		} else if (check_read_interval(setup_name, id, "faninterval", &fan_check_interval, 0, MAX_INTERVAL, "seconds", buf, len)) {
			;
		} else if (check_read_interval(setup_name, id, "warn", &warning_temperature, 0, MAX_WARNING_TEMPERATURE, "degrees", buf, len)) {
			;
		} else if (check_read_interval(setup_name, id, "warninterval", &warning_interval, 0, MAX_INTERVAL, "seconds", buf, len)) {
			;
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

	if (temperature_interval < 0) {
		temperature_interval = (DEFAULT_TEMPERATURE_INTERVAL * interval) / DEFAULT_INTERVAL;
	}
	if (fan_check_interval < 0) {
		fan_check_interval = (DEFAULT_FAN_CHECK_INTERVAL * temperature_interval) / DEFAULT_TEMPERATURE_INTERVAL;
	}
	if (warning_interval < 0) {
		warning_interval = (DEFAULT_WARNING_INTERVAL * interval) / DEFAULT_INTERVAL;
	}
	if (gpu_temp_interval < 0) {
		gpu_temp_interval = temperature_interval;
		if (gpu_temp_interval < fan_check_interval) gpu_temp_interval = fan_check_interval;
	}
	if (ssd_temp_interval < 0) {
		ssd_temp_interval = temperature_interval;
		if (ssd_temp_interval < fan_check_interval) ssd_temp_interval = fan_check_interval;
	}
	if (ssd_hide_temperature < 0) {
		ssd_hide_temperature = 40;
	}

	update_settings();

	if (log_file != NULL) {
		fprintf(log_file, "Read setup file '%s' at %s.\n", setup_name, show_time());
		fprintf(log_file, " interval %d seconds\n", interval);
		fprintf(log_file, " small change temperature interval %d seconds\n", temperature_interval);
		fprintf(log_file, " fan check interval %d seconds\n", fan_check_interval);
		fprintf(log_file, " gpu check interval %d seconds\n", gpu_temp_interval);
		fprintf(log_file, " ssd check interval %d seconds\n", ssd_temp_interval);
		fprintf(log_file, " ssd hide temperature at or below %d degrees\n", ssd_hide_temperature);
		fprintf(log_file, " warn at cpu temp %d degrees\n", warning_temperature);
		fprintf(log_file, " warn again after %d seconds\n", warning_interval);
		fprintf(log_file, " play sound '%s'\n", (sound_name? sound_name: "<none>"));
		fprintf(log_file, " beep '%d'\n", do_beep);
		fprintf(log_file, " unicode '%d'\n", do_unicode);
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
	static int last_gpu_temp = 0;
	static int last_ssd_temp = 0;
	static int last_fan_speed = -1;
	static time_t last_warning_time = 0;
	static time_t last_fan_check_time = 0;
	static time_t last_gpu_temp_check_time = 0;
	static time_t last_ssd_temp_check_time = 0;
	static time_t last_repaint_time = 0;
	static time_t current_time;
	int temperature;
	int ssd_temp;
	int fan_speed;
	enum open_window_enum { TEMP_BUF_LEN = 80 };
	char temp_buf[ TEMP_BUF_LEN ];

	current_time = time(NULL);

	temperature = check_temperature();

	fan_speed = last_fan_speed;

	ssd_temp = last_ssd_temp;

	if (force_update || temperature != last_temperature || current_time > last_fan_check_time + ((fan_speed == 0 && temperature <= 46)? 3: 1) * fan_check_interval) {
		fan_speed = check_fan_speed();
		last_fan_check_time = current_time;
	}

	if (force_update || temperature != last_temperature || fan_speed != last_fan_speed ||
            current_time > last_ssd_temp_check_time + ((fan_speed == 0 && temperature <= 46)? 3: 1) * ssd_temp_interval) {
		ssd_temp = check_ssd_temp();
		if (ssd_temp <= ssd_hide_temperature) {
			/* don't show normal idle temperatures */
			ssd_temp = 0;
		}
		last_ssd_temp_check_time = current_time;
	}

	if (debug && log_file != NULL) {
		fprintf(log_file, "old temp %d new temp %d old gpu %d old ssd %d old fan %d new fan %d at %s\n",
			last_temperature, temperature, last_gpu_temp, last_ssd_temp, last_fan_speed, fan_speed, show_time());
		fflush(log_file);
	}

	if (temperature >= warning_temperature && current_time >= last_warning_time + warning_interval) {
		last_warning_time = current_time;
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

	if ((temperature != last_temperature || fan_speed != last_fan_speed || ssd_temp != last_ssd_temp) &&
	    (force_update ||
	     abs(temperature - last_temperature) > 2 ||
	     temperature >= warning_temperature ||
	     last_temperature >= warning_temperature ||
	     ssd_temp != last_ssd_temp ||
	     fan_speed != last_fan_speed ||
	     current_time >= last_repaint_time + temperature_interval)) {

		if (last_label != NULL) {
			gtk_container_remove (GTK_CONTAINER (event_box), last_label);
			if (debug && log_file != NULL) {
				fprintf(log_file, "remove label for state %d\n", last_temperature);
			}
		}

		last_temperature = temperature;
		last_ssd_temp = ssd_temp;
		last_fan_speed = fan_speed;
		last_repaint_time = current_time;
		if (temperature > 0) {
			const char *gpu_mark;
			char ssd_buf[ TEMP_BUF_LEN ];
			if (current_time > last_gpu_temp_check_time + gpu_temp_interval) {
				last_gpu_temp = check_gpu_temp();
				last_gpu_temp_check_time = current_time;
			}
			gpu_mark = ((last_gpu_temp > 0)? gpu_text: "");
			ssd_buf[0] = '\0';
			if (last_ssd_temp > 0) {
				sprintf(ssd_buf, "%s %d", ssd_text, last_ssd_temp);
			}
			if (fan_speed > 0) {
				sprintf(temp_buf, "%s %d%s%s%s %d", temp_text, temperature, gpu_mark, ssd_buf, fan_text, fan_speed);
			} else {
				sprintf(temp_buf, "%s %d%s%s", temp_text, temperature, gpu_mark, ssd_buf);
			}
			last_label = gtk_label_new (temp_buf);
			gtk_container_add (GTK_CONTAINER (event_box), last_label);
			if (debug && log_file != NULL) {
				fprintf(log_file, "add label for temp %d fan %d\n", last_temperature, fan_speed);
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
		GdkEventButton  *event,
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

	temperature_interval = DEFAULT_TEMPERATURE_INTERVAL;

	fan_check_interval = DEFAULT_FAN_CHECK_INTERVAL;

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
