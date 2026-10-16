# Changelog

## 5.4 (2026-10-16)
- New v7-h768 NNUE network trained on own self-play data, embedded in the binary.
- +39.6 ±12.9 Elo over 5.3 in 1,358 games at 10+0.1 (W496 D520 L342, SPRT passed).
- 100-game stability check: no crashes, illegal moves, or time losses.
- Bench: 1310786.
- Author: Vinod Naraniwal, Sonam Naraniwal (AXIORA GLOBAL SYSTEMS PVT LTD).
- Builds: Windows AVX2, Windows portable, Linux AVX2.
- Release: https://github.com/axioraworld/bhavi-chess-engine/releases/tag/v5.4

## 5.3 (2026-10-13)
- New, larger NNUE network (768 hidden neurons, 8 output buckets), about +29 Elo over 5.2 in self-play testing.
- Stability fix: resolved a rare hang caused by simultaneous output from search and input threads.
- Search improvements: singular extensions and non-pawn correction history.
