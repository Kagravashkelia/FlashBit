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
inline std::vector<uint64_t> pos_history;
constexpr int NO_EVAL = 9999999; // Valor centinela fuera del rango normal de evaluación

inline bool is_repetition(const chess::Board& board) {
    int clock = board.halfMoveClock();
    int end = std::max(0, static_cast<int>(pos_history.size()) - clock);
    for (int i = static_cast<int>(pos_history.size()) - 2; i >= end; i -= 2) {
        if (pos_history[i] == board.hash()) {
            return true;
        }
    }
    return false;
}

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
// 1. TABLA DE TRANSPOSICIÓN (TT) Y TABLAS HISTÓRICAS
// ============================================================================
enum TTFlag : uint8_t {
    TT_EXACT = 0,
    TT_ALPHA = 1,
    TT_BETA = 2
};

constexpr int MATE_SCORE = 29000;
constexpr int DRAW_PENALTY = 20000;
constexpr int WINNING_EVAL = 150;

struct TTEntry {
    uint64_t key = 0;
    int depth = -1;
    int score = 0;
    int static_eval = NO_EVAL; // INICIALIZAR CON NO_EVAL
    TTFlag flag = TT_ALPHA;
    chess::Move best_move = chess::Move::NO_MOVE;
};

inline size_t TT_SIZE = 1048576; 
inline size_t TT_MASK = TT_SIZE - 1;
inline std::vector<TTEntry> TT(TT_SIZE);

inline void resize_tt(size_t size_mb) {
    size_t target_bytes = size_mb * 1024 * 1024;
    size_t num_entries = target_bytes / sizeof(TTEntry);
    TT_SIZE = 1;
    while ((TT_SIZE << 1) <= num_entries) TT_SIZE <<= 1;
    TT_MASK = TT_SIZE - 1;
    TT.clear();
    TT.resize(TT_SIZE);
}

inline void clear_tt() { std::fill(TT.begin(), TT.end(), TTEntry{}); }

inline int score_to_tt(int score, int ply) {
    if (score >= MATE_SCORE - 1000)  return score + ply;
    if (score <= -MATE_SCORE + 1000) return score - ply;
    return score;
}

inline int score_from_tt(int score, int ply) {
    if (score >= MATE_SCORE - 1000)  return score - ply;
    if (score <= -MATE_SCORE + 1000) return score + ply;
    return score;
}

std::string format_score_uci(int score) {
    if (score >= MATE_SCORE - 1000) {
        int mate_in = (MATE_SCORE - score + 1) / 2;
        return "mate " + std::to_string(mate_in);
    }
    if (score <= -MATE_SCORE + 1000) {
        int mate_in = (-MATE_SCORE - score) / 2; 
        return "mate " + std::to_string(mate_in);
    }
    return "cp " + std::to_string(score);
}

inline void store_tt(uint64_t key, int depth, int score, int static_eval, TTFlag flag, chess::Move best_move, int ply) {
    size_t index = key & TT_MASK;
    // CORRECCIÓN: Nunca sobrescribir profundidades mayores por un flag EXACT
    if (TT[index].key != key || depth >= TT[index].depth) {
        TT[index] = { key, depth, score_to_tt(score, ply), static_eval, flag, best_move };
    }
}

inline bool probe_tt(uint64_t key, int depth, int alpha, int beta, int& score, int& cached_eval, chess::Move& tt_move, int ply) {
    size_t index = key & TT_MASK;
    const TTEntry& entry = TT[index];
    if (entry.key == key) {
        tt_move = entry.best_move;
        cached_eval = entry.static_eval;
        if (entry.depth >= depth) {
            int tt_eval = score_from_tt(entry.score, ply);
            if (entry.flag == TT_EXACT) { score = tt_eval; return true; }
            // CORRECCIÓN: Devolver el tt_eval real, no truncarlo al bound
            if (entry.flag == TT_ALPHA && tt_eval <= alpha) { score = tt_eval; return true; }
            if (entry.flag == TT_BETA  && tt_eval >= beta)  { score = tt_eval; return true; }
        }
    }
    return false;
}

constexpr int MAX_PLY = 128;
inline chess::Move killer_moves[MAX_PLY][2];
inline int history_table[2][64][64];
inline int static_eval_stack[MAX_PLY];

inline void clear_search_history() {
    std::fill(&killer_moves[0][0], &killer_moves[0][0] + (MAX_PLY * 2), chess::Move::NO_MOVE);
    for (int c = 0; c < 2; ++c)
        for (int f = 0; f < 64; ++f)
            for (int t = 0; t < 64; ++t)
                history_table[c][f][t] = 0;
}

// ============================================================================
// 1.5. STATIC EXCHANGE EVALUATION (SEE)
// ============================================================================
constexpr int PIECE_VALUES[] = { 100, 300, 320, 500, 900, 20000, 0 };

inline chess::PieceType get_least_valuable_attacker(const chess::Board& board, chess::Square sq, 
                                                   chess::Color side, chess::Bitboard& occupied, 
                                                   chess::Square& attacker_sq) {
    chess::Bitboard pawns = board.pieces(chess::PieceType::PAWN, side) & occupied;
    chess::Bitboard pawn_attackers = chess::attacks::pawn(~side, sq) & pawns;
    if (pawn_attackers) { attacker_sq = static_cast<chess::Square>(pawn_attackers.lsb()); return chess::PieceType::PAWN; }

    chess::Bitboard knights = board.pieces(chess::PieceType::KNIGHT, side) & occupied;
    chess::Bitboard knight_attackers = chess::attacks::knight(sq) & knights;
    if (knight_attackers) { attacker_sq = static_cast<chess::Square>(knight_attackers.lsb()); return chess::PieceType::KNIGHT; }

    chess::Bitboard bishops = board.pieces(chess::PieceType::BISHOP, side) & occupied;
    chess::Bitboard bishop_attackers = chess::attacks::bishop(sq, occupied) & bishops;
    if (bishop_attackers) { attacker_sq = static_cast<chess::Square>(bishop_attackers.lsb()); return chess::PieceType::BISHOP; }

    chess::Bitboard rooks = board.pieces(chess::PieceType::ROOK, side) & occupied;
    chess::Bitboard rook_attackers = chess::attacks::rook(sq, occupied) & rooks;
    if (rook_attackers) { attacker_sq = static_cast<chess::Square>(rook_attackers.lsb()); return chess::PieceType::ROOK; }

    chess::Bitboard queens = board.pieces(chess::PieceType::QUEEN, side) & occupied;
    chess::Bitboard queen_attackers = chess::attacks::queen(sq, occupied) & queens;
    if (queen_attackers) { attacker_sq = static_cast<chess::Square>(queen_attackers.lsb()); return chess::PieceType::QUEEN; }

    chess::Bitboard king = board.pieces(chess::PieceType::KING, side) & occupied;
    chess::Bitboard king_attackers = chess::attacks::king(sq) & king;
    if (king_attackers) { attacker_sq = static_cast<chess::Square>(king_attackers.lsb()); return chess::PieceType::KING; }

    return chess::PieceType::NONE;
}

inline int see(const chess::Board& board, const chess::Move& move) {
    chess::Square from = move.from();
    chess::Square to = move.to();

    auto victim = board.at(to);
    int gain[32];
    int d = 0;

    chess::PieceType attacker_type = board.at(from).type();
    chess::Color side = board.sideToMove();
    
    gain[d] = (victim != chess::Piece::NONE) ? PIECE_VALUES[static_cast<int>(victim.type())] : 0;
    if (move.typeOf() == chess::Move::PROMOTION) {
        gain[d] += PIECE_VALUES[static_cast<int>(move.promotionType())] - PIECE_VALUES[static_cast<int>(chess::PieceType::PAWN)];
        attacker_type = move.promotionType();
    }

    chess::Bitboard occupied = board.occ();
    occupied.clear(from.index()); 

    chess::Square next_attacker_sq = from;
    int current_piece_val = PIECE_VALUES[static_cast<int>(attacker_type)];

    while (true) {
        d++;
        side = ~side; 
        gain[d] = current_piece_val - gain[d - 1];

        if (std::max(-gain[d - 1], gain[d]) < 0) break;

        chess::PieceType next_attacker = get_least_valuable_attacker(board, to, side, occupied, next_attacker_sq);
        if (next_attacker == chess::PieceType::NONE) break;

        occupied.clear(next_attacker_sq.index()); 
        current_piece_val = PIECE_VALUES[static_cast<int>(next_attacker)];
    }

    while (--d > 0) {
        gain[d - 1] = -std::max(-gain[d - 1], gain[d]);
    }

    return gain[0];
}

inline bool see_ge(const chess::Board& board, const chess::Move& move, int threshold) {
    return see(board, move) >= threshold;
}

// ============================================================================
// 2. ORDENAMIENTO DE JUGADAS (OPTIMIZADO O(1) - PSEUDO SEE)
// ============================================================================
struct ScoredMove {
    chess::Move move;
    int score;
};

inline int score_move(const chess::Board& board, const chess::Move& move, const chess::Move& tt_move, int ply) {
    if (move == tt_move) return 200000000; 

    bool is_cap = board.isCapture(move);
    bool is_prom = (move.typeOf() == chess::Move::PROMOTION);

    if (is_prom) {
        if (move.promotionType() == chess::PieceType::QUEEN) return 150000000;
        return -50000000; 
    }

    if (is_cap) {
        auto attacker = board.at(move.from());
        auto victim = board.at(move.to());
        
        int victim_val = (victim != chess::Piece::NONE) 
                         ? PIECE_VALUES[static_cast<int>(victim.type())] 
                         : 100;
        int attacker_val = PIECE_VALUES[static_cast<int>(attacker.type())];
        
        if (victim_val >= attacker_val) {
            return 50000000 + (victim_val * 10 - attacker_val);
        } else {
            return 10000000 + (victim_val * 10 - attacker_val);
        }
    }

    if (ply < MAX_PLY) {
        if (move == killer_moves[ply][0]) return 20000000;
        if (move == killer_moves[ply][1]) return 19000000;
    }

    int color = static_cast<int>(board.sideToMove());
    return history_table[color][move.from().index()][move.to().index()];
}

// ============================================================================
// 3. QUIESCENCE SEARCH (Con Delta Pruning O(1))
// ============================================================================
int quiescence(chess::Board& board, int alpha, int beta, int ply = 0) {
    nodos_totales_busqueda++;
    check_time_and_longjump();

    uint64_t key = board.hash();
    chess::Move tt_move = chess::Move::NO_MOVE;
    int tt_score = 0;
    int cached_eval = NO_EVAL;

    if (probe_tt(key, 0, alpha, beta, tt_score, cached_eval, tt_move, ply)) {
        return tt_score;
    }

    int stand_pat = (cached_eval != NO_EVAL) ? cached_eval : evaluate(board);
    if (stand_pat >= beta) return beta;
    if (alpha < stand_pat) alpha = stand_pat;

    chess::Movelist moves;
    chess::movegen::legalmoves<chess::movegen::MoveGenType::CAPTURE>(moves, board);

    std::array<ScoredMove, 256> scored_moves;
    size_t num_moves = moves.size();
    for (size_t i = 0; i < num_moves; ++i) {
        // CORRECCIÓN: Pasar el tt_move real recuperado arriba para priorizarlo
        scored_moves[i] = { moves[i], score_move(board, moves[i], tt_move, 0) };
    }

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

        auto victim = board.at(move.to());
        int victim_val = (victim != chess::Piece::NONE) ? PIECE_VALUES[static_cast<int>(victim.type())] : 100;
        if (move.typeOf() != chess::Move::PROMOTION && (stand_pat + victim_val + 200 < alpha)) {
            continue;
        }

        if (!see_ge(board, move, 0)) continue;

        board.makeMove(move);
        pos_history.push_back(board.hash());
        int score = -quiescence(board, -beta, -alpha, ply + 1);
        board.unmakeMove(move);
        pos_history.pop_back();

        if (score >= beta) return beta;
        if (score > alpha) alpha = score;
    }
    return alpha;
}

constexpr int FUTILITY_MARGIN[6] = { 0, 150, 300, 450, 600, 750 }; 
inline int LMR_TABLE[64][256];

// CORRECCIÓN: Inicialización estática automática (Garantiza que LMR funcione)
struct LMRInit {
    LMRInit() {
        for (int d = 0; d < 64; ++d) {
            for (int m = 0; m < 256; ++m) {
                if (d > 0 && m > 0) {
                    LMR_TABLE[d][m] = 1 + static_cast<int>(std::log(d) * std::log(m) / 2.25);
                } else {
                    LMR_TABLE[d][m] = 0;
                }
            }
        }
    }
};
inline LMRInit _lmr_init_instance;

// ============================================================================
// 4. BÚSQUEDA NEGAMAX PRINCIPAL (Con Lazy SEE)
// ============================================================================
int negamax(chess::Board& board, int depth, int alpha, int beta, int ply) {
    nodos_totales_busqueda++;
    check_time_and_longjump();

    if (ply > 0 && (is_repetition(board) || board.isHalfMoveDraw())) {
    int draw_eval = Stockfish::Probe::eval(board.getFen().c_str());

    // Si estamos ganando, la tabla es muy mala.
    // Si estamos perdiendo, la tabla es buena.
    if (draw_eval > WINNING_EVAL)
        return -DRAW_PENALTY;

    if (draw_eval < -WINNING_EVAL)
        return DRAW_PENALTY;

    return 0;  
    }

    int mate_val = MATE_SCORE - ply;
    if (alpha < -mate_val) alpha = -mate_val;
    if (beta > mate_val - 1) beta = mate_val - 1;
    if (alpha >= beta) return alpha;

    bool is_pv_node = (beta - alpha > 1);
    bool in_check = board.inCheck();
    int alpha_orig = alpha;
    uint64_t key = board.hash();
    chess::Move tt_move = chess::Move::NO_MOVE;

    int tt_score = 0;
    int cached_eval = NO_EVAL;
    if (probe_tt(key, depth, alpha, beta, tt_score, cached_eval, tt_move, ply)) {
        if (!is_pv_node || (tt_score > alpha && tt_score < beta)) return tt_score;
    }

    // CORRECCIÓN: NUNCA entrar en quiescence si estás en jaque (Bypass vital)
    if (depth <= 0 && !in_check) {
        return quiescence(board, alpha, beta, ply);
    }

    int extension = (in_check && ply < MAX_PLY - 1) ? 1 : 0;
    
    int eval = (cached_eval != NO_EVAL) ? cached_eval : Stockfish::Probe::eval(board.getFen().c_str());
    if (ply < MAX_PLY) static_eval_stack[ply] = eval;

    if (depth >= 4 && tt_move == chess::Move::NO_MOVE && (is_pv_node || eval + 256 >= beta)) {
        int iid_depth = depth - 2;
        negamax(board, iid_depth, alpha, beta, ply);
        probe_tt(key, 0, alpha, beta, tt_score, cached_eval, tt_move, ply);
    }

    bool improving = !in_check && ply >= 2 && eval >= static_eval_stack[ply - 2];

    if (!is_pv_node && !in_check) {
        // --- 1. RAZORING (NUEVO) ---
        // Si estamos destrozados en la evaluación, saltamos directo a Quiescence
        if (depth <= 2 && eval + 300 + (depth * 150) < alpha) {
            int q_score = quiescence(board, alpha, beta, ply);
            if (q_score <= alpha) return q_score;
        }

        if (depth <= 3 && eval - 120 * depth >= beta && std::abs(beta) < MATE_SCORE) {
            return eval - 120 * depth;
        }

        if (depth <= 3) {
            if (eval + 300 + (depth * 100) <= alpha) {
                int q_score = quiescence(board, alpha, beta, ply); 
                if (q_score <= alpha) return q_score;
            }
        }

        if (depth >= 2 && eval >= beta && board.hasNonPawnMaterial(board.sideToMove())) {
            board.makeNullMove();
            pos_history.push_back(board.hash()); // CORRECCIÓN: Mantener historia sincrónica
            // --- 2. NMP BOOST (AGRESIVO) ---
            // Aumentamos la reducción base a 4 para podar árboles enormes
            int R = 4 + (depth / 3) + std::min(3, (eval - beta) / 200) + (improving ? 1 : 0); 
            int null_score = -negamax(board, depth - 1 - R, -beta, -beta + 1, ply + 1);
            board.unmakeNullMove();
            pos_history.pop_back(); // CORRECCIÓN
            if (null_score >= beta) return beta;
        }

        // --- 3. RFP MEJORADO ---
        // Exigimos menos ventaja estática para dar por buena la posición y podar
        int rfp_margin = (improving ? 60 : 90) * depth;
        if (depth <= 5 && eval - rfp_margin >= beta && std::abs(beta) < MATE_SCORE) {
            return eval;
        }

        if (depth >= 5 && std::abs(beta) < MATE_SCORE) {
            int probcut_beta = beta + 200;
            chess::Movelist prob_moves;
            chess::movegen::legalmoves<chess::movegen::MoveGenType::CAPTURE>(prob_moves, board);
            
            for (int i = 0; i < static_cast<int>(prob_moves.size()); ++i) {
                if (!see_ge(board, prob_moves[i], probcut_beta - eval)) continue;
                
                board.makeMove(prob_moves[i]);
                pos_history.push_back(board.hash());
                int probcut_score = -negamax(board, depth - 4, -probcut_beta, -probcut_beta + 1, ply + 1);
                board.unmakeMove(prob_moves[i]);
                pos_history.pop_back();

                if (probcut_score >= probcut_beta) return probcut_beta;
            }
        }
    }

    bool futility_pruning = false;
    if (!is_pv_node && !in_check && depth <= 5 && std::abs(alpha) < MATE_SCORE) {
        if (eval + FUTILITY_MARGIN[depth] <= alpha) futility_pruning = true;
    }

    chess::Movelist moves;
    chess::movegen::legalmoves(moves, board);

    if (moves.empty()) {
    if (in_check)
        return -MATE_SCORE + ply;

    int draw_eval = Stockfish::Probe::eval(board.getFen().c_str());

    // Ahogado: si vamos ganando, lo consideramos desastroso.
    if (draw_eval > WINNING_EVAL)
        return -DRAW_PENALTY;

    if (draw_eval < -WINNING_EVAL)
        return DRAW_PENALTY;

    return 0;
    }

    std::array<ScoredMove, 256> scored_moves;
    size_t num_moves = moves.size();
    for (size_t i = 0; i < num_moves; ++i) {
        scored_moves[i] = { moves[i], score_move(board, moves[i], tt_move, ply) };
    }

    int best_score = -std::numeric_limits<int>::max();
    chess::Move best_move = chess::Move::NO_MOVE;
    int moves_searched = 0; 
    int quiet_moves_searched = 0;
    std::array<chess::Move, 64> searched_quiets;
    int num_searched_quiets = 0;

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
        chess::Color moving_side = board.sideToMove();
        bool is_capture = board.isCapture(move);
        bool is_promotion = (move.typeOf() == chess::Move::PROMOTION);
        bool is_quiet = !is_capture && !is_promotion;

        bool is_bad_capture = false;
        bool see_computed = false;

        auto evaluate_see = [&]() {
            if (!see_computed) {
                is_bad_capture = is_capture && !see_ge(board, move, 0);
                see_computed = true;
            }
        };

        if (is_capture && depth <= 3 && !is_pv_node && !in_check) {
            evaluate_see();
            if (is_bad_capture) continue;
        }

        if (is_quiet) {
            int hist = history_table[static_cast<int>(moving_side)][move.from().index()][move.to().index()];
            
            // PROTECCIÓN: Solo podamos si ya hemos buscado al menos una jugada (moves_searched > 0)
            if (moves_searched > 0 && depth <= 3 && hist < -512 && !is_pv_node && !in_check &&
                !see_ge(board, move, -50 * depth)) {
                continue;
            }

            // --- 5. HISTORY PRUNING INTENSO ---
            // Castigamos más rápido las jugadas que históricamente fallan
            int history_threshold = -2500 * depth;
            if (moves_searched > 0 && !is_pv_node && !in_check && depth <= 5 && hist < history_threshold) {
                continue;
            }
            

            // --- 4. LMP ESTRICTO CUADRÁTICO ---
            // Limita drásticamente las jugadas "basura" en los finales del bucle
            int lmp_threshold = 3 + (depth * depth) / (improving ? 1 : 2);
            if (!is_pv_node && !in_check && depth <= 7 && quiet_moves_searched >= lmp_threshold) {
                continue;
            }

            if (futility_pruning && moves_searched > 0) {
                continue;
            }

            quiet_moves_searched++;
            if (num_searched_quiets < 64) {
                searched_quiets[num_searched_quiets++] = move;
            }
        }

        board.makeMove(move);
        pos_history.push_back(board.hash());
        bool gives_check = board.inCheck();
        moves_searched++;
        
        int score;
        int new_depth = depth - 1 + extension;

        if (moves_searched == 1) {
            score = -negamax(board, new_depth, -beta, -alpha, ply + 1);
        } else {
            bool can_lmr = (depth >= 3 && moves_searched > 2 && !in_check && !gives_check && !is_pv_node && !is_promotion);

            if (can_lmr) {
                if (is_capture) evaluate_see();
                
                if (is_quiet || (is_capture && is_bad_capture)) {
                    int d = std::min(depth, 63);
                    int m = std::min(moves_searched, 255);
                    int R = LMR_TABLE[d][m];
                    
                    if (!is_pv_node) R++;
                    if (!improving) R++;
                    if (is_quiet) {
                        int hist = history_table[static_cast<int>(moving_side)][move.from().index()][move.to().index()];
                        if (hist < 0) R++;
                        else if (hist > 10000) R--;
                    }

                    R = std::clamp(R, 1, new_depth - 1);
                    score = -negamax(board, new_depth - R, -alpha - 1, -alpha, ply + 1);
                } else {
                    score = alpha + 1; 
                }
            } else {
                score = alpha + 1; 
            }

            if (score > alpha) {
                score = -negamax(board, new_depth, -alpha - 1, -alpha, ply + 1);
                if (score > alpha && score < beta) {
                    score = -negamax(board, new_depth, -beta, -alpha, ply + 1);
                }
            }
        }

        board.unmakeMove(move);
        pos_history.pop_back();

        if (score > best_score) {
            best_score = score;
            best_move = move;
        }
        if (score > alpha) alpha = score;

        if (alpha >= beta) { 
            if (is_quiet && ply < MAX_PLY) {
                // EVITAR CLONES: Solo actualizamos si la jugada no es ya el asesino primario
                if (move != killer_moves[ply][0]) {
                    killer_moves[ply][1] = killer_moves[ply][0];
                    killer_moves[ply][0] = move;
                }

                int color = static_cast<int>(board.sideToMove());
                int bonus = std::clamp(16 * depth * depth, 0, 1200);
                
                int& hist_ref = history_table[color][move.from().index()][move.to().index()];
                hist_ref += bonus - (hist_ref * std::abs(bonus) / 16384);

                for (int q = 0; q < num_searched_quiets - 1; ++q) {
                    const auto& q_move = searched_quiets[q];
                    int& bad_hist = history_table[color][q_move.from().index()][q_move.to().index()];
                    bad_hist -= bonus + (bad_hist * std::abs(bonus) / 16384);
                }
            }
            break; 
        }
    }

    if (best_score == -std::numeric_limits<int>::max()) {
        return alpha_orig;
    }

    TTFlag flag = TT_EXACT;
    if (best_score <= alpha_orig) flag = TT_ALPHA;
    else if (best_score >= beta) flag = TT_BETA;

    store_tt(key, depth, best_score, eval, flag, best_move, ply);

    return best_score;
}