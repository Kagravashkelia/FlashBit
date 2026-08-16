#pragma once
#include <limits>
#include <vector>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <stdexcept>
#include <cmath> 
#include "pst.hpp"
#include "chesslib.hpp"
#include "evaluate.hpp"

// ============================================================================
// 0. CONTROL DE TIEMPO Y LONGJUMP (Zero-Cost Overhead)
// ============================================================================
extern std::atomic<bool> stop_search;
struct SearchAbortedException : public std::exception {};

inline std::chrono::high_resolution_clock::time_point search_start_time;
inline int search_max_time_ms = 0;
inline bool search_is_infinite = false;
inline uint64_t nodos_totales_busqueda = 0;

inline void init_search_time(int max_time_ms, bool infinite = false) {
    search_start_time = std::chrono::high_resolution_clock::now();
    search_max_time_ms = max_time_ms;
    search_is_infinite = infinite;
    nodos_totales_busqueda = 0;
}

inline void check_time_and_longjump() {
    if (stop_search.load(std::memory_order_relaxed)) {
        throw SearchAbortedException();
    }
    
    // Solo revisar el reloj cada 4096 nodos para evitar overhead masivo en CPU
    if ((nodos_totales_busqueda & 4095) == 0) {
        if (!search_is_infinite && search_max_time_ms > 0) {
            auto now = std::chrono::high_resolution_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - search_start_time).count();
            if (elapsed >= search_max_time_ms) {
                throw SearchAbortedException(); 
            }
        }
    }
}

// ============================================================================
// 1. TABLAS PRE-CALCULADAS Y OPTIMIZACIONES CONSTANTES (Lambdas C++17)
// ============================================================================

// LMR Table hiper-agresiva. Reducimos el divisor de 1.95 a 1.25.
// Las jugadas tardías sufrirán reducciones masivas (estilo Stockfish).
inline const std::array<std::array<int, 256>, 64> LMR_TABLE = []() {
    std::array<std::array<int, 256>, 64> table{};
    for (int d = 1; d < 64; ++d) {
        for (int m = 1; m < 256; ++m) {
            // Factor 1.25 = Reducción extrema para ramificaciones altas
            double r = std::log(d) * std::log(m) / 1.25;
            table[d][m] = static_cast<int>(r);
        }
    }
    return table;
}();

// LMP Table radical. En lugar de explorar la cola, cortamos casi de inmediato.
// Fórmula: 1 + (depth * depth) / 2.
inline const std::array<int, 64> LMP_TABLE = []() {
    std::array<int, 64> table{};
    for (int d = 1; d < 64; ++d) {
        table[d] = 1 + ((d * d) / 2); 
    }
    return table;
}();

// Futility extendido hasta profundidad 9 para aplastar nodos mediocres
constexpr int FUTILITY_MARGIN[9] = { 0, 100, 200, 300, 400, 500, 600, 700, 800 }; 

// ============================================================================
// 2. TABLA DE TRANSPOSICIÓN (TT) Y TABLAS HISTÓRICAS
// ============================================================================
enum TTFlag { TT_EXACT, TT_ALPHA, TT_BETA };
constexpr int MATE_SCORE = 29000;

struct TTEntry {
    uint64_t key = 0;
    int depth = -1;
    int score = 0;
    TTFlag flag = TT_ALPHA;
    chess::Move best_move = chess::Move::NO_MOVE;
};

inline size_t TT_SIZE = 1048576; 
inline size_t TT_MASK = TT_SIZE - 1;
inline std::vector<TTEntry> TT = std::vector<TTEntry>(TT_SIZE, TTEntry{0, -1, 0, TT_ALPHA, chess::Move::NO_MOVE});

inline void resize_tt(size_t size_mb) {
    size_t target_bytes = size_mb * 1024 * 1024;
    size_t num_entries = target_bytes / sizeof(TTEntry);
    TT_SIZE = 1;
    while ((TT_SIZE << 1) <= num_entries) TT_SIZE <<= 1;
    TT_MASK = TT_SIZE - 1;
    TT.clear();
    TT.resize(TT_SIZE, TTEntry{0, -1, 0, TT_ALPHA, chess::Move::NO_MOVE});
}

inline void clear_tt() { std::fill(TT.begin(), TT.end(), TTEntry{}); }

inline int score_to_tt(int score, int ply) {
    if (score > MATE_SCORE - 1000)  return score + ply;
    if (score < -MATE_SCORE + 1000) return score - ply;
    return score;
}

inline int score_from_tt(int score, int ply) {
    if (score > MATE_SCORE - 1000)  return score - ply;
    if (score < -MATE_SCORE + 1000) return score + ply;
    return score;
}

inline void store_tt(uint64_t key, int depth, int score, TTFlag flag, chess::Move best_move, int ply) {
    size_t index = key & TT_MASK;
    if (TT[index].key == 0 || depth >= TT[index].depth || flag == TT_EXACT || TT[index].key != key) {
        TT[index] = { key, depth, score_to_tt(score, ply), flag, best_move };
    }
}

inline bool probe_tt(uint64_t key, int depth, int alpha, int beta, int& score, chess::Move& tt_move, int ply) {
    size_t index = key & TT_MASK;
    const TTEntry& entry = TT[index];
    if (entry.key == key) {
        tt_move = entry.best_move;
        if (entry.depth >= depth) {
            int tt_eval = score_from_tt(entry.score, ply);
            if (entry.flag == TT_EXACT) { score = tt_eval; return true; }
            if (entry.flag == TT_ALPHA && tt_eval <= alpha) { score = alpha; return true; }
            if (entry.flag == TT_BETA  && tt_eval >= beta)  { score = beta; return true; }
        }
    }
    return false;
}

constexpr int MAX_PLY = 128;
inline chess::Move killer_moves[MAX_PLY][2];
inline int history_table[2][64][64];
inline int capture_history[6][6][64]; 

inline void clear_search_history() {
    std::fill(&killer_moves[0][0], &killer_moves[0][0] + (MAX_PLY * 2), chess::Move::NO_MOVE);
    for (int c = 0; c < 2; ++c)
        for (int f = 0; f < 64; ++f)
            for (int t = 0; t < 64; ++t)
                history_table[c][f][t] = 0; 
                
    for (int a = 0; a < 6; ++a)
        for (int v = 0; v < 6; ++v)
            for (int s = 0; s < 64; ++s)
                capture_history[a][v][s] = 0;
}

// ============================================================================
// 3. ORDENAMIENTO DE JUGADAS 
// ============================================================================
constexpr int PIECE_VALUES[] = { 100, 300, 320, 500, 900, 20000, 0 };

struct ScoredMove {
    chess::Move move;
    int score;
};

inline int score_move(const chess::Board& board, const chess::Move& move, const chess::Move& tt_move, int ply) {
    if (move == tt_move) return 20000000; 

    if (board.isCapture(move)) {
        auto attacker = board.at(move.from());
        auto victim = board.at(move.to());
        int v_type = static_cast<int>(victim.type());
        int a_type = static_cast<int>(attacker.type());
        
        int victim_val = (victim != chess::Piece::NONE) ? PIECE_VALUES[v_type] : 100;
        int attacker_val = PIECE_VALUES[a_type];
        
        int base_capture = 10000000 + (victim_val * 10 - attacker_val);
        return base_capture + capture_history[a_type][v_type][move.to().index()];
    }
    
    if (move.typeOf() == chess::Move::PROMOTION) return 9000000;

    if (ply < MAX_PLY) {
        if (move == killer_moves[ply][0]) return 900000;
        if (move == killer_moves[ply][1]) return 800000;
    }

    int color = static_cast<int>(board.sideToMove());
    return history_table[color][move.from().index()][move.to().index()];
}

// ============================================================================
// 4. QUIESCENCE SEARCH (Optimizada con Prefetch y Delta Pruning Agresivo)
// ============================================================================
int quiescence(chess::Board& board, int alpha, int beta) {
    nodos_totales_busqueda++;
    check_time_and_longjump();

    uint64_t key = board.hash();
    
    #if defined(__GNUC__) || defined(__clang__)
    __builtin_prefetch(&TT[key & TT_MASK]);
    #endif

    int tt_score = 0;
    chess::Move tt_move = chess::Move::NO_MOVE;
    
    if (probe_tt(key, 0, alpha, beta, tt_score, tt_move, 0)) {
        return tt_score;
    }

    // 1. Declaramos alpha_orig al inicio para que esté disponible en todo el scope
    const int alpha_orig = alpha; 
    
    bool in_check = board.inCheck();
    int stand_pat = in_check ? -MATE_SCORE : evaluate(board);

    // 2. Evaluamos Stand Pat solo si no estamos en jaque
    if (!in_check) {
        if (stand_pat >= beta) {
            store_tt(key, 0, stand_pat, TT_BETA, chess::Move::NO_MOVE, 0);
            return beta;
        }
        if (alpha < stand_pat) alpha = stand_pat;
    }

    chess::Movelist moves;
    if (in_check) {
        chess::movegen::legalmoves(moves, board);
    } else {
        chess::movegen::legalmoves<chess::movegen::MoveGenType::CAPTURE>(moves, board);
    }

    std::array<ScoredMove, 256> scored_moves;
    size_t num_moves = moves.size();
    for (size_t i = 0; i < num_moves; ++i) {
        scored_moves[i] = { moves[i], score_move(board, moves[i], tt_move, 0) };
    }

    int best_score = stand_pat;
    chess::Move best_move = chess::Move::NO_MOVE;

    for (size_t i = 0; i < num_moves; ++i) {
        size_t best_idx = i;
        int max_score = scored_moves[i].score;
        for (size_t j = i + 1; j < num_moves; ++j) {
            if (scored_moves[j].score > max_score) {
                max_score = scored_moves[j].score;
                best_idx = j;
            }
        }
        std::swap(scored_moves[i], scored_moves[best_idx]);

        const auto& move = scored_moves[i].move;
        
        // Delta Pruning / Pseudo-SEE (Solo si no estamos en jaque)
        if (!in_check && move.typeOf() != chess::Move::PROMOTION) {
            auto victim = board.at(move.to());
            int victim_val = (victim != chess::Piece::NONE) ? PIECE_VALUES[static_cast<int>(victim.type())] : 100;
            
            if (stand_pat + victim_val + 200 < alpha) continue;
            
            auto attacker = board.at(move.from());
            int attacker_val = (attacker != chess::Piece::NONE) ? PIECE_VALUES[static_cast<int>(attacker.type())] : 100;
            if (attacker_val > victim_val && stand_pat + victim_val < alpha) continue;
        }

        board.makeMove(move);
        int score = -quiescence(board, -beta, -alpha);
        board.unmakeMove(move);

        if (score > best_score) {
            best_score = score;
            best_move = move;
        }

        if (score >= beta) {
            store_tt(key, 0, beta, TT_BETA, best_move, 0);
            return beta;
        }
        if (score > alpha) alpha = score;
    }

    // Ahora alpha_orig está visible y funciona perfectamente aquí
    TTFlag flag = (best_score > alpha_orig) ? TT_EXACT : TT_ALPHA;
    store_tt(key, 0, best_score, flag, best_move, 0);
    return alpha;
}

// ============================================================================
// 5. BÚSQUEDA NEGAMAX PRINCIPAL (Con PVS Corregido y Color Cacheado)
// ============================================================================
int negamax(chess::Board& board, int depth, int alpha, int beta, int ply) {
    nodos_totales_busqueda++;
    check_time_and_longjump();

    uint64_t key = board.hash();
    
    // PREFETCH TEMPRANO
    #if defined(__GNUC__) || defined(__clang__)
    __builtin_prefetch(&TT[key & TT_MASK]);
    #endif

    bool is_pv_node = (beta - alpha > 1);
    bool in_check = board.inCheck();
    int alpha_orig = alpha;
    
    // CACHÉ DE COLOR: Evita llamar y calcular el lado a mover múltiples veces
    int us = static_cast<int>(board.sideToMove()); 
    
    chess::Move tt_move = chess::Move::NO_MOVE;
    int tt_score = 0;
    
    if (probe_tt(key, depth, alpha, beta, tt_score, tt_move, ply)) {
        if (!is_pv_node || (tt_score > alpha && tt_score < beta)) return tt_score;
    }

    if (depth <= 0) {
        return quiescence(board, alpha, beta);
    }

    if (depth >= 4 && tt_move == chess::Move::NO_MOVE && !is_pv_node) {
        depth--;
    }

    // Límite de extensiones para evitar stack overflows
    int extension = (in_check && ply < MAX_PLY) ? 1 : 0;
    int eval = evaluate(board);

    // ========================================================================
    // PODAS PREVIAS AL MOVIMIENTO
    // ========================================================================
    if (!is_pv_node && !in_check) {
        if (depth <= 4) {
            if (eval + 300 + (depth * 100) <= alpha) {
                int q_score = quiescence(board, alpha, beta);
                if (q_score <= alpha) return q_score;
            }
        }

        if (depth >= 3 && eval >= beta && board.hasNonPawnMaterial(static_cast<chess::Color>(us)) && std::abs(eval) < MATE_SCORE - 1000) {
            board.makeNullMove();
            int R = 4 + (depth / 2) + std::min(3, (eval - beta) / 200); 
            int null_score = -negamax(board, depth - 1 - R, -beta, -beta + 1, ply + 1);
            board.unmakeNullMove();

            if (null_score >= beta) {
                return (null_score >= MATE_SCORE - 1000) ? beta : null_score;
            }
        }

        if (depth <= 7 && eval - 90 * depth >= beta && std::abs(beta) < MATE_SCORE - 1000) {
            return eval;
        }
    }

    bool futility_pruning = false;
    if (!is_pv_node && !in_check && depth <= 8 && std::abs(alpha) < MATE_SCORE - 1000) {
        if (eval + FUTILITY_MARGIN[depth] <= alpha) futility_pruning = true;
    }

    chess::Movelist moves;
    chess::movegen::legalmoves(moves, board);

    if (moves.empty()) return in_check ? -MATE_SCORE + ply : 0;

    std::array<ScoredMove, 256> scored_moves;
    size_t num_moves = moves.size();
    for (size_t i = 0; i < num_moves; ++i) {
        scored_moves[i] = { moves[i], score_move(board, moves[i], tt_move, ply) };
    }

    int best_score = -std::numeric_limits<int>::max();
    chess::Move best_move = chess::Move::NO_MOVE;
    int moves_searched = 0; 
    int quiet_moves_searched = 0;
    int lmp_threshold = LMP_TABLE[std::min(depth, 63)];

    for (size_t i = 0; i < num_moves; ++i) {
        size_t best_idx = i;
        int max_score = scored_moves[i].score;
        for (size_t j = i + 1; j < num_moves; ++j) {
            if (scored_moves[j].score > max_score) {
                max_score = scored_moves[j].score;
                best_idx = j;
            }
        }
        std::swap(scored_moves[i], scored_moves[best_idx]);

        const auto& move = scored_moves[i].move;
        bool is_capture = board.isCapture(move);
        bool is_promotion = (move.typeOf() == chess::Move::PROMOTION);
        bool is_quiet = !is_capture && !is_promotion;

        if (is_quiet) {
            quiet_moves_searched++;

            if (!is_pv_node && !in_check && depth <= 4 && max_score < -1000) continue; 
            if (!is_pv_node && !in_check && depth <= 8 && quiet_moves_searched > lmp_threshold) continue; 
            if (futility_pruning && moves_searched > 0) continue;
        }

        board.makeMove(move);
        bool gives_check = board.inCheck();
        moves_searched++;
        
        int score;
        int new_depth = depth - 1 + extension;

        // PVS (Principal Variation Search) Corregido y Optimizado
        if (moves_searched == 1) {
            // Nodo Principal: Ventana Completa
            score = -negamax(board, new_depth, -beta, -alpha, ply + 1);
        } else {
            bool can_lmr = (depth >= 2 && !in_check && !is_capture && !gives_check && !is_pv_node && !is_promotion);

            if (can_lmr) {
                int lmr_depth = std::min(depth, 63);
                int lmr_moves = std::min(moves_searched, 255);
                int R = LMR_TABLE[lmr_depth][lmr_moves];
                
                int hist_val = history_table[us][move.from().index()][move.to().index()];
                
                if (is_quiet) {
                    // MEJORA STOCKFISH: Ajuste dinámico basado en historia.
                    // Escala de historia: +/- 16384. Dividimos entre 8192 para obtener un ajuste de +/- 2 plies.
                    R -= hist_val / 8192; 
                    
                    // Ajustes de límites de seguridad (ya los tenías)
                    if (hist_val < 0) R++;
                    else if (hist_val > 10000) R--;
                }

                R = std::max(0, std::min(R, new_depth - 1)); // Seguridad del índice
                
                score = -negamax(board, new_depth - R, -alpha - 1, -alpha, ply + 1);
                
                if (score > alpha && R > 0) {
                    score = -negamax(board, new_depth, -alpha - 1, -alpha, ply + 1);
                }
            } else {
                // Jugadas normales sin LMR se buscan con ventana nula directamente
                score = -negamax(board, new_depth, -alpha - 1, -alpha, ply + 1);
            }

            // 3. Si encontramos una mejora sobre alpha en las ventanas nulas, hacer búsqueda completa
            if (score > alpha && score < beta) {
                score = -negamax(board, new_depth, -beta, -alpha, ply + 1);
            }
        }

        board.unmakeMove(move);

        if (score > best_score) {
            best_score = score;
            best_move = move;
        }
        if (score > alpha) alpha = score;

        if (alpha >= beta) { 
            int bonus = std::min(depth * depth, 400); 

            if (is_quiet && ply < MAX_PLY) {
                killer_moves[ply][1] = killer_moves[ply][0];
                killer_moves[ply][0] = move;

                // Utilizamos el color 'us' previamente cacheado (MUCHO más rápido)
                int& hist = history_table[us][move.from().index()][move.to().index()];
                hist += bonus - (hist * std::abs(bonus) / 16384);

                for (size_t p = 0; p < i; ++p) {
                    const auto& penalized_move = scored_moves[p].move;
                    if (!board.isCapture(penalized_move) && penalized_move.typeOf() != chess::Move::PROMOTION) {
                        int& p_hist = history_table[us][penalized_move.from().index()][penalized_move.to().index()];
                        p_hist -= bonus + (p_hist * std::abs(bonus) / 16384);
                    }
                }
            } 
            else if (is_capture) {
                auto attacker = board.at(move.from());
                auto victim = board.at(move.to()); 
                if (attacker != chess::Piece::NONE && victim != chess::Piece::NONE) {
                    int a_type = static_cast<int>(attacker.type());
                    int v_type = static_cast<int>(victim.type());
                    int& c_hist = capture_history[a_type][v_type][move.to().index()];
                    c_hist += bonus - (c_hist * std::abs(bonus) / 16384);
                }
            }
            break; 
        }
    }

    TTFlag flag = TT_EXACT;
    if (best_score <= alpha_orig) flag = TT_ALPHA;
    else if (best_score >= beta) flag = TT_BETA;

    store_tt(key, depth, best_score, flag, best_move, ply);

    return best_score;
}