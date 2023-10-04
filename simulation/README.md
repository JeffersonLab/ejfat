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


#### packetBlasteeFullNewMP.cc

This is the latest & greatest data receiver.
It reads data in by means of N thds for each source which also reassembles,
where N can be set on the command
line, but performs best when = 2 (possibly 3).
Events can be dumped after reassembly or passed on to process threads or thread.
Accounts for out-of-order and duplicate packets.
Internal variables can adjust how long and for how many buffers to wait for late packets in 2-tiered process.

This is by far the fastest receiver. On ejfat nodes, pinning 2 cores, from those closest to the NIC,
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



#### packetBlasteeFullNew.cc

Reads UDP packets in 2 thds for all sources, then passes packets to reassembly threads (3 per source).
Events can be dumped after reassembly or passed on to process threads (1 per source).
Accounts for out-of-order and duplicate packets.
Very limited adjustment on time to wait for late packets.

For a single sender:

 * 2.1 GB/s with ~0.002% loss
 * 2.2 GB/s with ~0.002% loss
 * 2.3 GB/s with ~0.003% loss
 * 2.4 GB/s with > 1.5%  loss, seems to be the end of the line


With 2 sources, the best total receiving rate (after a few minutes) is:

| Source | Data rate (GB/s) |rate |
---------|------------------|-----|
| src #1 |  1.1 | 1.1  |
| src #2 |  1.1 | 0.9  |
|  total |  2.2 | 2.0  |
| packet loss | 1.6%  |  0 |


Doesn't make a diff if buffers dumped or examined.



#### packetBlasteeEtFifoClient.cc  &   packtBlasteeEtFifoClientNew.cc


This data receiver reads UDP packets from clasBlaster.cc as an event source, reassembles it, then
places it into an ET system which is configured to be used as a FIFO.
It also connects and reports telemetry to the LB's control plane.
One thread monitors the ET system and reports the fifo level along with PID error signal.
The reading thread runs on a command-line-settable # of cores.
Because it uses a routine from ejfat_assemble_ersap.hpp to reassemble,
it only accommodates out-of-order packets if they don't cross event boundaries.
Duplicate packets will mess things up.

The "New" version of this receiver uses the latest interactions with the control plane,
developed when finishing work for the CHEP paper. It's yet untested.



#### packetBlastee.cc


The originial and most basic data receiver with one thread for stats and a settable number
of threads for the reading and reassembling of events.

Because it uses a routine from ejfat_assemble_ersap.hpp to reassemble,
it only accommodates out-of-order packets if they don't cross event boundaries.
Duplicate packets will mess things up.



#### packetBlaster.cc

General program used to send data to the various receiving programs. Lots of cmd line options.


#### udp_send_order.cc & udp_recv_order.cc

These 2 programs send and receive packets.
They can communicate directly with eachother if, in ejfat_packetize.hpp, you comment out the

    #define ADD_LB_HEADER 1

line near the top of the file. If, however, you want to use this with the ejfat emulater,
it must not be commented out.

Most of the hard work is done in ejfat_packetize.hpp and ejfat_assemble_ersap.hpp.
These 2 files are copied over to ersap-ejfat for implementing the packetizing
and reassembly services and engines.







## THINGS TO DO:

1) change the mixture of C and C++ into a nice, uniform C++.
