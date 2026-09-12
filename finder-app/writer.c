/*
 * writer.c
 *
 * Assignment 2: File Operations and Cross Compiler
 *
 * C replacement for the writer.sh test script from Assignment 1.
 * Usage: ./writer <file> <string>
 *
 * Unlike writer.sh, this program does NOT create directories that don't
 * exist -- the caller is responsible for ensuring the target directory
 * exists. Attempting to write to a nonexistent directory is treated as
 * an error and logged accordingly.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <syslog.h>

int main(int argc, char *argv[])
{
    /* Set up syslog under the USER facility, per assignment spec */
    openlog("writer", LOG_PID | LOG_CONS, LOG_USER);

    if (argc != 3) {
        syslog(LOG_ERR, "Invalid number of arguments: %d (expected 2: <file> <string>)",
               argc - 1);
        fprintf(stderr, "Usage: %s <file> <string>\n", argv[0]);
        closelog();
        return 1;
    }

    const char *writefile = argv[1];
    const char *writestr  = argv[2];

    FILE *fp = fopen(writefile, "w");
    if (fp == NULL) {
        /* errno gives us the specific reason (e.g. ENOENT if the
         * containing directory doesn't exist, EACCES for permissions) */
        syslog(LOG_ERR, "Could not open file '%s' for writing: %s",
               writefile, strerror(errno));
        fprintf(stderr, "Error: could not open file '%s': %s\n",
                writefile, strerror(errno));
        closelog();
        return 1;
    }

    /* Log at DEBUG level per spec: "Writing <string> to <file>" */
    syslog(LOG_DEBUG, "Writing %s to %s", writestr, writefile);

    size_t len = strlen(writestr);
    size_t written = fwrite(writestr, sizeof(char), len, fp);

    if (written != len) {
        syslog(LOG_ERR, "Failed to write full string to '%s': %s",
               writefile, strerror(errno));
        fprintf(stderr, "Error: failed to write full string to '%s'\n", writefile);
        fclose(fp);
        closelog();
        return 1;
    }

    if (fclose(fp) != 0) {
        syslog(LOG_ERR, "Error closing file '%s': %s", writefile, strerror(errno));
        fprintf(stderr, "Error: could not close file '%s': %s\n",
                writefile, strerror(errno));
        closelog();
        return 1;
    }

    closelog();
    return 0;
}
