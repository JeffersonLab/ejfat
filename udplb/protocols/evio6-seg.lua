--
local p_evio6seg = Proto("evio6seg", "EVIO6 Segmentation")

local f_version = ProtoField.uint8("eviogseg.version", "Version", base.DEC, nil, 0xF0)
local f_flags = ProtoField.uint16("evio6seg.flags", "Flags", base.HEX, nil, 0x0FFF)
local f_rsvd = ProtoField.bool("evio6seg.flags.rsvd", "Reserved", 16, nil, 0xFFC)
local f_first = ProtoField.bool("evio6seg.flags.first", "First Segment", 16, nil, 0x002)
local f_last = ProtoField.bool("evio6seg.flags.last", "Last Segment", 16, nil, 0x001)
local f_rocid = ProtoField.uint16("evio6seg.rocid", "ROC ID", base.HEX)
local f_offset = ProtoField.uint32("evio6seg.offset", "Offset", base.HEX)

p_evio6seg.fields = {
   f_version,
   f_flags,
   f_rsvd,
   f_first,
   f_last,
   f_rocid,
   f_offset,
}

-- field accessor function, used in the dissector
local offset = Field.new("evio6seg.offset")
local rocid = Field.new("evio6seg.rocid")

local data_dis = Dissector.get("data")

function p_evio6seg.dissector(buf, pkt, tree)
   local t = tree:add(p_evio6seg, buf(0,4))
   t:add(f_version, buf(0,1))

   local tflags = t:add(f_flags, buf(0,2))
   tflags:add(f_rsvd, buf(0,2))
   tflags:add(f_first, buf(0,2))
   tflags:add(f_last, buf(0,2))

   t:add(f_rocid, buf(2,2))
   t:add(f_offset, buf(4,4))

   data_dis:call(buf(8):tvb(), pkt, tree)

   pkt.cols.protocol:set("EVIO6SEG")
   pkt.cols.packet_len:set(buf(8):tvb():reported_length_remaining())
   pkt.cols.info:set("ROC: " .. string.format("0x%X", rocid()()) .. " Offset: " .. offset()())
			    
end

local udplb_encap_table = DissectorTable.get("udplb.proto")
udplb_encap_table:add(1, p_evio6seg)

local udp_encap_table = DissectorTable.get("udp.port")
udp_encap_table:add(0x4556, p_evio6seg)

