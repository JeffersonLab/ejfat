/************* Load Balancer Send  *******************/
#include <unistd.h>

#include <fstream>
#include <iostream>

using namespace std;

void   Usage(void)
{
    char usage_str[] =
        "\nUsage: \n\
        -e event #  \n\
        -h help \n\n";
        cout<<usage_str;
        cout<<"Required: -e\n";
}

int main (int argc, char *argv[])
{
    int optc;
    extern char *optarg;
    extern int   optind, optopt;

    uint32_t event_num;
    bool passedE = false;

    while ((optc = getopt(argc, argv, "e:")) != -1)
    {
        switch (optc)
        {
        case 'h':
            Usage();
            exit(1);
        case 'e':
            event_num = atoi((const char *) optarg) ;
            passedE = true;
            break;
        case '?':
            cerr<<"Unrecognised option: -"<<optopt<<'\n';
            Usage();
            exit(1);
        }
    }

    if(!passedE) { Usage(); exit(1); }

    ifstream f1("/dev/stdin", std::ios::binary | std::ios::in);
    ofstream f2("/dev/stdout",std::ios::binary | std::ios::out);

    // prepare LB meta-data
    // New LB meta-data header on front of payload
    union {
        struct __attribute__((packed)) lb_hdr {
            uint32_t l      : 8;
            uint32_t b      : 8;
            uint32_t vrsn   : 8;
            uint32_t ptcl   : 8;
            uint32_t rsrvd  : 16;
            uint32_t ent    : 16;
            uint64_t event  : 64;
        } lbmdbf;
        unsigned int lbmduia [4];
    } lbmd;

    lbmd.lbmdbf = {'L','B',2,1,0, 0, 1};
    lbmd.lbmdbf.event = event_num; //passed in

    // New RE meta-data header on front of payload
    union {
        struct __attribute__((packed))re_hdr {
            uint32_t vrsn    : 4;
            uint32_t rsrvd   : 12;
            uint32_t data_id : 16;
            uint32_t offset  : 32;
            uint32_t length  : 32;
            uint64_t event   : 64;
        } remdbf;
        uint32_t remduia[5];
    } remd;

    remd.remdbf = {2,0,1,0,1024,1};
    remd.remdbf.event = event_num; //passed in

    char x[1024]; //packet size limit

    f1.read((char*)(&x), sizeof(x));

    remd.remdbf.length = f1.gcount();
    f2.write((char*)lbmd.lbmduia, 4*sizeof(lbmd.lbmduia[0]));
    f2.write((char*)remd.remduia, 5*sizeof(remd.remduia[0]));
    f2.write((char*)x, f1.gcount());

    return 0;
}
