//
// Copyright 2022, Jefferson Science Associates, LLC.
// Subject to the terms in the LICENSE file found in the top-level directory.
//
// EPSCI Group
// Thomas Jefferson National Accelerator Facility
// 12000, Jefferson Ave, Newport News, VA 23606
// (757)-269-7100


/**
 * @file This is an example for Vardan or any user of the ET-system-as-a-fifo.
 * Shows how to read the ET system when it's configured as a fifo.
 * Other than that, this program is never used or sun.
 */



#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <getopt.h>
#include <limits.h>
#include <time.h>
#include <string.h>
#include <pthread.h>
#include <fstream>

#include "et.h"
#include "et_fifo.h"

/* prototype */
static void *signal_thread (void *arg);

/**
 * This routine takes a pointer and writes out the desired number of bytes
 * to the given file.
 *
 * @param data      data to print out.
 * @param bytes     number of bytes to print in hex.
 * @param filename  file name.
 * @param loop      fifo entry count # (loop).
 * @param dataId    data Id to tack onto filename.
 */
static void writeBufToFile(const char *data, uint32_t bytes, const char *filename, int loop, uint16_t dataId) {

    if (bytes < 1) {
        fprintf(stderr, "<no bytes to write ... >\n");
        return;
    }

    char name[512];
    memset(name, 0, 512);
    sprintf(name, "%s_%d_%hu", filename, loop, dataId);

    std::fstream file;
    file.open(name, std::ios::app | std::ios::binary);
    file.write(data, bytes);
    file.close();
}




int main(int argc,char **argv) {

    int             i, j, c, i_tmp, status, numRead, locality;
    int             errflg=0, verbose=0, delay=0, writeOut=0;
    int             debugLevel = ET_DEBUG_ERROR;
    char            stationName[ET_STATNAME_LENGTH], et_name[ET_FILENAME_LENGTH];

    pthread_t       tid;
    et_sys_id       id;
    et_fifo_id      fid;
    et_fifo_entry   *entry;

    et_openconfig   openconfig;
    sigset_t        sigblock;
    struct timespec getDelay;
    struct timespec timeout;
    struct timespec t1, t2;

    /* statistics variables */
    double          rate=0.0, avgRate=0.0;
    int64_t         count=0, totalCount=0, totalT=0, time, time1, time2, bytes=0, totalBytes=0;


    /* 4 multiple character command-line options */
    static struct option long_options[] =
            {{0,0,0,0}};

    memset(et_name, 0, ET_FILENAME_LENGTH);
    strcpy(stationName, "Users");

    while ((c = getopt_long_only(argc, argv, "vwhn:s:f:d:", long_options, 0)) != EOF) {

        if (c == -1)
            break;

        switch (c) {

            case 'd':
                i_tmp = atoi(optarg);
                if (i_tmp >= 0) {
                    delay = i_tmp;
                } else {
                    printf("Invalid argument to -d. Must be >= 0 millisec\n");
                    exit(-1);
                }
                break;

            case 'f':
                if (strlen(optarg) >= ET_FILENAME_LENGTH) {
                    fprintf(stderr, "ET file name is too long\n");
                    exit(-1);
                }
                strcpy(et_name, optarg);
                break;

            case 'v':
                verbose = 1;
                debugLevel = ET_DEBUG_INFO;
                break;

            case 'w':
                writeOut = 1;
                break;

            case ':':
            case 'h':
            case '?':
            default:
                errflg++;
        }
    }


    if (optind < argc || errflg || strlen(et_name) < 1) {
        fprintf(stderr,
                "\nusage: %s  %s\n%s\n%s\n",
                argv[0], "-f <ET name>",
                "                     [-h] [-v] [-w]",
                "                     [-d <delay ms>]");

        fprintf(stderr, "          -h    help\n\n");
        fprintf(stderr, "          -w    write out buffers into files\n");
        fprintf(stderr, "          -f    ET system's (memory-mapped file) name\n");
        fprintf(stderr, "          -v    verbose output (also prints data if reading with -read)\n");
        fprintf(stderr, "          -d    delay between fifo gets in milliseconds\n");

        fprintf(stderr, "          This consumer reads data written by ersap packet assembler into ET fifo system\n\n");

        exit(2);
    }


    timeout.tv_sec  = 2;
    timeout.tv_nsec = 0;

    /* delay is in milliseconds */
    if (delay > 0) {
        getDelay.tv_sec = delay / 1000;
        getDelay.tv_nsec = (delay - (delay / 1000) * 1000) * 1000000;
    }

    /*************************/
    /* setup signal handling */
    /*************************/
    /* block all signals */
    sigfillset(&sigblock);
    status = pthread_sigmask(SIG_BLOCK, &sigblock, NULL);
    if (status != 0) {
        printf("%s: pthread_sigmask failure\n", argv[0]);
        exit(1);
    }

    /* spawn signal handling thread */
    pthread_create(&tid, NULL, signal_thread, (void *)NULL);


    /******************/
    /* open ET system */
    /******************/
    et_open_config_init(&openconfig);

    /* debug level */
    et_open_config_setdebugdefault(openconfig, debugLevel);

    printf("Trying to open ET = %s\n", et_name);
    if (et_open(&id, et_name, openconfig) != ET_OK) {
        printf("%s: et_open problems\n", argv[0]);
        exit(1);
    }
    et_open_config_destroy(openconfig);

    /*-------------------------------------------------------*/

    /* set level of debug output (everything) */
    et_system_setdebug(id, debugLevel);

    /***********************/
    /* Use FIFO interface  */
    /***********************/
    status = et_fifo_openConsumer(id, &fid);
    if (status != ET_OK) {
        printf("%s: et_fifo_open problems\n", argv[0]);
        exit(1);
    }

    /* no error here */
    numRead = et_fifo_getEntryCapacity(fid);

    entry = et_fifo_entryCreate(fid);
    if (entry == NULL) {
        printf("%s: et_fifo_open out of mem\n", argv[0]);
        exit(1);
    }


    /* read time for future statistics calculations */
    clock_gettime(CLOCK_REALTIME, &t1);
    time1 = 1000L*t1.tv_sec + t1.tv_nsec/1000000L; /* milliseconds */

    int bufId, hasData;
    size_t len;
    int *data, loop=0;

    while (1) {

        /**************/
        /* get events */
        /**************/

        /* Example of reading a fifo entry */
        status = et_fifo_getEntry(fid, entry);
        if (status != ET_OK) {
            printf("%s: error getting events\n", argv[0]);
            goto error;
        }

        /*******************/
        /* read/print data */
        /*******************/
        // All events in fifo entry
        et_event** evts = et_fifo_getBufs(entry);

        // Look at each event/buffer
        for (j = 0; j < numRead; j++) {
            // Does buffer have data? If not, it's nothing following does either
            hasData = et_fifo_hasData(evts[j]);
            if (!hasData) {
                break;
            }


            // Data associated with this event
            et_event_getdata(evts[j], (void **) &data);
            // Length of data associated with this event
            et_event_getlength(evts[j], &len);
            // Id associated with this buffer in this fifo entry
            bufId = et_fifo_getId(evts[j]);

            bytes += len;
            totalBytes += len;

            if (verbose) {
                printf("buf id = %d, has data = %s, len = %lu\n", bufId, hasData ? "true" : "false", len);
            }

            if (writeOut) {
                writeBufToFile((char *)data, len, "./outputFile", loop, bufId);
            }
        }
        loop++;

        /*******************/
        /* put events */
        /*******************/

        /* putting array of events */
        status = et_fifo_putEntry(entry);
        if (status != ET_OK) {
            printf("%s: error getting events\n", argv[0]);
            goto error;
        }

        count += numRead;

        end:

        /* statistics */
        clock_gettime(CLOCK_REALTIME, &t2);
        time2 = 1000L*t2.tv_sec + t2.tv_nsec/1000000L; /* milliseconds */
        time = time2 - time1;
        if (time > 5000) {
            /* reset things if necessary */
            if ( (totalCount >= (LONG_MAX - count)) ||
                 (totalT >= (LONG_MAX - time)) )  {
                bytes = totalBytes = totalT = totalCount = count = 0;
                time1 = time2;
                continue;
            }

            rate = 1000.0 * ((double) count) / time;
            totalCount += count;
            totalT += time;
            avgRate = 1000.0 * ((double) totalCount) / totalT;

            /* Event rates */
            printf("\n %s Events: %3.4g Hz,  %3.4g Avg.\n", argv[0], rate, avgRate);

            /* Data rates */
            rate    = ((double) bytes) / time;
            avgRate = ((double) totalBytes) / totalT;
            printf(" %s Data:   %3.4g kB/s,  %3.4g Avg.\n\n", argv[0], rate, avgRate);

            bytes = count = 0;

            clock_gettime(CLOCK_REALTIME, &t1);
            time1 = 1000L*t1.tv_sec + t1.tv_nsec/1000000L;
        }

        // Delay before calling et_fifo_getEntry again
        if (delay > 0) {
            nanosleep(&getDelay, NULL);
        }

    } /* while(1) */

    error:
    // Although not necessary at this point
    et_fifo_freeEntry(entry);

    printf("%s: ERROR\n", argv[0]);
    return 0;
}



/************************************************************/
/*              separate thread to handle signals           */
static void *signal_thread (void *arg) {

    sigset_t        signal_set;
    int             sig_number;

    sigemptyset(&signal_set);
    sigaddset(&signal_set, SIGINT);

    /* Wait for Control-C */
    sigwait(&signal_set, &sig_number);

    printf("Got control-C, exiting\n");
    exit(1);
}
