#!/usr/bin/env python3

from scapy.all import *

class UDPLB(Packet):
    name = "UDP LB Packet"
    fields_desc = [
        XShortField("magic", 0x4C42),
        XByteField("version", 1),
        XByteField("proto", 0),
        XLongField("tick", 0),
    ]

class EVIO6Seg(Packet):
    name = "EVIO6 Segment"
    fields_desc = [
        BitField("version", 0, 4),
        BitField("reserved", 0, 10),
        FlagsField("flags", 0, 2,
                   ["last",
                    "first",]),
        XShortField("rocid", 0),
        XIntField("offset", 0),]

bind_layers(UDPLB, EVIO6Seg, {'proto': 1})

EVIO6_BLOB_SIZE = 1050
evio6_blob = bytearray(EVIO6_BLOB_SIZE)
EVIO6_SEG_SIZE = 100

with scapy.utils.PcapWriter('packets_in.pcap', linktype=DLT_EN10MB) as w:
    w.write_header([])
    for offset in range(0, EVIO6_BLOB_SIZE, EVIO6_SEG_SIZE):
        flags = []
        if offset == 0:
            # first segment
            flags.append("first")
        if offset + EVIO6_SEG_SIZE > EVIO6_BLOB_SIZE:
            # last (possibly short) segment
            flags.append("last")

        p = Ether(dst="00:aa:bb:cc:dd:ee", src="00:11:22:33:44:55")/IP(dst="10.1.2.3", src="10.1.2.2")/UDP(sport=50000,dport=0x4c42)/UDPLB(tick=10)/EVIO6Seg(rocid=0xabc, flags=flags, offset=offset)/Raw(load=evio6_blob[offset:offset+EVIO6_SEG_SIZE])
        w.write_packet(p)
        #print(p.show())

        p = Ether(dst="00:aa:bb:cc:dd:ee", src="00:01:02:03:04:05")/IPv6(dst="fe80::2", src="fe80::1")/UDP(sport=12345,dport=0x4c42)/UDPLB(tick=20)/EVIO6Seg(rocid=0x123, flags=flags, offset=offset)/Raw(load=evio6_blob[offset:offset+EVIO6_SEG_SIZE])
        w.write_packet(p)
        #print(p.show())
