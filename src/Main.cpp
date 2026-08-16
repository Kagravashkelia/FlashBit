#include <iostream>
#include <string>
#include <sstream>
#include <vector>
#include <chrono>
#include <atomic>
#include <thread>
#include <algorithm>
#include <mutex>

#include "chesslib.hpp"
#include "NegamaxQuinenseSearch.hpp"

// ============================================================================
// VARIABLES GLOBALES DE ESTADO Y SINCRONIZACIÓN
// ============================================================================
std::atomic<bool> stop_search(false);
chess::Move last_best_move = chess::Move::NO_MOVE;
std::thread search_thread;
std::mutex cout_mutex; 

// HISTORIAL PARA EVITAR TRIPLE REPETICIÓN
std::vector<uint64_t> game_history;

// ============================================================================
// FUNCIONES AUXILIARES
// ============================================================================

void sync_cout(const std::string& text) {
    std::lock_guard<std::mutex> lock(cout_mutex);
    std::cout << text << std::flush;
}

void stop_current_search() {
    stop_search = true;
    if (search_thread.joinable()) {
        search_thread.join();
    }
}

// Gestión de tiempo mejorada y más segura
inline int calculate_time(int wtime, int btime, int winc, int binc, int movestogo, chess::Color sideToMove) {
    int time_left = (sideToMove == chess::Color::WHITE) ? wtime : btime;
    int inc       = (sideToMove == chess::Color::WHITE) ? winc  : binc;

    if (time_left <= 0) return 10;

    // Fórmula conservadora: asume que quedan unas 25 jugadas en promedio
    int moves = (movestogo > 0) ? movestogo : 25; 
    int time_allocated = (time_left / moves) + (inc * 3 / 4);

    // Margen de seguridad estricto para evitar perder por tiempo
    if (time_allocated > time_left) time_allocated = time_left - 100;
    
    return std::max(20, time_allocated - 50); 
}

// Extrae la variante principal (PV) blindada contra jugadas ilegales de la TT
inline std::string get_pv_string(chess::Board board, int depth) {
    std::string pv_str = "";
    chess::Move tt_move = chess::Move::NO_MOVE;
    int dummy_score = 0;

    for (int i = 0; i < depth; ++i) {
        if (!probe_tt(board.hash(), depth - i, -MATE_SCORE * 2, MATE_SCORE * 2, dummy_score, tt_move, 0)) {
            break;
        }
        if (tt_move == chess::Move::NO_MOVE) break;

        // BARRERA DE SEGURIDAD CONTRA CRASHEOS DE EVALUACIÓN
        // Verificamos que el movimiento de la TT sea realmente legal en este tablero
        chess::Movelist moves;
        chess::movegen::legalmoves(moves, board);
        bool is_legal = false;
        for (const auto& m : moves) {
            if (m == tt_move) {
                is_legal = true;
                break;
            }
        }

        if (!is_legal) break; // Si hubo colisión en la TT, abortamos la línea PV aquí

        pv_str += chess::uci::moveToUci(tt_move) + " ";
        board.makeMove(tt_move);
    }
    return pv_str;
}

// ============================================================================
// BÚSQUEDA POR PROFUNDIZACIÓN ITERATIVA
// ============================================================================
chess::Move iterative_deepening(chess::Board board, int max_prof, int max_time_ms, bool infinite) {
    chess::Move best_move_overall = chess::Move::NO_MOVE;
    last_best_move = chess::Move::NO_MOVE;
    stop_search = false;

    init_search_time(max_time_ms, infinite);

    chess::Movelist legal_moves;
    chess::movegen::legalmoves(legal_moves, board);
    if (legal_moves.empty()) return chess::Move::NO_MOVE; 
    
    best_move_overall = legal_moves[0]; // Salvavidas

    int alpha = -MATE_SCORE * 2;
    int beta  =  MATE_SCORE * 2;
    int delta =  30; 
    int score =  0;

    for (int current_depth = 1; current_depth <= max_prof; ++current_depth) {
        if (stop_search) break;

        if (current_depth >= 4) {
            alpha = std::max(-MATE_SCORE * 2, score - delta);
            beta  = std::min( MATE_SCORE * 2, score + delta);
        } else {
            alpha = -MATE_SCORE * 2;
            beta  =  MATE_SCORE * 2;
        }

        bool search_failed = false;

        while (true) {
            chess::Board search_board = board; 
            
            try {
                score = negamax(search_board, current_depth, alpha, beta, 0);
            } 
            catch (const SearchAbortedException&) {
                search_failed = true;
                break; 
            }

            if (stop_search) { search_failed = true; break; }

            // Aspiration Window Fail-Low
            if (score <= alpha) {
                alpha = std::max(-MATE_SCORE * 2, alpha - delta);
                delta += delta + (delta / 2);
            }
            // Aspiration Window Fail-High
            else if (score >= beta) {
                beta = std::min(MATE_SCORE * 2, beta + delta);
                delta += delta + (delta / 2);
            }
            else {
                break; // Dentro de la ventana
            }
        }

        if (search_failed || stop_search) break;

        delta = 30; // Resetear ventana para siguiente profundidad

        // Recuperar mejor jugada
        chess::Move tt_move = chess::Move::NO_MOVE;
        int dummy_score = 0;
        probe_tt(board.hash(), current_depth, -MATE_SCORE * 2, MATE_SCORE * 2, dummy_score, tt_move, 0);

        if (tt_move != chess::Move::NO_MOVE) {
            best_move_overall = tt_move;
            last_best_move = tt_move;
        }

        auto current_time = std::chrono::high_resolution_clock::now();
        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(current_time - search_start_time).count();

        std::string score_str;
        if (score > MATE_SCORE - 1000) {
            int moves_to_mate = (MATE_SCORE - score + 1) / 2;
            score_str = "mate " + std::to_string(moves_to_mate);
        } else if (score < -MATE_SCORE + 1000) {
            int moves_to_mate = (-MATE_SCORE - score) / 2;
            score_str = "mate " + std::to_string(moves_to_mate);
        } else {
            score_str = "cp " + std::to_string(score);
        }

        std::string pv_line = get_pv_string(board, current_depth);

        std::stringstream ss_info;
        long long nps = (elapsed_ms > 0) ? (nodos_totales_busqueda * 1000) / elapsed_ms : 0;
        ss_info << "info depth " << current_depth
                << " score " << score_str
                << " nodes " << nodos_totales_busqueda
                << " nps " << nps
                << " time " << elapsed_ms;

        if (!pv_line.empty()) {
            ss_info << " pv " << pv_line;
        } else if (best_move_overall != chess::Move::NO_MOVE) {
            ss_info << " pv " << chess::uci::moveToUci(best_move_overall);
        }
        
        ss_info << "\n";
        sync_cout(ss_info.str());
    }

    return best_move_overall;
}

// ============================================================================
// BUCLE PRINCIPAL / PROTOCOLO UCI
// ============================================================================
int main() {
    std::ios_base::sync_with_stdio(false);
    std::cin.tie(NULL);

    chess::Board board;
    std::string line;

    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;

        std::stringstream ss(line);
        std::string command;
        ss >> command;

        if (command == "uci") {
            sync_cout("id name Stormfish\nid author Anthony\noption name Hash type spin default 16 min 1 max 1024\nuciok\n");
        } 
        else if (command == "isready") {
            sync_cout("readyok\n");
        } 
        else if (command == "ucinewgame") {
            stop_current_search();
            clear_tt();
            clear_search_history(); // IMPORTANTE: Limpiar historiales entre partidas
            game_history.clear();   // NUEVO: Limpiamos historial de repetición
            board = chess::Board();
            sync_cout("readyok\n");
        } 
        else if (command == "position") {
            stop_current_search();

            std::string pos_type;
            ss >> pos_type;

            if (pos_type == "startpos") {
                board = chess::Board();
                game_history.clear();                   // NUEVO
                game_history.push_back(board.hash());   // NUEVO

                std::string moves_token;
                if (ss >> moves_token && moves_token == "moves") {
                    std::string move_str;
                    while (ss >> move_str) {
                        chess::Move move = chess::uci::uciToMove(board, move_str);
                        if (move != chess::Move::NO_MOVE) {
                            board.makeMove(move);
                            game_history.push_back(board.hash()); // NUEVO
                        }
                    }
                }
            } 
            else if (pos_type == "fen") {
                std::string fen = "";
                std::string token;
                
                while (ss >> token && token != "moves") {
                    if (!fen.empty()) fen += " ";
                    fen += token;
                }
                
                board.setFen(fen);
                game_history.clear();                 // NUEVO
                game_history.push_back(board.hash()); // NUEVO

                if (token == "moves") {
                    std::string move_str;
                    while (ss >> move_str) {
                        chess::Move move = chess::uci::uciToMove(board, move_str);
                        if (move != chess::Move::NO_MOVE) {
                            board.makeMove(move);
                            game_history.push_back(board.hash()); // NUEVO
                        }
                    }
                }
            }
        } 
        else if (command == "go") {
            stop_current_search();

            int depth = 64;
            int movetime = 0;
            int wtime = 0, btime = 0, winc = 0, binc = 0, movestogo = 0;
            bool infinite = false;
            bool depth_specified = false;

            std::string token;
            while (ss >> token) {
                if (token == "depth") { ss >> depth; depth_specified = true; } 
                else if (token == "movetime") { ss >> movetime; }
                else if (token == "wtime") { ss >> wtime; }
                else if (token == "btime") { ss >> btime; }
                else if (token == "winc") { ss >> winc; }
                else if (token == "binc") { ss >> binc; }
                else if (token == "movestogo") { ss >> movestogo; }
                else if (token == "infinite") { infinite = true; }
            }

            int time_allocated = movetime;
            if (time_allocated == 0 && (wtime > 0 || btime > 0)) {
                time_allocated = calculate_time(wtime, btime, winc, binc, movestogo, board.sideToMove());
            }

            if (infinite && !depth_specified) {
                depth = 64;
            }

            stop_search = false;

            search_thread = std::thread([board, depth, time_allocated, infinite]() {
                chess::Move best_move = iterative_deepening(board, depth, time_allocated, infinite);

                chess::Move final_move = (best_move != chess::Move::NO_MOVE) ? best_move : last_best_move;

                std::stringstream ss_out;
                if (final_move != chess::Move::NO_MOVE) {
                    ss_out << "bestmove " << chess::uci::moveToUci(final_move) << "\n";
                } else {
                    ss_out << "bestmove 0000\n";
                }
                sync_cout(ss_out.str());
            });
        }
        else if (command == "stop") {
            stop_current_search();
        } 
        else if (command == "quit") {
            stop_current_search();
            break;
        }
    }

    return 0;
}