#include <iostream>
#include <string>
#include <sstream>
#include <vector>
#include <chrono>
#include <atomic>
#include <thread>
#include <algorithm>
#include <mutex>

#include "probe.h" // La cabecera del repositorio clonado

#include "chesslib.hpp"
#include "NegamaxQuinenseSearch.hpp"

// ============================================================================
// VARIABLES GLOBALES DE ESTADO Y SINCRONIZACIÓN
// ============================================================================
std::atomic<bool> stop_search(false);
chess::Move last_best_move = chess::Move::NO_MOVE;
std::thread search_thread;
std::mutex cout_mutex; // Previene que varios hilos corrompan std::cout

// ============================================================================
// FUNCIONES AUXILIARES
// ============================================================================

// Imprime en consola de forma segura mediante Mutex
void sync_cout(const std::string& text) {
    std::lock_guard<std::mutex> lock(cout_mutex);
    std::cout << text << std::flush;
}

// Detiene cualquier hilo de búsqueda activo y espera a que finalice limpiamente
void stop_current_search() {
    stop_search = true;
    if (search_thread.joinable()) {
        search_thread.join();
    }
}

// Gestión de tiempo para partidas de reloj
inline int calculate_time(int wtime, int btime, int winc, int binc, int movestogo, chess::Color sideToMove) {
    int time_left = (sideToMove == chess::Color::WHITE) ? wtime : btime;
    int inc       = (sideToMove == chess::Color::WHITE) ? winc  : binc;

    if (time_left <= 0) return 10;

    int moves = (movestogo > 0) ? movestogo : 30; 
    int time_allocated = (time_left / moves) + (inc * 3 / 4);

    return std::max(10, time_allocated - 50); // Margen de seguridad de 50ms
}

// Extrae la variante principal (PV) recorriendo la TT
inline std::string get_pv_string(chess::Board board, int depth) {
    std::string pv_str = "";
    chess::Move tt_move = chess::Move::NO_MOVE;
    int dummy_score = 0;

    for (int i = 0; i < depth; ++i) {
        int dummy_eval = 0;
        if (!probe_tt(board.zobrist(), depth - i, -100000, 100000, dummy_score, dummy_eval, tt_move, 0)) {
            break;
        }
        if (tt_move == chess::Move::NO_MOVE) break;

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

    // 1. INICIALIZAR EL CONTROL DE TIEMPO GLOBAL DE TU HEADER
    init_search_time(max_time_ms, infinite);

    int alpha = -100000;
    int beta  =  100000;
    int delta =  50;
    int score =  0;

    for (int current_depth = 1; current_depth <= max_prof; ++current_depth) {
        if (stop_search) break;

        if (current_depth >= 5) {
            alpha = std::max(-100000, score - delta);
            beta  = std::min( 100000, score + delta);
        } else {
            alpha = -100000;
            beta  =  100000;
        }

        bool search_failed = false;

        while (true) {
            chess::Board search_board = board; 
            
            // 2. ATRAPAR LA EXCEPCIÓN LIMPIAMENTE AQUÍ
            try {
                score = negamax(search_board, current_depth, alpha, beta, 0);
            } 
            catch (const SearchAbortedException&) {
                // Si la búsqueda se aborta, interrumpimos la profundidad actual de inmediato
                search_failed = true;
                break; 
            }

            if (stop_search) { search_failed = true; break; }

            if (score <= alpha) {
                alpha = std::max(-100000, alpha - delta);
                delta += delta / 2;
            }
            else if (score >= beta) {
                beta = std::min(100000, beta + delta);
                delta += delta / 2;
            }
            else {
                break;
            }
        }

        // Si la búsqueda fue abortada por tiempo o comando stop, salimos del bucle principal
        if (search_failed || stop_search) break;

        delta = 50;

        // Recuperar mejor jugada de la TT de la última profundidad completa
        chess::Move tt_move = chess::Move::NO_MOVE;
        int dummy_score = 0;
        int dummy_eval = 0;
        probe_tt(board.zobrist(), current_depth, -100000, 100000, dummy_score, dummy_eval, tt_move, 0);

        if (tt_move != chess::Move::NO_MOVE) {
            best_move_overall = tt_move;
            last_best_move = tt_move;
        }

        std::string pv_line = get_pv_string(board, current_depth);
        auto current_time = std::chrono::high_resolution_clock::now();  
        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(current_time - search_start_time).count();

        // Protección contra división por cero
        uint64_t nps = (elapsed_ms > 0) ? (static_cast<uint64_t>(nodos_totales_busqueda) * 1000 / elapsed_ms) : 0;

        std::stringstream ss_info;
        ss_info << "info depth " << current_depth
                << " score " << format_score_uci(score)
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
    // Inicializa el sistema con la BigNet y SmallNet del repo clonado
    Stockfish::Probe::init("nn-71d6d32cb962.nnue", "nn-b1a57edbea57.nnue");

    std::cout << "Stormfish FireBlack 2026" << std::endl;

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
        else if (command == "eval") {
            board = chess::Board();
            // 1. Obtener la evaluación actual de la posición
            int score = evaluate(board); 

            // 2. Imprimir el puntaje usando la función helper
            std::cout << format_score_uci(score) << "\n" << std::endl;
        }
        else if (command == "ucinewgame") {
            stop_current_search();
            clear_tt();
            board = chess::Board();
            sync_cout("readyok\n");
        } 
        else if (command == "position") {
            stop_current_search();

            std::string pos_type;
            ss >> pos_type;

            if (pos_type == "startpos") {
                board = chess::Board();
                std::string moves_token;
                if (ss >> moves_token && moves_token == "moves") {
                    std::string move_str;
                    while (ss >> move_str) {
                        chess::Move move = chess::uci::uciToMove(board, move_str);
                        if (move != chess::Move::NO_MOVE) {
                            board.makeMove(move);
                        }
                    }
                }
            } 
            else if (pos_type == "fen") {
                std::string fen = "";
                std::string token;
                
                // Lee el FEN dinámicamente hasta encontrar la palabra "moves" o el fin del stream
                while (ss >> token && token != "moves") {
                    if (!fen.empty()) fen += " ";
                    fen += token;
                }
                
                board.setFen(fen);

                if (token == "moves") {
                    std::string move_str;
                    while (ss >> move_str) {
                        chess::Move move = chess::uci::uciToMove(board, move_str);
                        if (move != chess::Move::NO_MOVE) {
                            board.makeMove(move);
                        }
                    }
                }
            }
        } 
        else if (command == "move" || command == "usermove") {
            stop_current_search();
            std::string move_str;
            if (ss >> move_str) {
                chess::Move move = chess::uci::uciToMove(board, move_str);
                if (move != chess::Move::NO_MOVE) {
                    board.makeMove(move);
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

            // EL HILO ES EL ÚNICO RESPONSABLE DE ENVIAR "bestmove"
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
            // Únicamente detenemos el hilo. El hilo se encargará de responder con el bestmove
            stop_current_search();
        } 
        else if (command == "quit") {
            stop_current_search();
            break;
        }
    }

    return 0;
}