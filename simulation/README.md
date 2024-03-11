## NOTE on MTU


It turns out that the ethernet MTU contains both IP and UDP/TCP headers.

"It is true that a typical IPv4 header is 20 bytes, and the UDP header is 8 bytes.
However it is possible to include IP options which can increase the size of the
IP header to as much as 60 bytes. In addition, sometimes it is necessary for
intermediate nodes to encapsulate datagrams inside of another protocol such as
IPsec (used for VPNs and the like) in order to route the packet to its destination.
So if you do not know the MTU on your particular network path, it is best to leave
a reasonable margin for other header information that you may not have anticipated."

IP options are used when, for example, a source route is specified,
where the sender requests a certain routing path. I don't think we'll deal with this at the lab.

The IPsec headers can be really big. They're used to provide security.
I doubt if we'll ever deal with this so UDP header = 8 bytes on our networks.

IPv6 fixes the IP header size at 40 bytes.

So I think we can set payload size = MTU(1024) - 20 - 8 -LB -RE = 1024 - 28 - 34 = 962.


Michael's code does the following:

My components current recv 1024 bytes off the wire (not including udp/ip headers);
so udp payload = (sender payload + 12 + 16).
Thus max(sender payload) = udp payload - 34 = 1024 - 34 = 990


That means the programmatically in Carl's code, the MTU can be set by hand to 1024 + (990 - 962) = 1052.

Switching to IPv6 means decreasing payload by 20 additional bytes.


## PROGRAMS in "simulation" directory


#### packetBlasteeFastMP.cc

This is the latest & greatest data receiver.
It is packetBlasteeFullNewMP.cc but with 2 improvements.
1) Like packetBlasterFullNewMP, it reads data in by means of N thds for each source
which also reassembles.
However, unlike that program, it doesn't use the complicated algorithm for
dealing with every out-of-order and duplicate packet. It turns out that doing
so uses so much compute time that at high input rates, it drops many more
packets than the simple approach. That simple approach is just to call a function
which returns the next buildable buffer. It only accounts for out-of-order
within the boundaries of a single event.
2) As part of keeping stats, it measures the latency of an event - that is the average time to reassemble in
nanoseconds. Clock starts when the first packet of an event arrives and ends when
the calling thread gets the full event. This number will only make sense if all
incoming events are the same size.


#### packetBlasteeFullNewMP.cc

This is the 2nd best data receiver.
It reads data in by means of N thds for each source which also reassembles,
where N can be set on the command
line, but performs best when = 2 (possibly 3).
Events can be dumped after reassembly or passed on to process threads or thread.
Accounts for out-of-order and duplicate packets.
Internal variables can adjust how long and for how many buffers to wait for late packets in 2-tiered process.

On ejfat nodes, pinning 2 cores, from those closest to the NIC,
to each reassembly thread, for a single sender it reads:

 * 3.05 GB/s with ~0.002% loss
 * 3.26 GB/s with ~0.06%  loss
 * 3.40 GB/s with ~0.4%   loss, but can't seem to be pushed beyond that.

With 2 sources (each with 2 cores), the best total receiving rate (after a few minutes) is:

| Source | Data rate (GB/s) | rate | rate | rate |
---------|------------------|------|------|------|
| src #1 |  2.1 | 1.9  |  2.07  |   2.2  |
| src #2 |  1.6 | 1.9  |  2.17  |   2.2  |
|  total |  3.7 | 3.8  |  4.24  |   4.4  |
| packet loss | 0%  |  0.002 | 0.06 |  > 0.5  |



#### packetBlasteeEtMT.cc

This data receiver is similar to packetBlasteeEtFifoClient (see below),
differs by adding 2 threads and buffering. It's faster and makes
packetBlasteeEtFifoClient obsolete, but has the disadvantage of needing
the disruptor (fast ring buffer) library to link against. It also needs
the EtFifoEntryItem.cpp/.h files.

Measurements show that writing into the ET system has a delay every so
often when ET's memory-mapped file is updated.
This program attempts to using buffering to solve that problem. So instead of
writing into 1 buffer, there is a supply of 8192 buffers and a separate thread to
take filled buffers and write them into ET. Note: currently to change the #
of buffers, one must edit the code and recompile.

It also adds another thread to simultaneously get (up to 1024) 
empty ET fifo entries in a ring buffer, so there is always somewhere to write into.
These improve performance so that it handles 3X the input rate before one
sees dropped packets.



#### packetBlasteeEtFifoClient.cc

This data receiver reads UDP packets from clasBlaster.cc (a single event source),
reassembles it, then
places it into an ET system which is configured to be used as a FIFO.
It also connects and reports telemetry to the LB's control plane.
One thread monitors the ET system and reports the fifo level along with PID error signal.
The reading thread runs on a command-line-settable # of cores.
Because it uses a routine from ejfat_assemble_ersap.hpp to reassemble,
it only accommodates out-of-order packets if they don't cross event boundaries.
Duplicate packets will mess things up.

This program does <b>not</b> use the sophisticated handling of out-of-order and
duplicate packets that packetBlasteeFullNewMP uses. Like packetBlasteeFastMP,
it only calls getCompletePacketizedBuffer(), but I believe that's an advantage
since it's much faster reassembly code.



#### packetBlastee.cc

The originial and most basic data receiver with one thread for stats and a settable number
of threads for the reading and reassembling of events.

Because it uses a routine from ejfat_assemble_ersap.hpp to reassemble,
it only accommodates out-of-order packets if they don't cross event boundaries.
Duplicate packets will mess things up.



#### packetAnalyzer.cc

This program accepts packets and prints out both the LB and RE headers.
Used only to debug header issues.



#### packetBlaster.cc

General program used to send data to the various receiving programs. Lots of cmd line options.



#### udp_send_order.cc

Send a file, which is either read or piped-in, to udp_rcv_order.cc



#### udp_rcv_order.cc

Receive the file sent to it by udp_send_order.cc and reconstruct it.

These 2 programs can communicate directly with eachother and bypass the LB if,
in ejfat_packetize.hpp, you comment out the

    #define ADD_LB_HEADER 1

line near the top of the file.
Most of the work is done in ejfat_packetize.hpp and ejfat_assemble_ersap.hpp header files.



#### ersap_et_consumer.cc

This file is just an example for Vardan or any user of the ET-system-as-a-fifo.
It shows how to read the ET system when it's configured as a fifo.
It was used to get stats on the effect of ET-consumer delay on an EJFAT system,
but other than that, this program is never used or run.


## THINGS TO DO:

1) change the mixture of C and C++ into a nice, uniform C++.

## THINGS NOT TO DO:

1) Replacing recv with recvmmsg (on linux) only slows things down.
2) Writing a packetBlastee in which 1 thread reads packets, and places
each packet onto a ring associated with a single source. Then 1 or more
build threads grab an empty buffer from a supply and build an event into
it by reading from a ring with a source's packets in it. The single
read thread is a real bottleneck.

