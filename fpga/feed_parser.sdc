create_clock -period 10.000 -name pll_inst_CLKOUT0 [get_ports {pll_inst_CLKOUT0}]
create_clock -period 20.000 -name pll_inst_CLKOUT1 [get_ports {pll_inst_CLKOUT1}]
set_clock_groups -asynchronous -group {pll_inst_CLKOUT0} -group {pll_inst_CLKOUT1}