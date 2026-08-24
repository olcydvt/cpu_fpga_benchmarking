// ============================================================
// feed_parser.sv — framer FSM (module named feed_parser_core
// to avoid clash with the top module 'feed_parser').
// Reads bytes from an async FIFO (FWFT mode), extracts messages.
// Message format: SYNC(0xAA) + TYPE(1) + PRICE(2, big-endian) + QTY(2, big-endian)
// Runs in the read clock domain (rd_clk / core clock).
// This is the C++ consumer/parser equivalent.
// ============================================================
module feed_parser_core (
    input  logic        clk,          // read clock (core clock, rd_clk)
    input  logic        rst_n,        // active-low reset

    // --- FIFO read interface (FWFT async FIFO) ---
    input  logic [7:0]  fifo_dout,    // FIFO read data (valid when not empty, FWFT)
    input  logic        fifo_empty,   // FIFO empty flag
    output logic        fifo_rd_en,   // FIFO read enable (advance to next byte)

    // --- decoded message output ---
    output logic [7:0]  msg_type,
    output logic [15:0] msg_price,
    output logic [15:0] msg_qty,
    output logic        msg_valid     // one-cycle pulse when a message is decoded
);

    // SYNC byte marks the start of a message
    localparam logic [7:0] SYNC = 8'hAA;

    // FSM states: one state per byte position in the message
    typedef enum logic [2:0] {
        IDLE, GET_TYPE, GET_PRICE_HI, GET_PRICE_LO, GET_QTY_HI, GET_QTY_LO
    } state_t;
    state_t state;

    // message field registers
    logic [7:0]  type_r;
    logic [15:0] price_r, qty_r;

    assign msg_type  = type_r;
    assign msg_price = price_r;
    assign msg_qty   = qty_r;

    // FWFT: read enable whenever FIFO has data. In FWFT mode the data is
    // already valid on fifo_dout, so we process it the same cycle.
    assign fifo_rd_en = !fifo_empty;

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state     <= IDLE;
            type_r    <= '0;
            price_r   <= '0;
            qty_r     <= '0;
            msg_valid <= 1'b0;
        end else begin
            msg_valid <= 1'b0;   // default: deassert (one-cycle pulse)

            // process a byte only when FIFO has valid data (FWFT)
            if (!fifo_empty) begin
                case (state)
                    IDLE:
                        if (fifo_dout == SYNC) state <= GET_TYPE;  // hunt for SYNC

                    GET_TYPE: begin
                        type_r <= fifo_dout;
                        state  <= GET_PRICE_HI;
                    end
                    GET_PRICE_HI: begin
                        price_r[15:8] <= fifo_dout;   // big-endian: high byte first
                        state <= GET_PRICE_LO;
                    end
                    GET_PRICE_LO: begin
                        price_r[7:0] <= fifo_dout;
                        state <= GET_QTY_HI;
                    end
                    GET_QTY_HI: begin
                        qty_r[15:8] <= fifo_dout;
                        state <= GET_QTY_LO;
                    end
                    GET_QTY_LO: begin
                        qty_r[7:0] <= fifo_dout;
                        msg_valid  <= 1'b1;           // message complete
                        state      <= IDLE;
                    end
                    default: state <= IDLE;
                endcase
            end
        end
    end
endmodule