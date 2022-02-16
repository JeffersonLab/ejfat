P4_SOURCE = p4-udplb.p4

.PHONY: sim
sim: $(P4_SOURCE:.p4=.json) runsim.txt packets_in.meta packets_in.pcap
	run-p4bm-sdnet -j $(P4_SOURCE:.p4=.json) -s runsim.txt -l log
	@echo ''
	@echo 'Log files are in "log_cli.txt" and "log_model.txt"'
	@echo ''

.PHONY: build
build: $(P4_SOURCE:.p4=.json)

%.json: %.p4
	p4c-sdnet -o $@ $<

packets_in.pcap: pcap-generator.py
	./pcap-generator.py

.PHONY: clean
clean:
	rm -f $(P4_SOURCE:.p4=.json)
	rm -f packets_out.pcap
	rm -f packets_out.meta
	rm -f packets_in.pcap
	rm -f log_cli.txt log_model.txt

