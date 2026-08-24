// ============================================================
// top.sv — TOP MODULE, named 'feed_parser' to match the
// Interface Designer template. Target: Trion T20Q144C3.
// Chain: test_generator (wr_clk) -> async FIFO (CDC) -> feed_parser_core (rd_clk)
//        -> latency_counter.
// This is the C++ main() equivalent (wires producer, ring buffer, consumer).
// Clocks come from the PLL via the Interface Designer as peripheral ports.
// ============================================================
module feed_parser
(
    // --- peripheral ports from Interface Designer (keep attributes!) ---
    (* syn_peri_port = 0 *) input clk_in,              // 50 MHz oscillator (to PLL)
    (* syn_peri_port = 0 *) input pll_locked,          // PLL locked
    (* syn_peri_port = 0 *) input pll_inst_CLKOUT0,    // 100 MHz -> rd_clk
    (* syn_peri_port = 0 *) input pll_inst_CLKOUT1    // 50 MHz -> wr_clk

);

    // --- clock aliases (readable names) ---
    wire rd_clk = pll_inst_CLKOUT0;
    wire wr_clk = pll_inst_CLKOUT1;

    // --- reset: hold until PLL locks (active-low). No external reset pin yet. ---
    wire rst_n = pll_locked;

    // ========================================================
    // BLOCK 1: Test Generator (wr_clk domain) — C++ producer
    // ========================================================
    wire [7:0] gen_wdata;
    wire       gen_wr_en;
    wire       fifo_full;
    wire       last_byte_sent;   // pulse on last byte of each message (wr_clk)

    test_generator #(.GAP_CYCLES(8)) gen_inst (
        .clk            (wr_clk),
        .rst_n          (rst_n),
        .wdata          (gen_wdata),
        .wr_en          (gen_wr_en),
        .full           (fifo_full),
        .last_byte_sent (last_byte_sent)
    );

    // ========================================================
    // BLOCK 2: Async FIFO (CDC bridge, wr_clk -> rd_clk) — C++ ring buffer
    // IP-generated. a_rst_i is ACTIVE-HIGH -> invert rst_n.
    // ========================================================
    wire [7:0] fifo_dout;
    wire       fifo_empty;
    wire       fifo_rd_en;

    async_fifo fifo_inst (
        // write side (wr_clk)
        .wr_clk_i   (wr_clk),
        .wr_en_i    (gen_wr_en),
        .wdata      (gen_wdata),
        .full_o     (fifo_full),
        // read side (rd_clk)
        .rd_clk_i   (rd_clk),
        .rd_en_i    (fifo_rd_en),
        .rdata      (fifo_dout),
        .empty_o    (fifo_empty),
        // reset (ACTIVE-HIGH!)
        .a_rst_i    (~rst_n),
        // unused optional outputs left open
        .almost_full_o  (),
        .prog_full_o    (),
        .wr_ack_o       (),
        .almost_empty_o (),
        .rd_valid_o     (),
        .rst_busy       (),
        .wr_datacount_o (),
        .rd_datacount_o (),
        .underflow_o    (),
        .overflow_o     ()
    );

    // ========================================================
    // BLOCK 3: Framer FSM (rd_clk domain) — C++ consumer/parser
    // ========================================================
    wire [7:0]  msg_type;
    wire [15:0] msg_price;
    wire [15:0] msg_qty;
    wire        msg_valid;

    feed_parser_core parser_inst (
        .clk        (rd_clk),
        .rst_n      (rst_n),
        .fifo_dout  (fifo_dout),
        .fifo_empty (fifo_empty),
        .fifo_rd_en (fifo_rd_en),
        .msg_type   (msg_type),
        .msg_price  (msg_price),
        .msg_qty    (msg_qty),
        .msg_valid  (msg_valid)
    );

    // ========================================================
    // BLOCK 4: Latency Counter (rd_clk domain) — C++ rdtsc equivalent
    // ========================================================
    // wire [15:0] latency_cycles;
    // wire        latency_valid;
    
    (* syn_keep = 1, syn_preserve = 1 *) wire [15:0] latency_cycles;
    (* syn_keep = 1, syn_preserve = 1 *) wire        latency_valid;

    latency_counter lat_inst (
        .rd_clk            (rd_clk),
        .rst_n             (rst_n),
        .last_byte_sent_wr (last_byte_sent),
        .msg_valid         (msg_valid),
        .latency_cycles    (latency_cycles),
        .latency_valid     (latency_valid)
    );
    

    // ========================================================
    // No LED/reset pins yet. latency_cycles / latency_valid are read
    // via the Logic Analyzer (Debugger), which taps these nets by name.
    // ========================================================

endmodule