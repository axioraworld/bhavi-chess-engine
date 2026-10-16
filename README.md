# Bhavi Chess Engine

Bhavi is a chess engine written by Vinod Naraniwal, Sonam Naraniwal (AXIORA GLOBAL SYSTEMS PVT LTD), India.

## Latest release: 5.4

[Bhavi 5.4](https://github.com/axioraworld/bhavi-chess-engine/releases/tag/v5.4) (2026-10-16) has a new v7-h768 NNUE network, about +39.6 Elo over 5.3 in a 1358-game SPRT at 10+0.1. Downloads: `bhavi-5.4-avx2.exe` (Windows, AVX2), `bhavi-5.4-portable.exe` (Windows, any 64-bit CPU), `bhavi-5.4-linux-avx2` (Linux). The sections below describe the 5.3 package.

- Protocol: UCI
- Evaluation: NNUE, with the network embedded in the executable (no extra files needed)
- Multi-threaded search

## Files

| File | For |
|------|-----|
| `bhavi-5.3-x64-avx2.exe` | Modern 64-bit CPUs with AVX2 (most PCs since about 2015). Fastest. |
| `bhavi-5.3-x64-popcnt.exe` | Older 64-bit CPUs (SSE4.2/POPCNT). Use this if the AVX2 version does not start. |

## UCI options

| Option | Type | Default | Range |
|--------|------|---------|-------|
| Hash | spin | 64 (MB) | 1-4096 |
| Threads | spin | 1 | 1-256 |
| Ponder | check | false | |
| Move Overhead | spin | 30 (ms) | 0-5000 |
| Use NNUE | check | true | |
| EvalFile | string | `<embedded>` | path to an alternative network |
| OwnBook | check | false | |
| BookFile | string | `<empty>` | path to a .bin opening book |

## Usage

Bhavi has no graphical interface of its own. Load it into any UCI chess GUI:

- **Arena:** Engines > Install New Engine, select the `.exe`, choose UCI.
- **Cute Chess:** Tools > Settings > Engines > `+`, select the `.exe`, protocol UCI.
- **BanksiaGUI:** Engines > Add, select the `.exe`.

From a command prompt, `bhavi-5.3-x64-avx2.exe bench` runs a quick speed test.

## License

MIT License. Copyright (c) 2026 Vinod Naraniwal, Sonam Naraniwal (AXIORA GLOBAL SYSTEMS PVT LTD). See `LICENSE`.
