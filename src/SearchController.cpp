#include "SearchController.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <sstream>
#include <vector>
#include <thread>
#include <atomic>

#include "NegamaxQuinenseSearch.hpp"
#include "probe.h"

std::atomic<bool> stop_search(false);
chess::Move last_best_move = chess::Move::NO_MOVE;
std::mutex cout_mutex;

void sync_cout(const std::string& text) {
    std::lock_guard<std::mutex> lock(cout_mutex);
    std::cout << text << std::flush;
}

namespace {

std::string get_pv_string(chess::Board board, int depth) {
    std::string pv;
    chess::Move tt_move = chess::Move::NO_MOVE;
    int dummy_score = 0;

    for (int ply = 0; ply < depth; ++ply) {
        int dummy_eval = 0;
        if (!probe_tt(board.zobrist(), depth - ply, -100000, 100000,
                      dummy_score, dummy_eval, tt_move, 0)) {
            break;
        }
        if (tt_move == chess::Move::NO_MOVE) break;

        pv += chess::uci::moveToUci(tt_move) + " ";
        board.makeMove(tt_move);
    }
    return pv;
}

// Función que ejecuta un hilo secundario/helper en Lazy SMP
void helper_thread_worker(ThreadWorker worker, chess::Board board, int max_depth) {
    for (int current_depth = 1; current_depth <= max_depth; ++current_depth) {
        if (stop_search) break;
        chess::Board search_board = board;
        try {
            negamax(worker, search_board, current_depth, -100000, 100000, 0);
        } catch (const SearchAbortedException&) {
            break;
        }
    }
}

chess::Move iterative_deepening(chess::Board board, int max_depth, int max_time_ms, bool infinite, int num_threads) {
    chess::Move best_move = chess::Move::NO_MOVE;
    last_best_move = chess::Move::NO_MOVE;
    stop_search = false;
    init_search_time(max_time_ms, infinite);

    int alpha = -100000;
    int beta = 100000;
    int delta = 50;
    int score = 0;

    // 1. Crear estructuras ThreadWorker independientes para cada hilo
    std::vector<ThreadWorker> workers;
    for (int i = 0; i < num_threads; ++i) {
        ThreadWorker w;
        w.id = i;
        w.pos_history.clear();
        w.pos_history.push_back(board.hash());
        workers.push_back(w);
    }

    // 2. Lanzar hilos secundarios (helpers)
    std::vector<std::thread> helper_threads;
    for (int i = 1; i < num_threads; ++i) {
        helper_threads.emplace_back(helper_thread_worker, std::ref(workers[i]), board, max_depth);
    }

    // 3. El Hilo 0 (principal) ejecuta el Iterative Deepening
    for (int current_depth = 1; current_depth <= max_depth; ++current_depth) {
        if (stop_search) break;

        if (current_depth >= 5) {
            alpha = std::max(-100000, score - delta);
            beta = std::min(100000, score + delta);
        } else {
            alpha = -100000;
            beta = 100000;
        }

        bool search_failed = false;
        while (true) {
            chess::Board search_board = board;
            try {
                score = negamax(workers[0], search_board, current_depth, alpha, beta, 0);
            } catch (const SearchAbortedException&) {
                search_failed = true;
                break;
            }

            if (stop_search) {
                search_failed = true;
                break;
            }
            if (score <= alpha) {
                alpha = std::max(-100000, alpha - delta);
                delta += delta / 2;
            } else if (score >= beta) {
                beta = std::min(100000, beta + delta);
                delta += delta / 2;
            } else {
                break;
            }
        }

        if (search_failed || stop_search) break;
        delta = 50;

        chess::Move tt_move = chess::Move::NO_MOVE;
        int dummy_score = 0;
        int dummy_eval = 0;
        probe_tt(board.zobrist(), current_depth, -100000, 100000,
                 dummy_score, dummy_eval, tt_move, 0);
        if (tt_move != chess::Move::NO_MOVE) {
            best_move = tt_move;
            last_best_move = tt_move;
        }

        // 4. Sumar nodos acumulados de todos los hilos
        uint64_t nodos_totales = 0;
        for (const auto& w : workers) {
            nodos_totales += w.nodos_busqueda;
        }

        std::string pv_line = get_pv_string(board, current_depth);
        auto now = std::chrono::high_resolution_clock::now();
        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - search_start_time).count();
        uint64_t nps = elapsed_ms > 0
            ? static_cast<uint64_t>(nodos_totales) * 1000 / elapsed_ms
            : 0;

        std::stringstream info;
        info << "info depth " << current_depth
             << " score " << format_score_uci(score)
             << " nodes " << nodos_totales
             << " nps " << nps
             << " time " << elapsed_ms;
        if (!pv_line.empty()) {
            info << " pv " << pv_line;
        } else if (best_move != chess::Move::NO_MOVE) {
            info << " pv " << chess::uci::moveToUci(best_move);
        }
        info << "\n";
        sync_cout(info.str());
    }

    // Detener y unirse a los hilos helper al terminar
    stop_search = true;
    for (auto& t : helper_threads) {
        if (t.joinable()) t.join();
    }

    return best_move;
}

} // namespace

SearchController::~SearchController() {
    stop();
}

void SearchController::start(const chess::Board& board, int depth, int time_ms, bool infinite, int num_threads) {
    stop();
    stop_search = false;

    search_thread = std::thread([board, depth, time_ms, infinite, num_threads]() {
        chess::Move best_move = iterative_deepening(board, depth, time_ms, infinite, num_threads);
        chess::Move final_move = best_move != chess::Move::NO_MOVE ? best_move : last_best_move;

        std::stringstream output;
        output << "bestmove ";
        output << (final_move != chess::Move::NO_MOVE
            ? chess::uci::moveToUci(final_move)
            : "0000");
        output << "\n";
        sync_cout(output.str());
    });
}

void SearchController::stop() {
    stop_search = true;
    if (search_thread.joinable()) {
        search_thread.join();
    }
}