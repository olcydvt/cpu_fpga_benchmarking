// ============================================================
// test_generator.sv — produces test messages, writes them byte-by-byte
// into the async FIFO (write clock domain, wr_clk).
// This is the C++ producer equivalent (generates the test stream).
// Each message has incrementing price/qty (varied, realistic).
// Leaves a gap between messages (simulates real network spacing).
// Asserts last_byte_sent on the final byte of each message
// (used by the latency counter as the "message sent" timestamp).
// ============================================================
module test_generator #(
    parameter int GAP_CYCLES = 8    // idle cycles between messages
) (
    input  logic       clk,          // write clock (wr_clk)
    input  logic       rst_n,        // active-low reset

    // --- FIFO write interface ---
    output logic [7:0] wdata,        // byte to write
    output logic       wr_en,        // write enable
    input  logic       full,         // FIFO full (backpressure)

    // --- latency measurement hook ---
    output logic       last_byte_sent // pulses when the last byte of a msg is written
);

    localparam logic [7:0] SYNC = 8'hAA;

    // FSM: send 6 bytes, then wait GAP_CYCLES, then repeat with next message
    typedef enum logic [3:0] {
        S_SYNC, S_TYPE, S_PRICE_HI, S_PRICE_LO, S_QTY_HI, S_QTY_LO, S_GAP
    } state_t;
    state_t state;

    // message content — increments each message (varied data)
    logic [7:0]  msg_type;
    logic [15:0] msg_price;
    logic [15:0] msg_qty;

    // gap counter (idle between messages)
    logic [7:0] gap_cnt;

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state          <= S_SYNC;
            wdata          <= '0;
            wr_en          <= 1'b0;
            last_byte_sent <= 1'b0;
            msg_type       <= 8'h01;
            msg_price      <= 16'h0100;
            msg_qty        <= 16'h0010;
            gap_cnt        <= '0;
        end else begin
            // defaults each cycle
            wr_en          <= 1'b0;
            last_byte_sent <= 1'b0;

            // only advance if FIFO can accept a byte (respect backpressure)
            if (!full) begin
                case (state)
                    S_SYNC: begin
                        wdata <= SYNC;
                        wr_en <= 1'b1;
                        state <= S_TYPE;
                    end
                    S_TYPE: begin
                        wdata <= msg_type;
                        wr_en <= 1'b1;
                        state <= S_PRICE_HI;
                    end
                    S_PRICE_HI: begin
                        wdata <= msg_price[15:8];   // big-endian
                        wr_en <= 1'b1;
                        state <= S_PRICE_LO;
                    end
                    S_PRICE_LO: begin
                        wdata <= msg_price[7:0];
                        wr_en <= 1'b1;
                        state <= S_QTY_HI;
                    end
                    S_QTY_HI: begin
                        wdata <= msg_qty[15:8];
                        wr_en <= 1'b1;
                        state <= S_QTY_LO;
                    end
                    S_QTY_LO: begin
                        wdata          <= msg_qty[7:0];
                        wr_en          <= 1'b1;
                        last_byte_sent <= 1'b1;      // mark last byte (latency timing)
                        state          <= S_GAP;
                        gap_cnt        <= '0;
                    end
                    S_GAP: begin
                        // idle: write nothing, count gap cycles
                        if (gap_cnt >= GAP_CYCLES - 1) begin
                            // gap done: prepare next message (increment fields)
                            msg_type  <= msg_type  + 8'h01;
                            msg_price <= msg_price + 16'h0001;
                            msg_qty   <= msg_qty   + 16'h0001;
                            state     <= S_SYNC;
                        end else begin
                            gap_cnt <= gap_cnt + 8'h01;
                        end
                    end
                    default: state <= S_SYNC;
                endcase
            end
        end
    end
endmodule