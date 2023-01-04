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
        cout<<"Required: -i -p\n";
}

template<class X>
void print(const X& x) {
    size_t N = x.size();
    for(uint16_t i=0;i<N;i++) cout << x[i] << ' '; cout << '\n';
}

template<class X>
void softmax(const X& x, X& y, const uint16_t N, double t) {
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
cout << "softmax t = "; print(t);            
    //now get random uniform in [1,N]:
    unsigned seed = std::chrono::system_clock::now().time_since_epoch().count();
    std::default_random_engine g (seed);
    std::uniform_int_distribution<int> u(1,N);
    size_t r = u(g);
cout << "softmax r = " << r << '\n';            
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
    double alpha = 0.3; // learning rate
    //double beta_0 = 0.5; // bias to for hosts (?)
    double beta_1 = 1; //  temperature

    // R vector with the first value set at 0.5
    vector<double> R; R.resize(num_hsts); for(uint16_t h=0;h<num_hsts;h++) R[h] = 0.5+h*0.2;
    // Q vector with the first value set at 0.5
    vector<double> Q; Q.resize(num_hsts); for(uint16_t h=0;h<num_hsts;h++) Q[h] = 0.5+h*0.1;
    ifstream hst_fdbk;
 //   std::discrete_distribution<uint16_t> d;
     // random device class instance, source of 'true' randomness for initializing random seed
    std::random_device rd; 

    // Mersenne twister PRNG, initialized with seed from previous random device instance
    std::mt19937 gen(rd()); 
    std::normal_distribution<float> d(0, 0.1); 

    //Betting Odds
    vector<double> BO;   BO.resize(num_hsts); for(uint16_t h=0;h<num_hsts;h++) BO[h] = 0;
    //Num  Successes
    vector<uint32_t> NS; NS.resize(num_hsts); for(uint16_t h=0;h<num_hsts;h++) NS[h] = 0;

    uint32_t trl=0;   // loop counter

    do {
        // Monitor feedback from nodes and adjust scheduling density

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
///////////// RW test /////////////////////////
        {
            size_t s = R.size();
            for(size_t i=0;i<s;i++) R[i] += d(gen);
        }      
///////////////////////////////////////////////
        //theta <- rep(NA, times = N_trials)
//        d.param(Q.begin(),Q.end()); //update action selection probability weightings from Q

        vector<double> t; t.resize(num_hsts);
if(passedD) cout << "Q = "; print(Q);            
        softmax(Q, t, num_hsts, beta_1);
if(passedD) cout << "t = "; print(t);            
        uint16_t action = smpl_wghtd(t);  //d();  //which.max(R[t,]) #
if(passedD) cout << "action = " << action << "\n";            
        Q[action] += alpha * (R[action] - Q[action]);
if(passedD) cout << "new Q = "; print(Q);            
/*
        nonactions_t <- (1:num_hsts)[-action[t]]
        Q[t + 1, nonactions_t] <- Q[t, nonactions_t]
        SM <- apply(Q, 1, softmax, temp=1)
        plot(SM[1,],type='l',col=clrs[1],ylim = c(min(SM),max(SM)))
        for (a in 2:num_hsts) {
          lines(SM[a,],col=clrs[a])
        }
*/
        //what are the betting odds after each trial? (LaPlace's Rule of Succession)
//        BO <- matrix(nrow = N_trials, ncol = num_hsts)
//        NS <- vector(length = num_hsts)
          NS[action]++;
if(passedD) cout << "NS = "; print(NS);            
          for(size_t i=0;i<num_hsts;i++)  BO[i] = double(NS[i]+1)/double(trl+2);
            //cout << "BO["<<i<<"] = "<<BO[i]<<" for NS["<<i<<"] = "<<NS[i]<<" and trl = "<<trl<<'\n';
          //}
if(passedD) cout << "BO = "; print(BO);            
          trl++;
    } while(1);

    return 0;
}
/*
// discrete_distribution constructor
#include <iostream>
#include <chrono>
#include <random>
#include <functional>
#include <array>

int main()
{
  // construct a trivial random generator engine from a time-based seed:
  unsigned seed = std::chrono::system_clock::now().time_since_epoch().count();
  std::default_random_engine generator (seed);

  std::array<double,4> init = {1.0,2.0,3.0,4.0};

  std::discrete_distribution<int> first;
  std::discrete_distribution<int> second (init.begin(),init.end());
  std::discrete_distribution<int> third {0.1,0.2,0.3,0.4};
  std::discrete_distribution<int> fourth (4,0.0,40.0,std::bind2nd(std::plus<double>(),5.0));
  std::discrete_distribution<int> fifth (fourth.param());

  // display probabilities:
  std::cout << "displaying probabilities:";
  std::cout << std::endl << "first : ";
  for (double x:first.probabilities()) std::cout << x << " ";
  std::cout << std::endl << "second: ";
  for (double x:second.probabilities()) std::cout << x << " ";
  std::cout << std::endl << "third : ";
  for (double x:third.probabilities()) std::cout << x << " ";
  std::cout << std::endl << "fourth: ";
  for (double x:fourth.probabilities()) std::cout << x << " ";
  std::cout << std::endl << "fifth : ";
  for (double x:fifth.probabilities()) std::cout << x << " ";
  std::cout << std::endl;

  return 0;
}

#include <random>

int main()
{
  std::default_random_engine generator;
  std::discrete_distribution<int> d1 {10.0,20.0,20.0,25.0,25.0};
  std::discrete_distribution<int> d2 (d1.param());

  // print two independent values:
  std::cout << d1(generator) << std::endl;
  std::cout << d2(generator) << std::endl;

  return 0;
}
*/
