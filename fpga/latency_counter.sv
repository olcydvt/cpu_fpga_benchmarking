// ============================================================
// latency_counter.sv — measures parse latency in rd_clk cycles.
// FPGA equivalent of the C++ rdtsc measurement.
// Counts rd_clk cycles from "last byte sent" (wr_clk domain,
// synchronized here) to "message decoded" (msg_valid, rd_clk domain).
// last_byte_sent crosses from wr_clk with a two-flop synchronizer (CDC),
// then edge-detected. Valid because rd_clk > wr_clk (pulse wide enough).
// ============================================================
module latency_counter (
    input  logic        rd_clk,        // read/core clock (measurement domain)
    input  logic        rst_n,         // active-low reset

    input  logic        last_byte_sent_wr,  // pulse from generator (wr_clk domain!)
    input  logic        msg_valid,          // pulse from framer (rd_clk domain)

    output logic [15:0] latency_cycles,     // measured latency (rd_clk cycles)
    output logic        latency_valid       // pulses when a new measurement is ready
);

    // --- CDC: bring last_byte_sent (wr_clk) into rd_clk with two flops ---
    // rd_clk is faster than wr_clk, so the wr_clk-wide pulse is guaranteed
    // to be sampled by at least one rd_clk edge.
    logic sync_ff1, sync_ff2, sync_ff3;
    always_ff @(posedge rd_clk or negedge rst_n) begin
        if (!rst_n) begin
            sync_ff1 <= 1'b0;
            sync_ff2 <= 1'b0;
            sync_ff3 <= 1'b0;
        end else begin
            sync_ff1 <= last_byte_sent_wr;  // may be metastable
            sync_ff2 <= sync_ff1;           // settled
            sync_ff3 <= sync_ff2;           // for edge detection
        end
    end

    // rising edge of synchronized pulse = "last byte just arrived"
    wire last_byte_edge = sync_ff2 & ~sync_ff3;

    // --- Measurement FSM: idle -> counting -> done ---
    typedef enum logic [1:0] { IDLE, COUNTING } meas_state_t;
    meas_state_t mstate;

    logic [15:0] counter;

    always_ff @(posedge rd_clk or negedge rst_n) begin
        if (!rst_n) begin
            mstate         <= IDLE;
            counter        <= '0;
            latency_cycles <= '0;
            latency_valid  <= 1'b0;
        end else begin
            latency_valid <= 1'b0;   // default: one-cycle pulse

            case (mstate)
                IDLE:
                    if (last_byte_edge) begin
                        counter <= 16'd1;       // last byte entered read domain
                        mstate  <= COUNTING;
                    end

                COUNTING: begin
                    counter <= counter + 16'd1;
                    if (msg_valid) begin
                        latency_cycles <= counter;   // decoded: latch the count
                        latency_valid  <= 1'b1;
                        mstate         <= IDLE;
                    end
                end

                default: mstate <= IDLE;
            endcase
        end
    end
endmodule