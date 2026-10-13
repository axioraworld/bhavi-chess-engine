# Changelog

## 5.3 (2026-10-13)
- New, larger NNUE network (768 hidden neurons, 8 output buckets), about +29 Elo over 5.2 in self-play testing.
- Stability fix: resolved a rare hang caused by simultaneous output from search and input threads.
- Search improvements: singular extensions and non-pawn correction history.
