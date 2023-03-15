//       Control Plane Schedule Adjuster - Volkswagon  Quality
//
// reads feedback from compute farm and adjust scheduling density
// 

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <string.h>
#include <string.h>
#include <fstream>
#include <iostream>
#include <inttypes.h>
#include <netdb.h>
#include <time.h>
#include <chrono>
#include <ctime>
#include <random>
#include <functional>
#include <array>
#include <algorithm>    // std::transform
#include <vector>       // std::vector
#include <math.h>       /* fabs */

using namespace std;

const size_t foo  = 0;
unsigned seed = std::chrono::system_clock::now().time_since_epoch().count();

void   Usage(void)
{
    char usage_str[] =
        "\nUsage: \n\
        -f feedback file   \n\
        -n num hosts  \n\
        -d debug mode \n\
        -h help \n\n";
        cout<<usage_str;
        cout<<"Required: -n -f\n";
}

template<class X>
void print(const X& x) {
    size_t N = x.size();
    for(uint16_t i=0;i<N;i++) cout << x[i] << ' '; cout << '\n';
}

template<class X>
void softmax(const X& x, X& y) {
    const uint64_t N = x.size();
    double a = 0;
    for(uint16_t i=0;i<N;i++) a   += exp(x[i]);
    for(uint16_t i=0;i<N;i++) y[i] = exp(x[i])/a;
}

static volatile int cpu=-1;

template<class X>
size_t smpl_wghtd(X w)
{
    //use inversion method to sample from tabulated distribution (weights)
    const size_t N = 1e3; //resolution
    size_t s = w.size();
    vector<double> t;
    t.push_back(N*w[0]);
    for(size_t i=1;i<s;i++) t.push_back(N*w[i]+t[i-1]);
cout << "smpl_wghtd t = "; print(t);            
    //now get random uniform in [1,N]:
    unsigned seed = std::chrono::system_clock::now().time_since_epoch().count();
    std::default_random_engine g (seed);
    std::uniform_int_distribution<int> u(1,N);
    size_t r = u(g);
cout << "smpl_wghtd r = " << r << '\n';            
    //now index into weight array
    for(size_t i=0;i<s;i++) if(r<=t[i]) return i; //fix this hack
    return s;
}

int main (int argc, char *argv[])
{
    int optc;
    extern char *optarg;
    extern int   optind, optopt;

    bool passedN=false, passedF=false, passedD=false;

    uint32_t num_hsts;                 // number of hosts to schedule
    char     hst_fdbk_t[256];           // host feedback file

    while ((optc = getopt(argc, argv, "dhn:f:")) != -1)
    {
        switch (optc)
        {
        case 'h':
            Usage();
            exit(1);
        case 'd':
            passedD = true;
            fprintf(stdout, "-d ");
            break;
        case 'n':
            num_hsts = (uint32_t) atoi((const char *) optarg) ;
            passedN = true;
            fprintf(stdout, "-n %d ", num_hsts);
            break;
        case 'f':
            strcpy(hst_fdbk_t, (const char *) optarg) ;
            passedF = true;
            fprintf(stdout, "-f %s ", hst_fdbk_t);
            break;
        case '?':
            fprintf (stdout, "Unrecognised option: %d\n", optopt);
            Usage();
            exit(1);
        }
    }
    fprintf(stdout, "\n");
    if(!(passedN && passedF)) { Usage(); exit(1); }

    // set up
    double alpha = 0.5; // learning rate
    //double beta_0 = 0.5; // bias to for hosts (?)

    // R vector with the first value set at 0.5
    vector<double> R; R.resize(num_hsts); //for(uint16_t h=0;h<num_hsts;h++) R[h] = 0.5+h*0.2;
    // Q vector with the first value set at 0.5
    vector<double> Q; Q.resize(num_hsts); for(uint16_t h=0;h<num_hsts;h++) Q[h] = 0;
    ifstream hst_fdbk;
 //   std::discrete_distribution<uint16_t> d;
     // random device class instance, source of 'true' randomness for initializing random seed
    std::random_device rd; 

    // Mersenne twister PRNG, initialized with seed from previous random device instance
    std::mt19937 gen(rd()); 
    std::normal_distribution<float> d(0, 0.1); 
    std::gamma_distribution<float> dg(10.0,0.01);

    //Betting Odds
    vector<double> BO;   BO.resize(num_hsts); for(uint16_t h=0;h<num_hsts;h++) BO[h] = 0;
    //Num  Successes
    vector<uint32_t> NS; NS.resize(num_hsts); for(uint16_t h=0;h<num_hsts;h++) NS[h] = 0;

    uint32_t trl=1;   // loop counter

    // set initial schedule Density (SD) ////////////
    vector<double> SD;   SD.resize(num_hsts);
    softmax(Q, SD);
    ///////// set initial Reward
    {
        size_t s = R.size();
        for(size_t i=0;i<s;i++) 
        {
            R[i] += 3*d(gen);
            R[i] = R[i] > 0.5 ? 0.5 : R[i];
            R[i] = R[i] < -0.5 ? -0.5 : R[i];
            //Q[i] = R[i]; //somethng other than zero
        }
    }      

    do {
        // Monitor feedback from nodes and adjust scheduling density

/*****************************************
        //sleep(1);
        //read host feedback: an array of health metrics
        hst_fdbk.open(hst_fdbk_t,std::ios::in);
        //load the feedback from each host
        while(true)
        {
            double x = 0;
            uint16_t i = 0;
            hst_fdbk >> x;
            if(!hst_fdbk.good()) break;
if(passedD) cout << "read " << x << " from feedback file\n";            
            R.push_back(R[i++]+x);
        }
        hst_fdbk.close();
*****************************************/
///////////////////////////////////////////////
        //theta <- rep(NA, times = N_trials)
//        d.param(Q.begin(),Q.end()); //update action selection probability weightings from Q

//if(passedD) cout << "old_SD = "; print(SD);            
if(passedD) cout << "old_Q = "; print(Q);            
if(passedD) cout << "R = "; print(R);            
        //uint16_t action = smpl_wghtd(SD);  //d();  //which.max(R[t,]) #
        vector<double> q0; q0.resize(Q.size()); 
        //std::transform (Q.begin(), Q.end(), q.begin(), fabs);
        for(size_t i=0;i<num_hsts;i++) q0[i] = (Q[i]<0?-1:1)*Q[i]; //fabs()
        vector<double> q; q.resize(q0.size()); 
        softmax(q0, q);
if(passedD) cout << "q = "; print(q); 
        uint16_t action = smpl_wghtd(q); //select for action based on distance from setpoint
if(passedD) cout << "action = " << action << "\n";            
if(passedD) cout << "Q_dlt = " << alpha * (R[action] - Q[action]) << "\n";            
        Q[action] += alpha * (R[action] - Q[action]);
if(passedD) cout << "new_Q = "; print(Q);            
        vector<double> old_SD; old_SD.resize(SD.size()); 
        for(size_t i=0;i<num_hsts;i++) old_SD[i] = SD[i];
if(passedD) cout << "old_SD = "; print(old_SD);
        softmax(Q, SD);
if(passedD) cout << "new_SD = "; print(SD);
        //what are the betting odds after each trial? (LaPlace's Rule of Succession)
//        BO <- matrix(nrow = N_trials, ncol = num_hsts)
//        NS <- vector(length = num_hsts)
        NS[action]++;
if(passedD) cout << "NS = "; print(NS);            
        for(size_t i=0;i<num_hsts;i++)  BO[i] = double(NS[i]+1)/double(trl+2);
        double BOsum = accumulate(BO.begin(),BO.end(),0.0);
        for(size_t i=0;i<num_hsts;i++)  BO[i] /= BOsum;   //normalize
if(passedD) cout << "BO = "; print(BO); 
        {                 
            float x = dg(gen); 
            float x1 = dg(gen);         
            //R[action] *= SD[action]+(R[action]>0?1:-1)*x/(x+x1)/2.0; //retard response with Beta noise
            //R[action] *= SD[action]+(R[action]>0?1:-1)*x; 	         //retard response with Gamma noise
//if(passedD) cout << "old_SD/SD[action] = " << old_SD/SD[action] << "\n"; 
            //R[action] -= x; 	         //retard response with Gamma noise
            //clip Reward to proper bounds
        vector<double> SDrto; SDrto.resize(SD.size()); 
        for(size_t i=0;i<num_hsts;i++) SDrto[i] = SD[i]/old_SD[i];
if(passedD) cout << "SDrto = "; print(SDrto); 
        for(size_t i=0;i<num_hsts;i++)//effect of schedule change
            {
            R[i] *= 0.875*(R[i]<=0 ? SDrto[i] : 1/SDrto[i]); 
            float x = d(gen)/2; 
if(passedD) cout << "jitter = " << x << "\n"; 
            R[i] += x; //add some response jitter	         
            R[i] = R[i] > 0.5 ? 0.5 : R[i];
            R[i] = R[i] < -0.5 ? -0.5 : R[i];
            }
        }
        trl++;
    } while(1);

    return 0;
}
