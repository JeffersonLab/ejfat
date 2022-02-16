# Install System Prerequisites

```
sudo pip3 install scapy
sudo apt install tshark
```

# Install wireshark/tshark dissectors for udp-lb and evio6-seg protocol headers

```
mkdir -p ~/.local/lib/wireshark/plugins
cat protocols/{udp-lb,evio6-seg}.lua > ~/.local/lib/wireshark/plugins/jlab-stack.lua
```

The awkward concatenaton of the two dissectors is to fix the non-deterministic order that wireshark/tshark uses to load plugins.

# Set up the vivado tools environment

```
source /opt/Xilinx/Vivado/2020.2/settings64.sh
source /opt/Xilinx/SDNet/2020.2/settings64.sh
export XILINXD_LICENSE_FILE=2200@callicserv.lbl.gov:27004@engvlic3.lbl.gov
```

# Simulate the P4 pipeline

**Note**: All simulation instructions are relative to the `sim` subdirectory.

```
cd sim
make sim

#p4c-sdnet -o p4-udplb.json p4-udplb.p4
#./pcap-generator.py
#run-p4bm-sdnet -j p4-udplb.json -s runsim.txt -l log
#WARNING: /opt/Xilinx/SDNet/2020.2/tps/lnx64/jre does not exist.
#WARNING: /opt/Xilinx/SDNet/2020.2/tps/lnx64/jre does not exist.

```

This takes care of all of these steps for you:
  - generate a set of simulation input packets (`packets_in.pcap`)
  - compile (using p4c-sdnet) your p4 program into the IR (`p4-udplb.json`) required by the simulator
  - run the p4 behavioural model controlled by a script (`runsim.txt`) which will
    - preload pipleine table entries
	- read input packets (`packets_in.pcap`) and metadata (`packets_in.meta`)
	- run your p4 program on each packet
  - captures output packets (`packets_out.pcap`) and metadata (`packets_out.meta`)
  - captures output log files (`log_cli.txt` and `log_model.txt`)

## Displaying the simulation input packets (optional)

You can display information about the simulated input packets using `capinfos` and `tshark`.

```
capinfos packets_in.pcap
tshark -r packets_in.pcap -O udplb,evio6seg
```

## Display the output packets (optional)

```
capinfos packets_out.pcap
tshark -r packets_out.pcap -O ip,ipv6,udp,udplb,evio6seg -o ip.check_checksum:TRUE -o udp.check_checksum:TRUE
```
