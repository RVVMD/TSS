# TSS — Transient Stability Simulator

C-based power grid transient stability simulator using SUNDIALS IDA.

PSS/E RAW/DYR input → Y-bus → power flow → GENCLS machines → DAE integration → CSV + plots.

## Quick start

```bash
sudo apt install libsundials-dev libsuitesparse-dev liblapacke-dev cmake make gcc mpich
git clone https://github.com/RVVMD/TSS.git
cd TSS
make test    # build + 10 unit tests
make run     # build + fault simulation with plots
```

## Usage

```
./build/src/transient-cli --raw <FILE> --dyr <FILE> [OPTIONS]
```

### Options

| Flag | Description |
|------|-------------|
| `--raw FILE` | PSS/E RAW file (required) |
| `--dyr FILE` | PSS/E DYR file (required) |
| `--events FILE` | Event script (INI format) |
| `--t-end SEC` | Simulation duration (default: 10.0) |
| `--t-step SEC` | Output interval (default: 0.01) |
| `--output DIR` | Output directory (default: ./results) |
| `--osc BUS` | 3-phase oscillogram at 1 kHz for bus ID |
| `--plot` | Generate gnuplot PNGs |
| `--info` | Print network topology report (no simulation) |
| `--diagram` | Print ASCII one-line network diagram |
| `--topo FILE` | Export Graphviz DOT topology graph |

### Examples

```bash
# Inspect a network
./build/src/transient-cli --raw tests/data/ieee14.raw --dyr tests/data/ieee14.dyr --info
./build/src/transient-cli --raw tests/data/ieee14.raw --dyr tests/data/ieee14.dyr --diagram

# Steady-state simulation (no faults)
./build/src/transient-cli --raw tests/data/ieee14.raw --dyr tests/data/ieee14.dyr --t-end 5.0 --plot

# 3-phase fault at bus 4
./build/src/transient-cli --raw tests/data/ieee14.raw --dyr tests/data/ieee14.dyr \
  --events tests/data/fault_ieee14.ini --t-end 3.0 --plot

# 3-phase oscillogram with DC offset + harmonics
./build/src/transient-cli --raw tests/data/ieee14.raw --dyr tests/data/ieee14.dyr \
  --events tests/data/fault_ieee14.ini --osc 4 --plot

# Single-line-to-ground fault
./build/src/transient-cli --raw tests/data/ieee14.raw --dyr tests/data/ieee14.dyr \
  --events tests/data/slg_fault.ini --osc 4 --plot
```

## Building custom networks

TSS reads standard PSS/E v33 RAW and DYR files. To build your own:

### 1. Create a RAW file

```raw
     0,   100.00            / case identification
YOUR CASE NAME
YOUR COMPANY
     1,'Slack     ',  69.0,3, 1,1,1, 1.00,  0.00,  0.0,  0.0, 1.05,0.95
     2,'Gen Bus   ',  69.0,2, 1,1,1, 1.00,  0.00,  0.0,  0.0, 1.05,0.95
     3,'Load Bus  ',  69.0,1, 1,1,1, 1.00,  0.00, 50.0, 20.0, 1.05,0.95
0 / END OF BUS DATA, BEGIN LOAD DATA
     1,3,   50.0,   20.0,  0,0,0,0,0,0
0 / END OF LOAD DATA, BEGIN GENERATOR DATA
     1,1,   80.0,   30.0, 9999,-9999,1.00,0,100.0,  0,0,0,1.0,1,100.0,350,0,1,1.0
     2,2,   30.0,   10.0, 9999,-9999,1.00,0,100.0,  0,0,0,1.0,1,100.0,100,0,1,1.0
0 / END OF GENERATOR DATA, BEGIN BRANCH DATA
     1,2,'1 ',  0.01,  0.10,  0.0, 9900,9900,9900, 0,0,0,0,1,1.0
     2,3,'1 ',  0.02,  0.20,  0.0, 9900,9900,9900, 0,0,0,0,1,1.0
0 / END OF BRANCH DATA
Q
```

Key fields per bus record (comma-delimited):
```
bus_id, 'name', base_kv, type, area, zone, owner, Vm, Va, Pd, Qd, Gs, Bs, Vmax, Vmin
```
Type: 1=PQ, 2=PV, 3=SLACK. Only one SLACK bus allowed.

### 2. Create a DYR file

```
bus_id,'GENCLS',machine_id, H, D, X'd
```

Example with D=5.0 damping:
```
1,'GENCLS',1,    5.000,      5.0,    0.10
2,'GENCLS',1,    5.000,      5.0,    0.15
```

One line per generator bus. H = inertia (MW·s/MVA), D = damping (pu), X'd = transient reactance (pu).

### 3. Create an event file (optional)

```ini
[event]
time = 1.0
type = fault        # fault | slg | ll | dlg | clear
bus = 3
r = 0.001           # fault resistance (pu)
x = 0.001           # fault reactance (pu)

[event]
time = 1.3
type = clear
bus = 3
```

### 4. Run

```bash
./build/src/transient-cli --raw mycase.raw --dyr mycase.dyr --info          # inspect
./build/src/transient-cli --raw mycase.raw --dyr mycase.dyr --diagram       # one-line
./build/src/transient-cli --raw mycase.raw --dyr mycase.dyr --t-end 5.0     # simulate
./build/src/transient-cli --raw mycase.raw --dyr mycase.dyr \               # with fault
  --events myevents.ini --t-end 3.0 --plot
```

A minimal 3-bus example is included at `tests/data/3bus.*`.

## Output files

| File | Contents |
|------|----------|
| `voltage.csv` | `time,V_1,ang_1,V_2,ang_2,...` |
| `delta.csv` | `time,d_g1,Pe_g1,Vt_g1,...` |
| `osc_busN.csv` | `time,Va,Vb,Vc` at 1 kHz (with DC offset + harmonics) |
| `*.png` | gnuplot graphs (when `--plot`) |

## Test data

| File | Description |
|------|-------------|
| `tests/data/ieee14.raw` | IEEE 14-bus — 14 buses, 20 branches, 5 gens |
| `tests/data/ieee14.dyr` | GENCLS machines (D=5.0) |
| `tests/data/3bus.raw` | Minimal 3-bus demo |
| `tests/data/3bus.dyr` | GENCLS for 3-bus |
| `tests/data/fault_ieee14.ini` | 3-phase fault at bus 4 |
| `tests/data/slg_fault.ini` | SLG fault at bus 4 |
| `tests/data/ll_fault.ini` | LL fault at bus 4 |
| `tests/data/3bus_fault.ini` | 3-phase fault for 3-bus demo |

## Supported features

| Feature | Status |
|---------|--------|
| PSS/E RAW parsing | ✓ |
| Y-bus (CSC sparse) | ✓ |
| Newton-Raphson power flow | ✓ |
| GENCLS classical model | ✓ |
| SUNDIALS IDA integration (BDF1) | ✓ |
| 3-phase, SLG, LL, DLG faults | ✓ |
| Fault clearing | ✓ |
| 3-phase oscillograms (1 kHz) | ✓ |
| DC offset + saturation harmonics | ✓ |
| Event scripting (INI) | ✓ |
| Network topology report (`--info`) | ✓ |
| ASCII one-line diagram (`--diagram`) | ✓ |
| DOT graph export (`--topo`) | ✓ |
| Post-simulation summary | ✓ |

## License

MIT
