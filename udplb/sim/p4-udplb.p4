// -*- Mode: c -*-
#include <core.p4>
#include <xsa.p4>

struct intrinsic_metadata_t {
    bit<64> ingress_global_timestamp;
    bit<64> egress_global_timestamp;
    bit<16> mcast_grp;
    bit<16> egress_rid;
}

header ethernet_t {
    bit<48> dstAddr;
    bit<48> srcAddr;
    bit<16> etherType;
}

header ipv6_t {
    bit<4>   version;
    bit<8>   trafficClass;
    bit<20>  flowLabel;
    bit<16>  payloadLen;
    bit<8>   nextHdr;
    bit<8>   hopLimit;
    bit<128> srcAddr;
    bit<128> dstAddr;
}

header ipv4_t {
    bit<4>  version;
    bit<4>  ihl;
    bit<8>  diffserv;
    bit<16> totalLen;
    bit<16> identification;
    bit<3>  flags;
    bit<13> fragOffset;
    bit<8>  ttl;
    bit<8>  protocol;
    bit<16> hdrChecksum;
    bit<32> srcAddr;
    bit<32> dstAddr;
}

header ipv4_opt_t {
    varbit<320> options; // IPv4 options - length = (ipv4.hdr_len - 5) * 32
}

header udp_t {
    bit<16> srcPort;
    bit<16> dstPort;
    bit<16> totalLen;
    bit<16> checksum;
}

header udplb_t {
    bit<16> magic; 		/* LB */
    bit<8> version;		/* version 0 */
    bit<8> proto;
    bit<64> tick;
}
#define SIZEOF_UDPLB_HDR 12

struct short_metadata {
    bit<64> ingress_global_timestamp;
    bit<9>  egress_spec;
    bit<1>  processed;
    bit<16> packet_length;
}
  
struct headers {
    ethernet_t       ethernet;
    ipv4_t           ipv4;
    ipv4_opt_t       ipv4_opt;
    ipv6_t           ipv6;
    udp_t            udp;
    udplb_t          udplb;
}

// User-defined errors 
error {
    InvalidIPpacket,
    InvalidUDPLBmagic,
    InvalidUDPLBversion
}

#if 0
#if 0
// Fix up IPv4 header checksum using rfc1624 method
//
//  HC  - old checksum in header
//  C   - one's complement sum of old header
//  HC' - new checksum in header
//  C'  - one's complement sum of new header
//  m   - old value of a 16-bit field
//  m'  - new value of a 16-bit field
//
// HC' = ~(C + (-m) + m')    --    [Eqn. 3]
//     = ~(~HC + ~m + m')
    
bit<16> cksum_swap(in bit<16> hc, in bit<16> old, in bit<16> new) {
    bit<18> sum;

    sum = (bit<18>)(hc ^ (bit<16>)0xFFFF);
    sum = sum + (bit<18>)(old ^ (bit<16>)0xFFFF);
    sum = sum + (bit<18>)(new);

    // Wrap any carry bits back around into the lsbs
    sum = (sum & 0xFFFF) + (sum >> 16);
    sum = (sum & 0xFFFF) + (sum >> 16);

    return sum[15:0];
}

#else

bit<16> cksum_swap(in bit<16> hc, in bit<16> old, in bit<16> new) {
    return hc ^ 0xFFFF;
}
#endif
#endif

parser ParserImpl(packet_in packet, out headers hdr, inout short_metadata short_meta, inout standard_metadata_t smeta) {
    state start {
        transition parse_ethernet;
    }

    state parse_ethernet {
        packet.extract(hdr.ethernet);
        transition select(hdr.ethernet.etherType) {
            16w0x0800: parse_ipv4;
            16w0x86dd: parse_ipv6;
            default: accept;
        }
    }
    state parse_ipv4 {
        packet.extract(hdr.ipv4);
	verify(hdr.ipv4.version == 4 && hdr.ipv4.ihl >= 5, error.InvalidIPpacket);
        packet.extract(hdr.ipv4_opt, (((bit<32>)hdr.ipv4.ihl - 5) * 32));
        transition select(hdr.ipv4.protocol) {
            8w17: parse_udp;
            default: accept;
        }
    }
    state parse_ipv6 {
        packet.extract(hdr.ipv6);
        verify(hdr.ipv6.version == 6, error.InvalidIPpacket);
        transition select(hdr.ipv6.nextHdr) {
            8w17: parse_udp;
            default: accept;
        }
    }
    state parse_udp {
        packet.extract(hdr.udp);
	transition select(hdr.udp.dstPort) {
	  16w0x4c42: parse_udplb;
	  default: accept;
	}
    }

    state parse_udplb {
      packet.extract(hdr.udplb);
      verify(hdr.udplb.magic == 0x4c42, error.InvalidUDPLBmagic);
      verify(hdr.udplb.version == 1, error.InvalidUDPLBversion);
      transition accept;
    }
}

control MatchActionImpl(inout headers hdr, inout short_metadata short_meta, inout standard_metadata_t smeta) {

    //
    // DstFilter
    //

    bit<128> meta_ipdst = 128w0;
	
    action drop() {
	smeta.drop = 1;
    }
    
    table dst_filter_table {
	actions = {
	    drop;
	    NoAction;
	}
	key = {
	    hdr.ethernet.dstAddr : exact;
	    hdr.ethernet.etherType : exact;
	    meta_ipdst : exact;
	}
	size = 32;
	default_action = drop;
    }

    //
    // EpochAssign
    //

    bit<32> meta_epoch = 0;
    
    action do_assign_epoch(bit<32> epoch) {
	meta_epoch = epoch;
    }

    table epoch_assign_table {
	actions = {
	    do_assign_epoch;
	    drop;
	}
	key = {
	    hdr.udplb.tick : lpm;
	}
	size = 128;
	default_action = drop;
    }

    //
    // LoadBalanceCalendar
    //

    // Use lsbs of tick to select a calendar slot
    bit<9> calendar_slot = (bit<9>) hdr.udplb.tick & 0x1FF;
    bit<16> meta_member_id = 0;

    action do_assign_member(bit<16> member_id) {
	meta_member_id = member_id;
    }

    table load_balance_calendar_table {
	actions = {
	    do_assign_member;
	    drop;
	}
	key = {
	    meta_epoch : exact;
	    calendar_slot : exact;
	}
	size = 2048;
	default_action = drop;
    }
    
    //
    // MemberInfoLookup
    //

    // Cumulative checksum delta due to field rewrites
    bit<16> ckd = 0;
    bit<16> new_udp_dst = 0;

    action cksum_sub(inout bit<16> cksum, in bit<16> a) {
	bit<17> sum = (bit<17>) cksum;

	sum = sum + (bit<17>)(a ^ 0xFFFF);
	sum = (sum & 0xFFFF) + (sum >> 16);

	cksum = sum[15:0];
    }

    action cksum_add(inout bit<16> cksum, in bit<16> a) {
	bit<17> sum = (bit<17>) cksum;

	sum = sum + (bit<17>)a;
	sum = (sum & 0xFFFF) + (sum >> 16);

	cksum = sum[15:0];
    }

    action cksum_swap(inout bit<16> cksum, in bit<16> old, in bit<16> new) {
	cksum_sub(cksum, old);
	cksum_add(cksum, new);
    }

 #if 0
    action cksum_swap(inout bit<16> cksum, in bit<16> old, in bit<16> new) {
	bit<17> sum = (bit<17>) cksum;

	// Wrap any carry bits back around into the lsbs
	//sum = (bit<17>)(sum ^ 0xFFFF);
	sum = sum + (bit<17>)(old ^ 0xFFFF);
	sum = (sum & 0xFFFF) + (sum >> 16);
	sum = sum + (bit<17>)(new);
	sum = (sum & 0xFFFF) + (sum >> 16);

	cksum = sum[15:0];
    }
#endif
    
    action do_ipv4_member_rewrite(bit<48> mac_dst, bit<32> ip_dst, bit<16> udp_dst) {
	// Calculate IPv4 and UDP pseudo header checksum delta using rfc1624 method

	cksum_swap(ckd, hdr.ipv4.dstAddr[31:16], ip_dst[31:16]);
	cksum_swap(ckd, hdr.ipv4.dstAddr[15:00], ip_dst[15:00]);
	cksum_swap(ckd, hdr.ipv4.totalLen, hdr.ipv4.totalLen - SIZEOF_UDPLB_HDR);

	// Apply the accumulated delta to the IPv4 header checksum
	hdr.ipv4.hdrChecksum = hdr.ipv4.hdrChecksum ^ 0xFFFF;
	cksum_add(hdr.ipv4.hdrChecksum, ckd);
	hdr.ipv4.hdrChecksum = hdr.ipv4.hdrChecksum ^ 0xFFFF;

	hdr.ethernet.dstAddr = mac_dst;
	hdr.ipv4.dstAddr = ip_dst;
	hdr.ipv4.totalLen = hdr.ipv4.totalLen - SIZEOF_UDPLB_HDR;

	new_udp_dst = udp_dst;
    }

    action do_ipv6_member_rewrite(bit<48> mac_dst, bit<128> ip_dst, bit<16> udp_dst) {
	// Calculate UDP pseudo header checksum delta using rfc1624 method

	cksum_swap(ckd, hdr.ipv6.dstAddr[63:48], ip_dst[63:48]);
	cksum_swap(ckd, hdr.ipv6.dstAddr[47:32], ip_dst[47:32]);
	cksum_swap(ckd, hdr.ipv6.dstAddr[31:16], ip_dst[31:16]);
	cksum_swap(ckd, hdr.ipv6.dstAddr[15:00], ip_dst[15:00]);

	cksum_swap(ckd, hdr.ipv6.payloadLen, hdr.ipv6.payloadLen - SIZEOF_UDPLB_HDR);

	hdr.ethernet.dstAddr = mac_dst;
	hdr.ipv6.dstAddr = ip_dst;
	hdr.ipv6.payloadLen = hdr.ipv6.payloadLen - 12;
	new_udp_dst = udp_dst;
    }

    table member_info_lookup_table {
	actions = {
	    do_ipv4_member_rewrite;
	    do_ipv6_member_rewrite;
	    drop;
	}
	key = {
	    hdr.ethernet.etherType : exact;
	    meta_member_id : exact;
	}
	size = 1024;
	default_action = drop;
    }

    // Entry Point
    apply {
	bool hit;

	//
	// DstFilter
	//
	
	// Normalize the IP destination address
	if (hdr.ipv4.isValid()) {
	    meta_ipdst = (bit<96>) 0 ++ (bit<32>) hdr.ipv4.dstAddr;
	} else if (hdr.ipv6.isValid()) {
	    meta_ipdst = hdr.ipv6.dstAddr;
	}
	
	hit = dst_filter_table.apply().hit;
	if (!hit) {
	    return;
	}

	//
	// EpochAssign
	//

	if (!hdr.udplb.isValid()) {
	    return;
	}

	hit = epoch_assign_table.apply().hit;
	if (!hit) {
	    return;
	}

	//
	// LoadBalanceCalendar
	//

	hit = load_balance_calendar_table.apply().hit;
	if (!hit) {
	    return;
	}

	//
	// MemberInfoLookup
	//

	hit = member_info_lookup_table.apply().hit;
	if (!hit) {
	    return;
	}

	//
	// UpdateUDPChecksum
	//

	// Calculate UDP pseudo header checksum delta using rfc1624 method

	cksum_swap(ckd, hdr.udp.dstPort, new_udp_dst);
	cksum_swap(ckd, hdr.udp.totalLen, hdr.udp.totalLen - SIZEOF_UDPLB_HDR);

	// Subtract out the bytes of the UDP load-balance header
	cksum_sub(ckd, hdr.udplb.magic);
	cksum_sub(ckd, hdr.udplb.version ++ hdr.udplb.proto);
	cksum_sub(ckd, hdr.udplb.tick[63:48]);
	cksum_sub(ckd, hdr.udplb.tick[47:32]);
	cksum_sub(ckd, hdr.udplb.tick[31:16]);
	cksum_sub(ckd, hdr.udplb.tick[15:00]);

	// Write the updated checksum back into the packet
	hdr.udp.checksum = hdr.udp.checksum ^ 0xFFFF;
	cksum_add(hdr.udp.checksum, ckd);
	hdr.udp.checksum = hdr.udp.checksum ^ 0xFFFF;

	// Update the destination port and fix up the length to adapt to the dropped udplb header
	hdr.udp.dstPort = new_udp_dst;
	hdr.udp.totalLen = hdr.udp.totalLen - SIZEOF_UDPLB_HDR;
    }
}

control DeparserImpl(packet_out packet, in headers hdr, inout short_metadata short_meta, inout standard_metadata_t smeta) {
    apply {
        packet.emit(hdr.ethernet);
        packet.emit(hdr.ipv4);
        packet.emit(hdr.ipv4_opt);
        packet.emit(hdr.ipv6);
        packet.emit(hdr.udp);
    }
}

XilinxPipeline(
  ParserImpl(),
  MatchActionImpl(),
  DeparserImpl()
) main;

