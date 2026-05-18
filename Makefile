.PHONY: build test run clean plot

BUILD_DIR := build
TEST_BIN := $(BUILD_DIR)/tests/test_ybus
CLI_BIN  := $(BUILD_DIR)/src/transient-cli

RAW   := tests/data/ieee14.raw
DYR   := tests/data/ieee14.dyr
EVTS  := tests/data/fault_ieee14.ini
OUT   := results
T_END := 3.0
STEP  := 0.01

MPI := mpirun -np 1 --allow-run-as-root

build:
	@mkdir -p $(BUILD_DIR)
	cd $(BUILD_DIR) && cmake .. && $(MAKE)

test: build
	$(MPI) $(TEST_BIN)

run: build
	mkdir -p $(OUT)
	$(MPI) $(CLI_BIN) \
		--raw $(RAW) --dyr $(DYR) --events $(EVTS) \
		--t-end $(T_END) --t-step $(STEP) --output $(OUT) --plot

sim: build
	mkdir -p $(OUT)
	$(MPI) $(CLI_BIN) \
		--raw $(RAW) --dyr $(DYR) \
		--t-end $(T_END) --t-step $(STEP) --output $(OUT) --plot

clean:
	rm -rf $(BUILD_DIR) $(OUT)

plot:
	@ls $(OUT)/*.png 2>/dev/null && echo "Plots in $(OUT)/" || echo "Run 'make run' first"
