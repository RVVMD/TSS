# TSS — Transient Stability Simulator

C-based power grid transient stability simulator using SUNDIALS IDA.

PSS/E RAW/DYR input → Y-bus → power flow → GENCLS machines → DAE integration → CSV output.

## Quick start

```bash
git clone https://github.com/RVVMD/TSS.git
cd TSS
make test    # build + run unit tests
make run     # build + run 3-phase fault simulation on IEEE 14-bus
```

## Dependencies

```bash
sudo apt install libsundials-dev libsuitesparse-dev liblapacke-dev mpich
```

## Make targets

| Target | Description |
|--------|-------------|
| `make` or `make build` | Configure (cmake) and build |
| `make test` | Build + run unit tests |
| `make run` | Build + fault simulation with events and plots |
| `make sim` | Build + steady-state simulation (no events) |
| `make clean` | Remove build/ and results/ |

## Manual build

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

## Usage

```bash
./build/src/transient-cli \
  --raw tests/data/ieee14.raw \
  --dyr tests/data/ieee14.dyr \
  --events tests/data/fault_ieee14.ini \
  --t-end 2.0 --t-step 0.01 \
  --output results --plot
```

### Options

| Flag | Default | Description |
|------|---------|-------------|
| `--raw FILE` | required | PSS/E RAW file |
| `--dyr FILE` | required | PSS/E DYR file |
| `--events FILE` | — | Event script (INI) |
| `--t-end SEC` | 10.0 | Simulation duration |
| `--t-step SEC` | 0.01 | Output interval |
| `--output DIR` | ./results | CSV output directory |
| `--plot` | off | Generate gnuplot PNGs |

## Test data

IEEE 14-bus test case included in `tests/data/`:

| File | Description |
|------|-------------|
| `ieee14.raw` | PSS/E v33 RAW — 14 buses, 20 branches, 5 generators |
| `ieee14.dyr` | GENCLS machines (H, D, X'd) for all 5 generators |
| `fault_ieee14.ini` | 3-phase fault at bus 4, t=1.0–1.3s |

## Output

```
results/
├── voltage.csv   # |V| at all buses (14 cols × N rows)
├── delta.csv     # δ and ω for all generators (10 cols × N rows)
├── voltage.png   # gnuplot voltage plot
└── delta.png     # gnuplot rotor angle plot
```

## Architecture

```
RAW/DYR ─→ Y-bus (CSC sparse) ─→ Power flow (Newton, LAPACKE dgesv)
                                        │
                                        ▼
                                  Machine init (Ep, δ, ω)
                                        │
                                        ▼
                                  DAE assembly (swing + network)
                                        │
                         ┌──────────────┴──────────────┐
                         │  SUNDIALS IDA (BDF1, dense) │
                         │  + event pipeline:           │
                         │    ybus_build → solve →     │
                         │    IDACalcIC → IDAReInit     │
                         └─────────────────────────────┘
                                        │
                                        ▼
                                   CSV + gnuplot
```

## License

MIT
