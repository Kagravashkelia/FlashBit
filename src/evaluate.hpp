#pragma once
#include "pst.hpp"
#include "chesslib.hpp"
#include <algorithm>
#include <array>

// ============================================================================
// HELPER ULTRA RÁPIDO PARA CONTEO DE BITS EN C++ / GCC
// ============================================================================
inline int count_bits(uint64_t b) {
    return __builtin_popcountll(b);
}

// ============================================================================
// 1. CONSTANTES Y PESOS DE FASE (Tapered Evaluation)
// ============================================================================
constexpr int PAWN_PHASE   = 0;
constexpr int KNIGHT_PHASE = 1;
constexpr int BISHOP_PHASE = 1;
constexpr int ROOK_PHASE   = 2;
constexpr int QUEEN_PHASE  = 4;
constexpr int TOTAL_PHASE  = 16 * PAWN_PHASE + 4 * KNIGHT_PHASE + 4 * BISHOP_PHASE + 4 * ROOK_PHASE + 2 * QUEEN_PHASE; // 24

// Bonos por Peones Pasados por Fila (1 a 8)
constexpr int PASSED_PAWN_BONUS_EG[8] = { 0, 5, 15, 30, 55, 90, 140, 0 };
constexpr int PASSED_PAWN_BONUS_MG[8] = { 0, 0,  5, 10, 20, 35,  60, 0 };

// Pesos para Ataque al Rey según tipo de pieza atacante
constexpr int KING_ATTACK_WEIGHTS[6] = { 0, 2, 2, 3, 5, 0 }; // C, A, T, D

// Máscaras de columnas
constexpr uint64_t FILE_MASKS[8] = {
    0x0101010101010101ULL, 0x0202020202020202ULL,
    0x0404040404040404ULL, 0x0808080808080808ULL,
    0x1010101010101010ULL, 0x2020202020202020ULL,
    0x4040404040404040ULL, 0x8080808080808080ULL
};

constexpr uint64_t NEIGHBOR_FILES[8] = {
    0x0202020202020202ULL, 0x0505050505050505ULL,
    0x0A0A0A0A0A0A0A0AULL, 0x1414141414141414ULL,
    0x2828282828282828ULL, 0x5050505050505050ULL,
    0xA0A0A0A0A0A0A0A0ULL, 0x4040404040404040ULL
};

// Generación rápida de ataques de peón
inline uint64_t get_w_pawn_attacks(uint64_t wp) {
    return ((wp & 0xFEFEFEFEFEFEFEFEULL) << 7) | ((wp & 0x7F7F7F7F7F7F7F7FULL) << 9);
}
inline uint64_t get_b_pawn_attacks(uint64_t bp) {
    return ((bp & 0x7F7F7F7F7F7F7F7FULL) >> 7) | ((bp & 0xFEFEFEFEFEFEFEFEULL) >> 9);
}

// ============================================================================
// 2. FUNCIÓN PRINCIPAL DE EVALUACIÓN
// ============================================================================
inline int evaluate(const chess::Board& board) {
    int mg_score = 0;
    int eg_score = 0;
    int game_phase = 0;

    // Bitboards generales por color y tipo
    const chess::Bitboard white_pieces = board.us(chess::Color::WHITE);
    const chess::Bitboard black_pieces = board.us(chess::Color::BLACK);
    const chess::Bitboard occupied     = board.occ();

    const chess::Bitboard w_pawns      = board.pieces(chess::PieceType::PAWN, chess::Color::WHITE);
    const chess::Bitboard b_pawns      = board.pieces(chess::PieceType::PAWN, chess::Color::BLACK);
    const chess::Bitboard all_pawns    = w_pawns | b_pawns;

    const chess::Bitboard piece_bitboards[6] = {
        board.pieces(chess::PieceType::PAWN),
        board.pieces(chess::PieceType::KNIGHT),
        board.pieces(chess::PieceType::BISHOP),
        board.pieces(chess::PieceType::ROOK),
        board.pieces(chess::PieceType::QUEEN),
        board.pieces(chess::PieceType::KING)
    };

    const int phase_weights[6] = {
        PAWN_PHASE, KNIGHT_PHASE, BISHOP_PHASE, ROOK_PHASE, QUEEN_PHASE, 0
    };

    // ------------------------------------------------------------------------
    // AMENAZAS Y COBERTURA GLOBAL DE ATAQUES (Bitboards de Control)
    // ------------------------------------------------------------------------
    const uint64_t w_pawn_attacks = get_w_pawn_attacks(w_pawns.getBits());
    const uint64_t b_pawn_attacks = get_b_pawn_attacks(b_pawns.getBits());

    uint64_t w_all_attacks = w_pawn_attacks;
    uint64_t b_all_attacks = b_pawn_attacks;

    // Reyes y Anillos de Ataque (King Ring)
    const int w_king_sq = (piece_bitboards[5] & white_pieces).lsb();
    const int b_king_sq = (piece_bitboards[5] & black_pieces).lsb();

    const uint64_t w_king_ring = chess::attacks::king(w_king_sq).getBits() | (1ULL << w_king_sq);
    const uint64_t b_king_ring = chess::attacks::king(b_king_sq).getBits() | (1ULL << b_king_sq);

    int w_king_attackers_count = 0, w_king_attack_weight = 0;
    int b_king_attackers_count = 0, b_king_attack_weight = 0;

    // ------------------------------------------------------------------------
    // A. MATERIAL, PST, MOVILIDAD Y ATAQUES AL REY
    // ------------------------------------------------------------------------
    for (int p_type = 0; p_type < 6; ++p_type) {
        int mg_val  = pst::mg_value[p_type];
        int eg_val  = pst::eg_value[p_type];
        int p_phase = phase_weights[p_type];

        // --- Piezas Blancas ---
        chess::Bitboard w_bb = piece_bitboards[p_type] & white_pieces;
        while (w_bb) {
            int sq = w_bb.pop();
            mg_score += mg_val + pst::mg_table[p_type][sq];
            eg_score += eg_val + pst::eg_table[p_type][sq];
            game_phase += p_phase;

            // Movilidad y Ataques de Piezas
            if (p_type > 0 && p_type < 5) {
                uint64_t atk = 0;
                if (p_type == 1) atk = chess::attacks::knight(sq).getBits();
                else if (p_type == 2) atk = chess::attacks::bishop(sq, occupied).getBits();
                else if (p_type == 3) atk = chess::attacks::rook(sq, occupied).getBits();
                else if (p_type == 4) atk = chess::attacks::queen(sq, occupied).getBits();

                w_all_attacks |= atk;

                // Movilidad segura
                uint64_t safe_atk = atk & ~b_pawn_attacks & ~white_pieces.getBits();
                int safe_moves = count_bits(safe_atk);
                mg_score += (safe_moves - 3) * 2;
                eg_score += (safe_moves - 3) * 3;

                // Conteo de ataque al Rey Enemigo
                if (atk & b_king_ring) {
                    w_king_attackers_count++;
                    w_king_attack_weight += KING_ATTACK_WEIGHTS[p_type];
                }

                // POSTE AVANZADO DE CABALLO (Outpost)
                if (p_type == 1 && (sq / 8 >= 3 && sq / 8 <= 5)) {
                    int file = sq % 8;
                    bool protected_by_pawn = (1ULL << sq) & w_pawn_attacks;
                    uint64_t enemy_pawn_span = NEIGHBOR_FILES[file] & (~0ULL << (8 * (sq / 8)));
                    bool cannot_be_attacked = !(enemy_pawn_span & b_pawns.getBits());
                    if (protected_by_pawn && cannot_be_attacked) {
                        mg_score += 25; eg_score += 20;
                    }
                }
            }
        }

        // --- Piezas Negras ---
        chess::Bitboard b_bb = piece_bitboards[p_type] & black_pieces;
        while (b_bb) {
            int sq = b_bb.pop();
            int pst_sq = sq ^ 56;
            mg_score -= mg_val + pst::mg_table[p_type][pst_sq];
            eg_score -= eg_val + pst::eg_table[p_type][pst_sq];
            game_phase += p_phase;

            if (p_type > 0 && p_type < 5) {
                uint64_t atk = 0;
                if (p_type == 1) atk = chess::attacks::knight(sq).getBits();
                else if (p_type == 2) atk = chess::attacks::bishop(sq, occupied).getBits();
                else if (p_type == 3) atk = chess::attacks::rook(sq, occupied).getBits();
                else if (p_type == 4) atk = chess::attacks::queen(sq, occupied).getBits();

                b_all_attacks |= atk;

                uint64_t safe_atk = atk & ~w_pawn_attacks & ~black_pieces.getBits();
                int safe_moves = count_bits(safe_atk);
                mg_score -= (safe_moves - 3) * 2;
                eg_score -= (safe_moves - 3) * 3;

                if (atk & w_king_ring) {
                    b_king_attackers_count++;
                    b_king_attack_weight += KING_ATTACK_WEIGHTS[p_type];
                }

                // POSTE AVANZADO DE CABALLO (Outpost)
                if (p_type == 1 && (sq / 8 >= 2 && sq / 8 <= 4)) {
                    int file = sq % 8;
                    bool protected_by_pawn = (1ULL << sq) & b_pawn_attacks;
                    uint64_t enemy_pawn_span = NEIGHBOR_FILES[file] & ((1ULL << (8 * (sq / 8 + 1))) - 1);
                    bool cannot_be_attacked = !(enemy_pawn_span & w_pawns.getBits());
                    if (protected_by_pawn && cannot_be_attacked) {
                        mg_score -= 25; eg_score -= 20;
                    }
                }
            }
        }
    }

    // ------------------------------------------------------------------------
    // B. TÁCTICA: PIEZAS AMENAZADAS Y COLGADAS (Hanging / Undefended Pieces)
    // ------------------------------------------------------------------------
    // 1. Piezas atacadas directamente por peones
    uint64_t b_under_pawn = (black_pieces.getBits() & ~b_pawns.getBits()) & w_pawn_attacks;
    mg_score += count_bits(b_under_pawn) * 35;
    eg_score += count_bits(b_under_pawn) * 25;

    uint64_t w_under_pawn = (white_pieces.getBits() & ~w_pawns.getBits()) & b_pawn_attacks;
    mg_score -= count_bits(w_under_pawn) * 35;
    eg_score -= count_bits(w_under_pawn) * 25;

    // 2. Piezas colgadas (Atacadas y SIN defensa propia)
    uint64_t b_hanging = (black_pieces.getBits() & ~b_pawns.getBits()) & w_all_attacks & ~b_all_attacks;
    mg_score += count_bits(b_hanging) * 30;
    eg_score += count_bits(b_hanging) * 40;

    uint64_t w_hanging = (white_pieces.getBits() & ~w_pawns.getBits()) & b_all_attacks & ~w_all_attacks;
    mg_score -= count_bits(w_hanging) * 30;
    eg_score -= count_bits(w_hanging) * 40;

    // ------------------------------------------------------------------------
    // C. SEGURIDAD DEL REY ESCALADA (King Safety)
    // ------------------------------------------------------------------------
    if (w_king_attackers_count >= 2) {
        int attack_danger = w_king_attackers_count * w_king_attack_weight;
        mg_score += (attack_danger * attack_danger) / 3; // Escalado no lineal
    }
    if (b_king_attackers_count >= 2) {
        int attack_danger = b_king_attackers_count * b_king_attack_weight;
        mg_score -= (attack_danger * attack_danger) / 3;
    }

    // Escudos de peones
    if (w_king_sq == 6 || w_king_sq == 7) {
        uint64_t shield = (1ULL << 13) | (1ULL << 14) | (1ULL << 15);
        mg_score += (w_pawns.getBits() & shield) ? 15 : -25;
    }
    if (b_king_sq == 62 || b_king_sq == 63) {
        uint64_t shield = (1ULL << 53) | (1ULL << 54) | (1ULL << 55);
        mg_score -= (b_pawns.getBits() & shield) ? 15 : -25;
    }

    // ------------------------------------------------------------------------
    // D. PEONES: ESTRUCTURA, PASADOS Y CADENAS
    // ------------------------------------------------------------------------
    // Peones Blancos
    chess::Bitboard w_p = w_pawns;
    while (w_p) {
        int sq = w_p.pop();
        int file = sq % 8;
        int rank = sq / 8;

        // Peones protegidos por peones (Estructura sólida)
        if ((1ULL << sq) & w_pawn_attacks) {
            mg_score += 10; eg_score += 12;
        }

        // Peones Pasados
        uint64_t front_mask = (FILE_MASKS[file] | NEIGHBOR_FILES[file]) & (~0ULL << (8 * (rank + 1)));
        if (!(front_mask & b_pawns.getBits())) {
            mg_score += PASSED_PAWN_BONUS_MG[rank];
            eg_score += PASSED_PAWN_BONUS_EG[rank];
        }

        // Peón Aislado
        if (!(NEIGHBOR_FILES[file] & w_pawns.getBits())) {
            mg_score -= 12; eg_score -= 16;
        }
    }

    // Peones Negros
    chess::Bitboard b_p = b_pawns;
    while (b_p) {
        int sq = b_p.pop();
        int file = sq % 8;
        int rank = sq / 8;

        if ((1ULL << sq) & b_pawn_attacks) {
            mg_score -= 10; eg_score -= 12;
        }

        uint64_t front_mask = (FILE_MASKS[file] | NEIGHBOR_FILES[file]) & ((1ULL << (8 * rank)) - 1);
        if (!(front_mask & w_pawns.getBits())) {
            mg_score -= PASSED_PAWN_BONUS_MG[7 - rank];
            eg_score -= PASSED_PAWN_BONUS_EG[7 - rank];
        }

        if (!(NEIGHBOR_FILES[file] & b_pawns.getBits())) {
            mg_score += 12; eg_score += 16;
        }
    }

    // ------------------------------------------------------------------------
    // E. CONCEPTOS ESPECÍFICOS DE PIEZAS
    // ------------------------------------------------------------------------
    // Pareja de Alfiles
    if (count_bits((piece_bitboards[2] & white_pieces).getBits()) >= 2) { mg_score += 20; eg_score += 40; }
    if (count_bits((piece_bitboards[2] & black_pieces).getBits()) >= 2) { mg_score -= 20; eg_score -= 40; }

    // Torres en Columnas Abiertas y 7ª Fila
    chess::Bitboard w_rooks = piece_bitboards[3] & white_pieces;
    int w_rooks_on_7th = 0;
    while (w_rooks) {
        int sq = w_rooks.pop();
        int file = sq % 8;
        uint64_t file_bb = FILE_MASKS[file];

        if (!(file_bb & all_pawns.getBits())) { mg_score += 24; eg_score += 24; }
        else if (!(file_bb & w_pawns.getBits())) { mg_score += 12; eg_score += 12; }
        if (sq / 8 == 6) { mg_score += 20; eg_score += 30; w_rooks_on_7th++; }
    }
    if (w_rooks_on_7th >= 2) { mg_score += 30; eg_score += 50; } // Batería devastadora en 7ma

    chess::Bitboard b_rooks = piece_bitboards[3] & black_pieces;
    int b_rooks_on_7th = 0;
    while (b_rooks) {
        int sq = b_rooks.pop();
        int file = sq % 8;
        uint64_t file_bb = FILE_MASKS[file];

        if (!(file_bb & all_pawns.getBits())) { mg_score -= 24; eg_score -= 24; }
        else if (!(file_bb & b_pawns.getBits())) { mg_score -= 12; eg_score -= 12; }
        if (sq / 8 == 1) { mg_score -= 20; eg_score -= 30; b_rooks_on_7th++; }
    }
    if (b_rooks_on_7th >= 2) { mg_score -= 30; eg_score -= 50; }

    // ------------------------------------------------------------------------
    // F. INICIATIVA (Tempo Bonus)
    // ------------------------------------------------------------------------
    int tempo = (board.sideToMove() == chess::Color::WHITE) ? 12 : -12;
    mg_score += tempo;

    // ------------------------------------------------------------------------
    // G. INTERPOLACIÓN TAPERED Y PERSPECTIVA
    // ------------------------------------------------------------------------
    int mg_phase = std::min(game_phase, TOTAL_PHASE);
    int eg_phase = TOTAL_PHASE - mg_phase;

    int final_score = (mg_score * mg_phase + eg_score * eg_phase) / TOTAL_PHASE;

    return (board.sideToMove() == chess::Color::WHITE) ? final_score : -final_score;
}