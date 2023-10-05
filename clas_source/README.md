#### clasBlaster.cc

This is a general data-sending program which reads events from a CLAS data file and sends each event
to the given destination. Many command line options.


#### clasBlasterEventFiles.cc

This is hacked up version of a slightly older clasBlaster.c.
It's designed so that the events in the HIPO file that it reads can be written
out into 1 file per event. Each file has just the event data and no record attached.
This was to provide ESNET with individual events to examine their compressibility.

#### clasBlasterIds.cc

This program acts as a data sender in testing the use of the ET system as a fifo.

This differs from clasBlaster.cc in that it will send exactly the same
CLAS event to the same destination but with different data source ids for each.
This simulates multiple sources that are in sync as they all produces the same
tick in succession. This allows testing of udp_rcv_et.c, which handles receiving from
multiple sources - but they must be in sync.


You'll have to coordinate the number of data sources, with the setting up of the ET system.
For example, for 3 sources, run the ET with something like:

    et_start_fifo -f /tmp/fifoEt -d -s 150000 -n 3 -e 1000

You can then run the receiving program like:

    simulation/udp_rcv_et -et /tmp/fifoEt -ids 1,3,76 -p 17750 -core 80 -pinCnt 4

This expects data sources 1,3, and 76. There will be room in each ET fifo entry to have
3 buffers (ET events), one for each source, all the same event number. There will be 1000 entries.
Each buffer will be 150kB. Max # of sources is 16 (can change that).

You can run this program like:

    clasBlasterIds -f /daqfs/java/clas_005038.1231.hipo -host 172.19.22.244 -p 19522 -mtu 9000 -s 25000000 -cores 60 -ids 1,3,76  -bufdelay -d 50000



