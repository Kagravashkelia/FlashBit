#pragma once

#include "probe.h"
#include "chesslib.hpp"

inline int evaluate(const chess::Board& board) {
    // Usamos el nombre exacto del header: Stockfish::Probe::eval
    int raw_score = Stockfish::Probe::eval(board.getFen().c_str()) >> 2; 

    return raw_score;
}