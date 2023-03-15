//      Scheduler Simulation for Control Plane - Volkswagon  Quality
//      works with do_pid_feed script
// reads pid control signals from compute farm and adjusts scheduling density
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

    ifstream hst_fdbk;
    //control values from node
    std::map<uint16_t, float> control;
    for(size_t n=0;n<1024;n++) {control[n] = 0;}///////////////
    //schedule density for node
    std::map<uint16_t, float> sched;
    for(size_t n=0;n<1024;n++) {sched[n] = 0;}///////////////
    uint64_t epoch = 0; //for now
    while(true)
    {
        //load the feedback from each node
        hst_fdbk.open(hst_fdbk_t,std::ios::in); //nodes that have reported pid 'errors'
        uint16_t num_nds = 0; //number of nodes giving feed back this reporting interval
        num_nds = 0;
        while(hst_fdbk.good()) {
            //read node feedback: an array of health metrics
            uint16_t n = 0;
            hst_fdbk >> n >> control[n];
            sched[n] = sched[n] == 0 ? 1e-6 : sched[n]; //activate node if not active
            if(!hst_fdbk.good()) break;
            num_nds++;
if(passedD) cout << "read " << n << ", " <<  control[n] << " from feedback file\n"; 
            // update weighting for node from control signal 
            //occurs check:
            //sched[n] = sched[n]==0 ? 1/1e6 : sched[n];    /////////////////////////      
if(passedD) cout << "sched["<< n <<"] = " << sched[n] << " ...\n"; 
            sched[n] *= (1.0f + control[n]);
if(passedD) cout << "adjusting sched["<<n<<"] = " << sched[n] << " ...\n"; 
        }
        hst_fdbk.close();
if(passedD) {cout << "read " << control.size() << " controls\n";}
if(passedD) {cout << "control: "; for(size_t n=0;n<sched.size();n++) {cout << control[n] << '\t';} cout << '\n';} 
if(passedD) {cout << "normalizing ...\n"; }
        // normalize schedule density
        float nrm_sum;
        nrm_sum = 0;
        for(size_t n=0;n<sched.size();n++) {nrm_sum += sched[n];}  nrm_sum = nrm_sum == 0 ? 1 : nrm_sum; /////////////
if(passedD) {cout << "nrm_sum = " << nrm_sum << '\n';}
        for(size_t n=0;n<sched.size();n++) {sched[n] /= nrm_sum;} 
if(passedD) {cout << "density: "; for(size_t n=0;n<sched.size();n++) {cout << sched[n] << '\t';} cout << '\n';} 
if(passedD) {cout << "write revised tick schedule ...\n";}
        //write revised tick schedule
#if 1
        for(size_t t=0;t<512;t++) {
            float r = d(gen);
if(passedD) {cout << "sample = " << r << '\n';}
            float cd; // cummuative distribution from iterating over sched weights
            cd = 0;
            uint16_t n;
            n = 0;
            for(size_t ni=0;ni<sched.size();ni++) {cd += sched[ni]; if(passedD) cout << "testing = " << r << " against " << cd << " n = " << n << '\n'; if(r <= cd) break; n++;}
if(passedD) {cout << "sampled index = " << n << '\n';}
            cout << "table_add load_balance_calendar_table do_assign_member 0x" << std::hex << epoch << " 0x" << std::hex << t << " => 0x" << std::hex << n << '\n';
        }
#else
        for(size_t n=0;n<num_nds;n++) {
            float r = d(gen);
if(passedD) {cout << "sample = " << r << '\n';}
            float cd; // cummuative distribution from iterating over sched weights
            cd = 0;
            uint16_t n;
            n = 0;
            for(size_t ni=0;ni<sched.size();ni++) {cd += sched[ni]; if(passedD) cout << "testing = " << r << " against " << cd << " n = " << n << '\n'; if(r <= cd) break; n++;}
if(passedD) {cout << "sampled index = " << n << '\n';}
            cout << "table_add load_balance_calendar_table do_assign_member 0x" << std::hex << epoch << " 0x" << std::hex << t << " => 0x" << std::hex << n << '\n';
        }
#endif
        usleep(100000);
    }
    
    return 0;
}

