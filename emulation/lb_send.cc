/************* Loab Balancer Send  *******************/
#include <unistd.h>

#include <fstream>
#include <iostream>

using namespace std;

void   Usage(void)
{
    char usage_str[] =
        "\nUsage: \n\
        -s seq #  \n\
        -h help \n\n";
        cout<<usage_str;
        cout<<"Required: -s\n";
}

int main (int argc, char *argv[])
{
    int optc;
    extern char *optarg;
    extern int   optind, optopt;

    unsigned int seq;
    bool passedS = false;

    while ((optc = getopt(argc, argv, "s:")) != -1)
    {
        switch (optc)
        {
        case 'h':
            Usage();
            exit(1);
        case 's':
            seq = atoi((const char *) optarg) ;
            passedS = true;
            break;
        case '?':
            cerr<<"Unrecognised option: -"<<optopt<<'\n';
            Usage();
            exit(1);
        }
    }

    if(!passedS) { Usage(); exit(1); }

    ifstream f1("/dev/stdin", std::ios::binary | std::ios::in);
    ofstream f2("/dev/stdout",std::ios::binary | std::ios::out);

    // prepare LB meta-data
    // LB meta-data header on front of payload
    union {
        struct __attribute__((packed)) lb_hdr {
            unsigned int l    : 8;
            unsigned int b    : 8;
            unsigned int vrsn : 8;
            unsigned int ptcl : 8;
            unsigned long int tick : 64;
        } lbmdbf;
        unsigned int lbmduia [3];
    } lbmd;

    lbmd.lbmdbf = {'L','B',1,1,1};

    // prepare RE meta-data
    // RE meta-data header on front of payload
    union {
        struct __attribute__((packed))re_hdr {
            unsigned int vrsn    : 4;
            unsigned int rsrvd   : 10;
            unsigned int frst    : 1;
            unsigned int lst     : 1;
            unsigned int data_id : 16;
            unsigned int seq     : 32;
        } remdbf;
        unsigned int remduia[2];
    } remd;

    remd.remdbf = {1,0,1,0,1,1};
    remd.remdbf.seq = seq; //passed in

    f2.write((char*)lbmd.lbmduia, 3*sizeof(lbmd.lbmduia[0]));
    f2.write((char*)remd.remduia, 2*sizeof(remd.remduia[0]));
    char x[1024]; //packet size limit
    f1.read((char*)(&x), sizeof(x));
    f2.write((char*)x, f1.gcount());

    return 0;
}
