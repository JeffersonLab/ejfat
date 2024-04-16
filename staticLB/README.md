
#### packetBlastee.cc

The originial and most basic data receiver with one thread for stats and a settable number
of threads for the reading and reassembling of events.

Because it uses a routine from ejfat_assemble_ersap.hpp to reassemble,
it only accommodates out-of-order packets if they don't cross event boundaries.
Duplicate packets will mess things up. **Does NOT speak grpc and thus is useful
only with a statically configured CP.**

**Dependencies:** None


#### packetBlasterOld.cc

General program used to send data to the various receiving programs. Lots of cmd line options.

**Dependencies:** None


#### packetBlasterSmall.cc

This programs sends small packets to a packetBlastee in order to test the LB.
Check with tcpdump how many packets are sent to LB and how many to the blastee.
Used to find runt TCP frame bug in LB. Lots of cmd line options.
**Does NOT send sync msgs to CP and thus is useful
  only with a statically configured CP.**
  
**Dependencies:** None


#### packetAnalyzer.cc

This program accepts packets and prints out both the LB and RE headers.
Used only to debug header issues. Sender need to send directly to this
program and thus it is **not** for use with an LB.

**Dependencies:** None



#### udp_send_order.cc

Send a file, which is either read or piped-in, to udp_rcv_order.cc

**Dependencies:** None



#### udp_rcv_order.cc

Receive the file sent to it by udp_send_order.cc and reconstruct it.
Most of the work is done in ejfat_packetize.hpp and ejfat_assemble_ersap.hpp.
**Does NOT speak grpc and thus is useful
  only with a statically configured CP.** 

**Dependencies:** None
