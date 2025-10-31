#include <stdio.h>
#include <syslog.h>
#include <stdlib.h>

int main(int argc, char** argv) {
    openlog("writer", LOG_PID | LOG_CONS, LOG_USER);

    if(argc < 3) {
        syslog(LOG_ERR, "please provide filepath and text arguments");
        closelog();
        return 1;
    }

    char* filepath = argv[1];
    char* text = argv[2];

    syslog(LOG_DEBUG, "Writing %s to %s", text, filepath);

    FILE *file = fopen(filepath, "w");

    if(file == NULL) {
        syslog(LOG_ERR, "failed to open file %s", filepath);
        closelog();
        return 1;
    }

    if(fprintf(file, "%s", text) < 0) {
        syslog(LOG_ERR, "failed to write the text");
        fclose(file);
        closelog();
        return 1;
    }

    syslog(LOG_DEBUG, "write success");

    fclose(file);
    closelog();
    return 0;
}
