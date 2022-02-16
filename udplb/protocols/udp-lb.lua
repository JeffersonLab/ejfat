--
local p_udplb = Proto("udplb", "UDP Load Balancer Protocol")

local f_magic = ProtoField.string("udplb.magic", "Magic", base.ASCII)
local f_version = ProtoField.uint8("udplb.version", "Version", base.DEC)
local f_proto = ProtoField.uint8("udplb.proto", "Protocol", base.HEX)
local f_tick = ProtoField.uint64("udplb.tick", "Tick", base.HEX)

p_udplb.fields = {
   f_magic,
   f_version,
   f_proto,
   f_tick,
}

local p_udplb_encap_table = DissectorTable.new("udplb.proto", "UDP-LB Encap", ftypes.UINT8, base.DEC, p_udplb)

local proto = Field.new("udplb.proto")
local tick = Field.new("udplb.tick")

function p_udplb.dissector(buf, pkt, tree)
   local t = tree:add(p_udplb, buf(0, 12))
   t:add(f_magic, buf(0,2))
   t:add(f_version, buf(2,1))
   t:add(f_proto, buf(3,1))
   t:add(f_tick, buf(4,8))

   -- local proto = buf(13,1):uint()
   local dissector = p_udplb_encap_table:get_dissector(proto()())

   if dissector ~= nil then
      -- found a dissector
      dissector:call(buf(12):tvb(), pkt, tree)
   else
      pkt.cols.protocol:set("UDP-LB")
      pkt.cols.packet_len:set(buf:len())
      pkt.cols.info:set("Tick: " .. tick()())
   end

end

local udp_encap_table = DissectorTable.get("udp.port")
udp_encap_table:add(0x4c42, p_udplb)
