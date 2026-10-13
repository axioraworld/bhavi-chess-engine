/**
 * Copyright (c) 2026 Vinod Naraniwal, Sonam Naraniwal (AXIORA GLOBAL SYSTEMS PVT LTD)
 * License: MIT
 *
 * main.cpp: UCI protocol loop plus helper commands:
 *   perft <depth> [fen]   exact move-path counts and speed
 *   bench [depth]         fixed-depth search over test positions
 *   eval                  static evaluation of the current position
 *   d                     print FEN
 */
#include "book.h"
#include "datagen.h"
#include "eval.h"
#include "movegen.h"
#include "nnue.h"
#include "search.h"
#include "tt.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <thread>

static const char *START_FEN = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";
#ifndef BHAVI_VERSION
#define BHAVI_VERSION "5.3"
#endif
static const char *ENGINE_NAME = "Bhavi " BHAVI_VERSION;

static Position rootPos;
static bool ownBook = false;

static void stopSearch() { Search::stop(); }

static void setPosition(std::istringstream &is) {
  std::string tok, fen;
  is >> tok;
  if (tok == "startpos") {
    fen = START_FEN;
    is >> tok;  // "moves" or nothing
  } else if (tok == "fen") {
    while (is >> tok && tok != "moves") fen += tok + " ";
  } else return;
  if (!rootPos.setFen(fen)) {
    std::cout << "info string invalid fen" << std::endl;
    rootPos.setFen(START_FEN);
    return;
  }
  while (is >> tok) {
    Move m = rootPos.parseUci(tok);
    if (!m) {
      std::cout << "info string illegal move " << tok << std::endl;
      break;
    }
    rootPos.make(m);
    rootPos.rebase();
  }
}

static const char *BENCH_FENS[] = {
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
    "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
    "r1bq1rk1/pp2bppp/2n2n2/2pp4/3P4/2PBPN2/PP1N1PPP/R2QK2R w KQ - 0 9",
    "2r3k1/pp3ppp/4p3/3pP3/3P4/1P3N2/P4PPP/2R3K1 w - - 0 25",
    "r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10",
    "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",
    "6k1/5ppp/8/8/8/8/5PPP/3R2K1 w - - 0 1",
    "r1bqkb1r/pppp1ppp/2n2n2/4p3/2B1P3/5N2/PPPP1PPP/RNBQK2R w KQkq - 4 4",
};

int main(int argc, char **argv) {
  std::ios::sync_with_stdio(false);
  std::cin.tie(nullptr);  // getline must not flush std::cout while the search thread writes to it
  initBitboards();
  Zobrist::init();
  Eval::init();
  Search::init();
  TT.resize(64);
  // Default network: embedded in the binary if built with one, else nets/default.nnue.
  if (!NNUE::loadEmbedded()) NNUE::loadFile("nets/default.nnue");
  const char *nnueEnv = std::getenv("AXIORA_NNUE");
  if (nnueEnv && std::string(nnueEnv) == "0") NNUE::enabled = false;
  if (const char *sf = std::getenv("BHAVI_SF")) Search::features = std::atoi(sf);
  rootPos.setFen(START_FEN);

  // Command-line mode: run the arguments as one command, then exit.
  std::string cmdLine;
  for (int i = 1; i < argc; i++) cmdLine += std::string(argv[i]) + (i + 1 < argc ? " " : "");

  std::string line;
  bool once = !cmdLine.empty();
  while (once || std::getline(std::cin, line)) {
    if (once) line = cmdLine;
    std::istringstream is(line);
    std::string cmd;
    is >> cmd;
    if (cmd == "uci") {
      std::lock_guard<std::mutex> io(Search::ioMutex);
      std::cout << "id name " << ENGINE_NAME << "\nid author Vinod Naraniwal, Sonam Naraniwal (AXIORA GLOBAL SYSTEMS PVT LTD)\n"
                << "option name Hash type spin default 64 min 1 max 4096\n"
                << "option name Threads type spin default 1 min 1 max 256\n"
                << "option name Ponder type check default false\n"
                << "option name Move Overhead type spin default 30 min 0 max 5000\n"
                << "option name Use NNUE type check default true\n"
                << "option name EvalFile type string default <embedded>\n"
                << "option name NNUE Weight type spin default 100 min 0 max 100\n"
                << "option name OwnBook type check default false\n"
                << "option name BookFile type string default <empty>\n"
#ifdef BHAVI_TEST
                << "option name SFeatures type spin default " << Search::features << " min 0 max 1048575\n"
#endif
                << "uciok" << std::endl;
    } else if (cmd == "isready") {
      std::lock_guard<std::mutex> io(Search::ioMutex);
      std::cout << "readyok" << std::endl;
    } else if (cmd == "ucinewgame") {
      stopSearch();
      Search::clear();
    } else if (cmd == "setoption") {
      std::string tok, name, value;
      is >> tok;  // name
      while (is >> tok && tok != "value") name += (name.empty() ? "" : " ") + tok;
      while (is >> tok) value += (value.empty() ? "" : " ") + tok;
      if (name == "Hash") { stopSearch(); TT.resize(std::max(1, std::min(65536, std::atoi(value.c_str())))); }
      else if (name == "Threads") { stopSearch(); Search::setThreads(std::atoi(value.c_str())); }
      else if (name == "Ponder") {}  // the GUI decides when to ponder
      else if (name == "SFeatures") Search::features = std::atoi(value.c_str());  // hidden, for testing
      else if (name == "Move Overhead") Search::moveOverhead = std::max(0, std::atoi(value.c_str()));
      else if (name == "Use NNUE") { stopSearch(); NNUE::enabled = (value == "true"); Eval::clearCaches(); }
      else if (name == "NNUE Weight") { stopSearch(); Eval::nnueWeight = std::max(0, std::min(100, std::atoi(value.c_str()))); Eval::clearCaches(); }
      else if (name == "OwnBook") ownBook = (value == "true");
      else if (name == "BookFile") {
        stopSearch();
        if (value.empty() || value == "<empty>") Book::unload();
        else {
          bool ok = Book::load(value);
          std::cout << "info string " << (ok ? "book " + value + " entries " + std::to_string(Book::size()) : "could not load book " + value) << std::endl;
        }
      }
      else if (name == "EvalFile") {
        stopSearch();
        bool ok = (value == "<embedded>" || value.empty()) ? NNUE::loadEmbedded() : NNUE::loadFile(value);
        std::cout << "info string " << (ok ? "loaded net " + NNUE::netName() + " (hidden " + std::to_string(NNUE::hidden()) + ")"
                                           : "could not load net " + value) << std::endl;
        Eval::clearCaches();
      }
    } else if (cmd == "position") {
      stopSearch();
      setPosition(is);
    } else if (cmd == "go") {
      stopSearch();
      Search::Limits lim;
      std::string tok;
      while (is >> tok) {
        if (tok == "movetime") is >> lim.movetime;
        else if (tok == "wtime") is >> lim.time[WHITE];
        else if (tok == "btime") is >> lim.time[BLACK];
        else if (tok == "winc") is >> lim.inc[WHITE];
        else if (tok == "binc") is >> lim.inc[BLACK];
        else if (tok == "movestogo") is >> lim.movestogo;
        else if (tok == "ponder") lim.ponder = true;
        else if (tok == "depth") is >> lim.depth;
        else if (tok == "nodes") is >> lim.nodes;
        else if (tok == "infinite") lim.infinite = true;
      }
      // Book move: answer at once (not while pondering or analysing).
      Move bm = ownBook && Book::loaded() && !lim.infinite && !lim.ponder ? Book::probe(rootPos) : NO_MOVE;
      if (bm) {
        std::cout << "info string book move\nbestmove " << moveToUci(bm) << std::endl;
      } else {
        Search::start(rootPos, lim);
        if (once) Search::wait();
      }
    } else if (cmd == "stop") {
      stopSearch();
    } else if (cmd == "ponderhit") {
      Search::ponderhit();
    } else if (cmd == "quit") {
      stopSearch();
      break;
    } else if (cmd == "bookkey") {
      char buf[32];
      std::snprintf(buf, sizeof buf, "%016llx", (unsigned long long)Book::key(rootPos));
      Move bm = Book::probe(rootPos, true);
      std::cout << "bookkey " << buf << " best " << (bm ? moveToUci(bm) : "none") << std::endl;
    } else if (cmd == "d") {
      std::cout << rootPos.fen() << std::endl;
    } else if (cmd == "eval") {
      std::cout << "eval hce " << Eval::evaluateFull(rootPos);
      if (NNUE::loaded()) std::cout << " nnue " << NNUE::evaluate(rootPos) << " (refresh " << NNUE::evaluateRefresh(rootPos) << ")";
      std::cout << std::endl;
    } else if (cmd == "evalfens") {
      // Read FENs from stdin, print side-to-move static evaluation per line.
      std::string f;
      Position p;
      while (std::getline(std::cin, f)) {
        if (f.empty()) continue;
        if (!p.setFen(f)) { std::cout << "bad" << std::endl; continue; }
        std::cout << Eval::evaluateStatic(p) << "\n";
      }
      std::cout.flush();
      break;
    } else if (cmd == "datagen") {
      // datagen <seconds> <nodes> <seed> <outfile>
      double secs = 60;
      uint64_t nodes = 5000, seed = 1;
      std::string out = "data.bin";
      is >> secs >> nodes >> seed >> out;
      runDatagen(out, secs, nodes, seed);
    } else if (cmd == "nnuecheck") {
      // nnuecheck <depth> [fen]: incremental accumulator vs full refresh on every node.
      // "nnuecheck leaf <depth> [fen]" only checks leaves (multi-move replays).
      int depth = 3;
      bool leaf = false;
      std::string tok;
      is >> tok;
      if (tok == "leaf") { leaf = true; is >> depth; } else depth = std::stoi(tok);
      std::string rest;
      while (is >> tok) rest += tok + " ";
      Position p;
      p.setFen(rest.empty() ? rootPos.fen() : rest);
      uint64_t checked = 0, bad = NNUE::verify(p, depth, checked, leaf);
      std::cout << "nnuecheck nodes " << checked << " mismatches " << bad << std::endl;
    } else if (cmd == "perft") {
      int depth = 1;
      is >> depth;
      std::string rest, tok;
      while (is >> tok) rest += tok + " ";
      Position p;
      p.setFen(rest.empty() ? rootPos.fen() : rest);
      auto t = std::chrono::steady_clock::now();
      uint64_t n = perft(p, depth);
      double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
      std::cout << "perft " << depth << " nodes " << n << " time " << int(s * 1000) << " ms nps "
                << uint64_t(n / (s > 0 ? s : 1e-9)) << std::endl;
    } else if (cmd == "bench") {
      int depth = 12;
      is >> depth;
      uint64_t total = 0;
      auto t = std::chrono::steady_clock::now();
      for (const char *f : BENCH_FENS) {
        Search::clear();
        Position p;
        p.setFen(f);
        Search::Limits lim;
        lim.depth = depth;
        uint64_t n = 0;
        Search::go(p, lim, false, &n);
        total += n;
      }
      double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
      std::cout << "bench depth " << depth << " nodes " << total << " time " << int(s * 1000) << " ms nps "
                << uint64_t(total / s) << std::endl;
    }
    if (once) break;
  }
  Search::shutdown();
  return 0;
}
