//      PID Controller for Control Plane - Volkswagon  Quality
//
// reads feedback from compute farm and adjusts scheduling density
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

using namespace std;

unsigned seed = std::chrono::system_clock::now().time_since_epoch().count();

void   Usage(void)
{
    char usage_str[] =
        "\nUsage: \n\
        -n num events  \n\
        -d debug mode \n\
        -h help \n\n";
        cout<<usage_str;
        cout<<"Required: -i -p\n";
}

template<class X>
void print(const X& x) {
    size_t N = x.size();
    for(uint16_t i=0;i<N;i++) cout << x[i] << ' '; cout << '\n';
}

static volatile int cpu=-1;

template<class X>
X pid(          // Proportional, Integrative, Derivative Controller
    const X& setPoint, // Desired Operational Set Point
    const X& prcsVal,  // Measure Process Value
    const X& delta_t,  // Time delta between determination of last control value
    const X& Kp,       // Konstant for Proprtional Control
    const X& Ki,       // Konstant for Integrative Control 
    const X& Kd        // Konstant for Derivative Control 
    )
{
    static X previous_error = 0; // for Derivative
    static X integral_acc = 0;   // For Integral (Accumulated Error)
           X error = setPoint - prcsVal;
           integral_acc += error * delta_t;   
           X derivative = (error - previous_error) / delta_t;    
    previous_error = error;
    return Kp * error + Ki * integral_acc + Kd * derivative;  // control output
}

int main (int argc, char *argv[])
{
    int optc;
    extern char *optarg;
    extern int   optind, optopt;

    bool passedN=false, passedD=false;

    uint32_t num_events = 100;                 // number of events to process
    
    while ((optc = getopt(argc, argv, "dhn:")) != -1)
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
        case 'n':
            num_events = (uint32_t) atoi((const char *) optarg) ;
            passedN = true;
            fprintf(stderr, "-n %d ", num_events);
            break;
        case '?':
            fprintf (stderr, "Unrecognised option: %d\n", optopt);
            Usage();
            exit(1);
        }
    }
    fprintf(stderr, "\n");

    //controller parms
    const float setPoint = 0.5;
    const float Kp = 0.5;
    const float Ki = 0.0;
    const float Kd = 0.00;

    // random device class instance, source of 'true' randomness for initializing random seed
    std::random_device rd; 
    // Mersenne twister PRNG, initialized with seed from previous random device instance
    std::mt19937 gen(rd()); 
    std::gamma_distribution<double> d(2.0,1.0);

    float ns_gain = 0.5; //noise gain
    //process and control values for host
    vector<float> pv(num_events);
    vector<float> cv(num_events);
    pv[0] = 0.0;    
    cv[0] = pid(setPoint,pv[0],float(1),Kp,Ki,Kd);
    {
        float x = d(gen); 
        float x1 = d(gen);         
        pv[1] = pv[0] + cv[0] + ns_gain*(x/(x+x1)-0.5f); //add beta noise;
    }
    cout << pv[0] << ',' << cv[0] << '\n'; 
    for(size_t i=1;i<num_events;i++) {
        float x = d(gen); 
        float x1 = d(gen);         
        cv[i] = pid(setPoint,pv[i],float(1),Kp,Ki,Kd);
        pv[i+1] = std::min(float(1),std::max(float(0),pv[i] + cv[i] + ns_gain*(x/(x+x1)-0.5f))); //add beta noise
//        pv[i+1] = std::clamp(pv[i+1], float(0), float(1) );
        cout << pv[i] << '\t' << cv[i] << '\n'; 
    } 


    return 0;
}

