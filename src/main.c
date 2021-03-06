/*
 * main.c
 *
 *      Entry point to fileguard cli executable
 */
#include "log.h"
#include "fileguard.h"

/* used for signal handling */
static volatile sig_atomic_t sc = true;

/* global file and watch descriptors to be cleaned up */
static int fd, wd;

/* represents an array filled with events for watching */
static const char *
events[] =
{
   "IN_ACCESS",             // File accessed
   "IN_ATTRIB",             // Metadata changes
   "IN_CLOSE_WRITE",        // File opened for writing was closed.
   "IN_CLOSE_NOWRITE",      // File or directory not opened for writing was closed.
   "IN_CREATE",             // File/directory created
   "IN_DELETE",             // File/directory deleted
   "IN_DELETE_SELF",        // Watched inode deleted
   "IN_MODIFY",             // File modified
   "IN_MOVE_SELF",          // Watched inode moved
   "IN_MOVED_FROM",         // Directory with old filename when a file is renamed.
   "IN_MOVED_TO",           // Directory with new filename when a file is renamed.
   "IN_OPEN",               // File/directory is opened
   "IN_UNMOUNT",            // Filesystem unmounted
};


/* displays help menu */
static void
usage(char * application_name)
{
    /* print help to STDOUT */
    fprintf(stdout, "Usage: (note that these are optional arguments)\n\n\t %s -[h|v|n] <other.yaml>\n\n"
            "-h : Display this help message\n"
            "-v : Turns ON verbosity\n"
            "-n : Turns ON libnotify notifications\n"
            , application_name);
}


/* cleanup routine called by atexit */
static void
cleanup(void)
{
    inotify_rm_watch(fd, wd);
}


/* signal trapping routine that calls cleanup routine */
static void
catch_sig(int s)
{
    log_warn("Signal %i caught! Cleaning up...\n", s);
    sc = false;
    cleanup();
    exit(EXIT_SUCCESS);
}


int
main(int argc, char **argv)
{
    /* represents a inotify_event struct */
    struct inotify_event *ev;

    /* default yaml file to check, parse and tokenize */
    char *yaml_target = NULL;

    /* return ints */
    int rd;
    int verbose;
    int notifier;

    /* initialization */
    errno = 0;
    atexit(cleanup);
    signal(SIGINT, catch_sig);

    /* argument parsing */
    int c;
    while ((c = getopt(argc, argv, "hvn")) != -1) {
       switch (c) {

           /* display help menu */
           case 'h':
               usage(argv[0]);
               exit(EXIT_SUCCESS);

           /* set verbosity flag */
           case 'v':
               log_debug("Setting verbosity flag");
               verbose = 1;
               break;

           /* set notifier flag */
           case 'n':
               log_debug("Setting notifier flag");
               notifier = 1;
               break;

           /* default short to usage */
           default:
               usage(argv[0]);
               exit(EXIT_FAILURE);
       }
   }

    /* initialize verbosity */
    if (!verbose)
        log_set_quiet(1);
    else
        log_set_level(3);


    /* check argument list for any yaml file configuration changes */
    for (int i = 1; i < argc; i++) {
        char *dot = strrchr(argv[i], '.');
        if (dot && strcmp(dot, "yaml")) {
            log_info("yaml file: %s", argv[i]);
            yaml_target = argv[i];
            break;
        } else {
            yaml_target = CONFIG_FILE;
            break;
        }
    }

   /* print usage and exit if no yaml config found */
   if (yaml_target == NULL) {
      usage(argv[0]);
      fprintf(stderr, "\nError: No required YAML configuration supplied.\n");
      exit(EXIT_FAILURE);
   }

    printf("Initializing fileguard!\n");

    /* perform file-checking */
    file_t yaml;
    yaml = file_check(yaml_target);
    if (yaml.flag < 0) {
        fprintf(stderr, "Error %i: Unable to open file: %s.\n", yaml.flag, yaml.data);

        /* create new file and then quit */
        log_debug("Creating configuration file for you...\n");
        file_t new_file = create_file(CONFIG_FILE, NULL);
        exit(EXIT_FAILURE);
    }

    log_debug("File %s found", yaml.data);

    /* parse our yaml configuration file */
    yaml_t y;
    y = parse_yaml_config(yaml_target);
    if (y.return_flag == false) {
        perror("Could not initialize YAML parser. Reason");
        exit(EXIT_FAILURE);
    }

    log_debug("Parsed YAML file: inode: %s event: %s execute: %s",
              y.inode, y.event, y.action);

    /* check if user-specified event is supported */
    int event_flag;
    for (int i = 0; events[i] != NULL; i++) {
        if (strcmp(y.event, events[i]) == 0) { event_flag = 0; break; }
        else                                 { event_flag = 1; continue; }
    }

    if (event_flag) {
        fprintf(stderr, "\nUnknown inode event supplied: %s\n", y.event);
        exit(EXIT_FAILURE);
    }

    log_info("inode event found! Continuing.");

    /* check if specified inode is an inode */
    file_t inode_check;
    inode_check = file_check(y.inode);
    if (inode_check.flag < 0) {
        fprintf(stderr, "Error %i: Unable to open inode \"%s\": %s\n", inode_check.flag, y.inode, inode_check.data);
        exit(EXIT_FAILURE);
    }

    log_info("inode successfully found and opened.");

    /* check for proper permissions */
    int iperm;
    iperm = check_inode_permissions(y.inode);
    if (iperm < 0) {
        perror("Permission check for inode failed! Reason");
        exit(EXIT_FAILURE);
    }

    /* initialize inotify */
    fd = inotify_init();
    if (fd < 0)
        perror("Could not initialize inotify. Reason");


    /* add a file watcher */
    wd = inotify_add_watch(fd, y.inode, IN_ALL_EVENTS);
    if (wd < 0)
        perror("Could not add watch. Reason");


    /* concatenate action string */
    char *mem, *str, *prepend, *command;
    mem = str = strdup(y.action);
    prepend = strtok(str, " ");
    command = strtok(NULL, "\"");

    if (command == NULL) {
        fprintf(stderr, "Command/path cannot be none. Exiting.\n");
        exit(EXIT_FAILURE);
    }

    /* event buffers */
    char buf[BUF_LEN] __attribute__ ((aligned(8)));
    char *p;

    /* main event loop */
    while (sc) {
        /* read from inotify fd */
        rd = read(fd, buf, BUF_LEN);
        if (rd == 0)
            fprintf(stdout, "read() tossed back a 0");
        else {
            perror("Couldn't read event. Reason");
            break;
        }

        /* process events and do what's necessary according to YAML */
        for (p = buf; p < buf + rd;) {

            /* made local so that data gets re-initialized within scope */
            struct tm * timeinfo;
            time_t rawtime;
            char *ltime, *eventstr;
            const char *event;

            /* get time, and create new string */
            timeinfo = gettime(rawtime);
            ltime = asctime(timeinfo);
            log_debug("Time of event: %s", ltime);

            /* copy over inotify_event */
            ev = (struct inotify_event *) p;

            /* display event through terminal*/
            event = get_event(ev);
            printf("%s event ocurred\n", event);

            /* raise notification if flag was set */
            log_debug("Raising notification");
            if (notifier)
                raise_notification(ltime, event);

            /* check command, if the specified event matches the current event and execute accordingly */
            if ((strcmp(prepend, "execute") == 0) && (strcmp(y.event, event) == 0))
                system((const char *) command);
            else if (strcmp(prepend, "log") == 0 ) {

                if ((eventstr = malloc(strlen(ltime) + strlen(event) + 2)) != NULL) {
                    eventstr[0] = '\0';
                    strcat(eventstr, ltime);
                    strcat(eventstr, event);
                    strcat(eventstr, "\n");
                } else {
                    perror("malloc failed. Reason");
                    exit(EXIT_FAILURE);
                }

                /* create a log file, with contents of eventstr */
                file_t tmpLog = create_file(command, eventstr);
                if (tmpLog.flag < 0 ) {
                    perror("Couldn't create log file. Reason");
                    exit(EXIT_FAILURE);
                }
                free(eventstr);
            }
            p += sizeof(struct inotify_event) + ev->len;
        }
    }

    /* success */
    exit(EXIT_SUCCESS);
}
