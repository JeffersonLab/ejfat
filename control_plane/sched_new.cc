//
// Copyright 2022, Jefferson Science Associates, LLC.
// Subject to the terms in the LICENSE file found in the top-level directory.
//
// EPSCI Group
// Thomas Jefferson National Accelerator Facility
// 12000, Jefferson Ave, Newport News, VA 23606
// (757)-269-7100
//
// Scheduler Simulation for Control Plane with GRPC input from ERSAP backends.
// Reads pid control signals from compute farm and adjusts scheduling density
// 

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fstream>
#include <iostream>
#include <inttypes.h>
#include <time.h>
#include <chrono>
#include <ctime>
#include <random>
#include <functional>
#include <array>
#include <algorithm>
#include <map>
#include <thread>
#include <unordered_map>

// GRPC stuff
#include <grpcpp/grpcpp.h>
#include "lbControlPlaneEsnet.grpc.pb.h"
#include "lb_cplane_esnet.h"


using namespace std;

unsigned seed = std::chrono::system_clock::now().time_since_epoch().count();

void   Usage(void)
{
    char usage_str[] =
        "\nUsage: \n\
        -d debug mode \n\
        -f feedback file   \n\
        -h help \n\n";
        cout<<usage_str;
        cout<<"Required: -f\n";
}

template<class X>
void print(const X& x) {
    size_t N = x.size();
    for(uint16_t i=0;i<N;i++) cout << x[i] << ' '; cout << '\n';
}

static volatile int cpu=-1;




// structure for passing args to thread
typedef struct threadStruct_t {
    uint16_t port;
    BackendReportServiceImpl *pGrpcService;
} threadStruct;


// Thread to monitor all the info coming in from backends and update the control plane
static void *controlThread(void *arg) {

    threadStruct *targ = static_cast<threadStruct *>(arg);
    BackendReportServiceImpl *service = targ->pGrpcService;

    while (true) {
        std::cout << "About to run GRPC server on port" << targ->port << std::endl;
        service->runServer(targ->port, service);
        std::cout << "Should never print this message!!!" << std::endl;
    }

    return (nullptr);
}


int main (int argc, char *argv[])
{
    int optc;
    extern char *optarg;
    extern int   optind, optopt;

    bool passedD=false, passedF=false;

    char     hst_fdbk_t[256] = "";  // node feedback file
    
    while ((optc = getopt(argc, argv, "dhn:f:")) != -1)
    {
        switch (optc)
        {
        case 'h':
            Usage();
            exit(1);
        case 'd':
            passedD = true;
            fprintf(stderr, "-d ");
            break;
        case 'f':
            strcpy(hst_fdbk_t, (const char *) optarg) ;
            passedF = true;
            fprintf(stderr, "-f %s ", hst_fdbk_t);
            break;
        case '?':
            fprintf (stderr, "Unrecognised option: %d\n", optopt);
            Usage();
            exit(1);
        }
    }
    fprintf(stderr, "\n");
    if(!passedF) { Usage(); exit(1); }

    // random device class instance, source of 'true' randomness for initializing random seed
    std::random_device rd; 
    // Mersenne twister PRNG, initialized with seed from previous random device instance
    std::mt19937 gen(rd()); 
    std::uniform_real_distribution<> d(0.0, 1.0);

    //control values from node
    std::map<uint16_t, float> control;
    for(size_t n=0;n<1024;n++) {control[n] = 0;}///////////////
    //schedule density for node
    std::map<uint16_t, float> sched;
    for(size_t n=0;n<1024;n++) {sched[n] = 0;}///////////////
    uint64_t epoch = 0; //for now

    BackendReportServiceImpl service;
    BackendReportServiceImpl *pGrpcService = &service;

    //------------------------------------------------
    // Start thread accept grpc messages, as server
    //------------------------------------------------
    threadStruct *targ = (threadStruct *)calloc(1, sizeof(threadStruct));
    if (targ == nullptr) {
        fprintf(stderr, "out of mem\n");
        return -1;
    }

    targ->port = 50051;
    targ->pGrpcService = pGrpcService;

    pthread_t thd1;
    int status = pthread_create(&thd1, NULL, controlThread, (void *) targ);
    if (status != 0) {
        fprintf(stderr, "\n ******* error creating PID thread ********\n\n");
        return -1;
    }
    //------------------------------------------------


    int64_t totalT = 0, time;
    struct timespec t1, t2, firstT;

    // Get the current time
    clock_gettime(CLOCK_MONOTONIC, &t1);
    firstT = t1;

    // One tricky thing is that the size of "control" must change as backends come and go

    // sched === weight = % of schedule

    while (true) {

        // Delay 2 seconds between actions
        std::this_thread::sleep_for(std::chrono::seconds(2));

        // This needs to be called each loop since it gets a COPY of the current data (for thread safety)
        std::shared_ptr<std::unordered_map<int32_t, BackEnd>> pDataMap = service.getBackEnds();
        size_t num_bes = pDataMap->size(); //number of backends giving feed back this reporting interval

        // Loop over all backends
        for (const std::pair<int32_t, BackEnd>& entry : *(pDataMap.get())) {
            const BackEnd & backend = entry.second;

            // read node feedback: an array of health metrics
            uint16_t n = 0;
            control[n] = backend.getPidError();
            sched[n] = sched[n] == 0 ? 1e-6 : sched[n]; //activate node if not active
            //num_bes++;
            if (passedD) cout << "Received " << n << ", " <<  control[n] << " from backend\n";
            if (passedD) cout << "sched[" << n << "] = " << sched[n] << " ...\n";
            // update weighting for node from control signal
            sched[n] *= (1.0f + control[n]);
            if (passedD) cout << "adjusting sched[" << n << "] = " << sched[n] << " ...\n";
        }

if (passedD) {cout << "read " << control.size() << " controls\n";}
if (passedD) {cout << "control: "; for(size_t n=0;n<sched.size();n++) {cout << control[n] << '\t';} cout << '\n';}
if (passedD) {cout << "normalizing ...\n"; }

        // normalize schedule density
        float nrm_sum;
        nrm_sum = 0;
        for (size_t n=0; n < num_bes; n++) {
            nrm_sum += sched[n];
        }
        nrm_sum = nrm_sum == 0 ? 1 : nrm_sum;
if (passedD) {cout << "nrm_sum = " << nrm_sum << '\n';}
        // ///////////

        for (size_t n=0; n < num_bes; n++) {
            sched[n] /= nrm_sum;
        }

if (passedD) {cout << "density: "; for(size_t n=0;n<num_bes;n++) {cout << sched[n] << '\t';} cout << '\n';}

if (passedD) {cout << "write revised tick schedule ...\n";}
        // write revised tick schedule
        std::map<uint16_t, uint32_t> lb_calendar_table;

        for (uint16_t t=0; t < 512; t++) {
            // random # between 0 & 1
            float r = d(gen);
if (passedD) {cout << "sample = " << r << '\n';}

            // cumulative distribution from iterating over sched weights
            float cd = 0.f;
            uint16_t n;
            n = 0;
            for (size_t ni=0; ni < num_bes; ni++) {
                cd += sched[ni];
                if (passedD) cout << "testing = " << r << " against " << cd << " n = " << n << '\n';
                if (r <= cd) break;
                n++;
            }

            lb_calendar_table[t] = n;

if (passedD) {cout << "sampled index = " << n << '\n';}
cout << "table_add load_balance_calendar_table do_assign_member 0x" << std::hex << epoch << " 0x" << std::hex << t << " => 0x" << std::hex << n << '\n';
        }
        //usleep(100000);
    }
    
    return 0;
}

