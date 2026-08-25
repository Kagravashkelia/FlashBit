#include "UciEngine.hpp"

#include <algorithm>
#include <iostream>
#include <sstream>
#include <string>

#include "evaluate.hpp"
#include "NegamaxQuinenseSearch.hpp"

namespace {

int calculate_time(int wtime, int btime, int winc, int binc,
                   int movestogo, chess::Color side_to_move) {
    int time_left = side_to_move == chess::Color::WHITE ? wtime : btime;
    int increment = side_to_move == chess::Color::WHITE ? winc : binc;
    if (time_left <= 0) return 10;

    int moves = movestogo > 0 ? movestogo : 30;
    int time_allocated = (time_left / moves) + (increment * 3 / 4);
    return std::max(10, time_allocated - 50);
}

}

void UciEngine::handle_position(std::stringstream& command) {
    search.stop();

    std::string position_type;
    command >> position_type;
    if (position_type == "startpos") {
        board = chess::Board();
        std::string moves_token;
        if (command >> moves_token && moves_token == "moves") {
            std::string move_text;
            while (command >> move_text) {
                chess::Move move = chess::uci::uciToMove(board, move_text);
                if (move != chess::Move::NO_MOVE) board.makeMove(move);
            }
        }
        return;
    }

    if (position_type == "fen") {
        std::string fen;
        std::string token;
        while (command >> token && token != "moves") {
            if (!fen.empty()) fen += " ";
            fen += token;
        }
        board.setFen(fen);

        if (token == "moves") {
            std::string move_text;
            while (command >> move_text) {
                chess::Move move = chess::uci::uciToMove(board, move_text);
                if (move != chess::Move::NO_MOVE) board.makeMove(move);
            }
        }
    }
}

void UciEngine::handle_go(std::stringstream& command) {
    int depth = 64;
    int movetime = 0;
    int wtime = 0;
    int btime = 0;
    int winc = 0;
    int binc = 0;
    int movestogo = 0;
    bool infinite = false;
    bool depth_specified = false;

    std::string token;
    while (command >> token) {
        if (token == "depth") {
            command >> depth;
            depth_specified = true;
        } else if (token == "movetime") {
            command >> movetime;
        } else if (token == "wtime") {
            command >> wtime;
        } else if (token == "btime") {
            command >> btime;
        } else if (token == "winc") {
            command >> winc;
        } else if (token == "binc") {
            command >> binc;
        } else if (token == "movestogo") {
            command >> movestogo;
        } else if (token == "infinite") {
            infinite = true;
        }
    }

    if (infinite && !depth_specified) depth = 64;
    int time_allocated = movetime;
    if (time_allocated == 0 && (wtime > 0 || btime > 0)) {
        time_allocated = calculate_time(wtime, btime, winc, binc,
                                        movestogo, board.sideToMove());
    }

    search.start(board, depth, time_allocated, infinite, num_threads);
}

void UciEngine::run() {
    std::cout << "FlashBit 1.2 NNUE" << std::endl;
    std::ios_base::sync_with_stdio(false);
    std::cin.tie(nullptr);

    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;

        std::stringstream command(line);
        std::string name;
        command >> name;

        if (name == "uci") {
            sync_cout("id name FlashBit 1.2 NNUE\nid author Anthony\noption name Hash type spin default 16 min 1 max 1024\noption name Threads type spin default 1 min 1 max 128\nuciok\n");
        } else if (name == "isready") {
            sync_cout("readyok\n");
        } else if (name == "eval") {
            board = chess::Board();
            sync_cout(format_score_uci(evaluate(board)) + "\n\n");
        } else if (name == "ucinewgame") {
            search.stop();
            clear_tt();
            board = chess::Board();
            sync_cout("readyok\n");
        } else if (name == "position") {
            handle_position(command);
        } else if (name == "move" || name == "usermove") {
            search.stop();
            std::string move_text;
            if (command >> move_text) {} else if (name == "setoption") {
            std::string token, opt_name, opt_val;
            command >> token; // Lee "name"
            if (token == "name") {
                command >> opt_name;
                command >> token; // Lee "value"
                if (token == "value") {
                    command >> opt_val;
                    if (opt_name == "Threads" || opt_name == "threads") {
                        num_threads = std::clamp(std::stoi(opt_val), 1, 128);
                    }
                }
            }
        } else if (name == "ucinewgame") {
                chess::Move move = chess::uci::uciToMove(board, move_text);
                if (move != chess::Move::NO_MOVE) board.makeMove(move);
            }
        } else if (name == "go") {
            handle_go(command);
        } else if (name == "stop") {
            search.stop();
        } else if (name == "quit") {
            search.stop();
            break;
        }
    }
}
