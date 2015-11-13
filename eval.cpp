//===========================================================//
//
// Maverick Chess Engine
// Copyright 2013-2015 Steve Maughan
//
//===========================================================//

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <assert.h>
#include <math.h>
#include <stdlib.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "defs.h"
#include "eval.h"
#include "data.h"
#include "procs.h"
#include "bittwiddle.h"

#ifdef USE_EVAL_HASH

//typedef struct eval_hash {
	//struct t_chess_eval eval;
    //t_hash key;
//} eval_hash;

//static const unsigned int EVAL_HASH_MASK = 0x1FFFF;
//static eval_hash eht[EVAL_HASH_MASK + 1] = { 0 };

#endif

t_chess_value evaluate(struct t_board *board, struct t_chess_eval *eval) {

    t_chess_value score;

#ifdef USE_EVAL_HASH    
	
	// Check for previous evaluation and return score if signature is identical
    //t_hash hash = board->hash;
	//assert(board->hash == calc_board_hash(board));

//	eval_hash *e = &eht[hash & EVAL_HASH_MASK];
//	if (e->key == hash)
//	{
//		*eval = e->eval;
//		return eval->static_score;
//	}

#endif

    //-- Known ending?
    int index = board->material_hash & material_hash_mask;
    if (material_hash[index].key == board->material_hash) {
        assert(material_hash[index].eval_endgame != NULL);
        material_hash[index].eval_endgame(board, eval);
        score = eval->static_score;
    }
    else
        score = calc_evaluation(board, eval);

#ifdef USE_EVAL_HASH

    // Store result in evaluation hash table (always overwrite)
//    e->key = hash;
//	e->eval = *eval;

#endif

    return score;
}

t_chess_value calc_evaluation(struct t_board *board, struct t_chess_eval *eval) {

    //-- Normal Position, so initialize the values
    init_eval(eval);

    //-- Is it a king & pawn endgame
    if (board->pawn_hash == board->hash) {
        eval->static_score = calc_king_pawn_endgame(board, eval);
        return eval->static_score;
    }

    t_chess_value score;

    eval->king_zone[WHITE] = king_zone[board->king_square[WHITE]];
    eval->king_zone[BLACK] = king_zone[board->king_square[BLACK]];

    //-- Are we in the middle game, endgame or somewhere in between
    calc_game_phase(board, eval);

    //-- What are the strengths / weaknesses of the pawn structure
    calc_pawn_value(board, eval);

    //-- How well placed are the pieces
    calc_piece_value(board, eval);

    //-- How dangerous are the passed pawns
    calc_passed_pawns(board, eval);

    //-- Is the king in danger
    calc_king_safety(board, eval);

    //-- Scale the score and adjust for color to move
    score = (1 - 2 * board->to_move) * (((eval->middlegame * eval->game_phase) + (eval->endgame * (256 - eval->game_phase))) / 256);

    //-- Store the score
    eval->static_score = score;
    return score;

}

inline void calc_game_phase(struct t_board *board, struct t_chess_eval *eval) {
    //-- Phase of the game
    eval->game_phase = 2 * popcount(board->piecelist[WHITEPAWN] ^ board->piecelist[BLACKPAWN]);
    eval->game_phase += 44 * popcount(board->piecelist[WHITEQUEEN] ^ board->piecelist[BLACKQUEEN]);
    eval->game_phase += 16 * popcount(board->piecelist[WHITEROOK] ^ board->piecelist[BLACKROOK]);
    eval->game_phase += 12 * popcount(board->piecelist[WHITEBISHOP] ^ board->piecelist[BLACKBISHOP]);
    eval->game_phase += 6 * popcount(board->piecelist[WHITEKNIGHT] ^ board->piecelist[BLACKKNIGHT]);
	
	eval->game_phase = min(eval->game_phase, 256);

	assert(eval->game_phase >= 0);
	assert(eval->game_phase <= 256);
}
	

inline void calc_pawn_value(struct t_board *board, struct t_chess_eval *eval) {
    eval->pawn_evaluation = lookup_pawn_hash(board, eval);
    eval->middlegame += eval->pawn_evaluation->middlegame;
    eval->endgame += eval->pawn_evaluation->endgame;
}

static inline int square_distance(int s1, int s2)
{
    int r = abs(RANK(s1) - RANK(s2));
    int c = abs(COLUMN(s1) - COLUMN(s2));
    if (r > c)
        return r;
    else
        return c;
};

inline void calc_piece_value(struct t_board *board, struct t_chess_eval *eval) {

    t_chess_color color, opponent;
    t_chess_square square;
    t_chess_piece piece;
    t_chess_piece piece_type;
    t_bitboard b;
    t_bitboard attack_squares;
    t_bitboard moves;
    int move_count;
    struct t_pawn_hash_record *pawn_record = eval->pawn_evaluation;

    for (color = WHITE; color <= BLACK; color++) {

        t_chess_value middlegame = 0;
        t_chess_value endgame = 0;

        opponent = OPPONENT(color);

        //=========================================================
        //-- Rooks first
        //=========================================================
        piece = PIECEINDEX(color, ROOK);
        eval->attacklist[piece] = 0;
        b = board->piecelist[piece];

        //-- Remove Rook and Queens (so we can easily evaluate rams)
        t_bitboard _all_pieces = board->all_pieces ^ board->pieces[color][QUEEN] ^ b;
        t_bitboard _not_occupied = ~(board->occupied[color] & _all_pieces);

        //-- Rooks on the 7th
        if ((b & rank_mask[color][6]) && (board->pieces[opponent][KING] & rank_mask[color][7])) {
            middlegame += MG_ROOK_ON_7TH;
            endgame += MG_ROOK_ON_7TH;
        }

        //-- Rooks on Open file
        if (b & pawn_record->open_file) {
			middlegame += popcount(b & pawn_record->open_file) * pawn_record->pawn_count[color] * MG_ROOK_ON_OPEN_FILE;
        }

        //-- Rooks on Semi-Open file
        if (b & pawn_record->semi_open_file[color]) {
			middlegame += popcount(b & pawn_record->semi_open_file[color]) * pawn_record->pawn_count[color] * MG_ROOK_ON_SEMI_OPEN_FILE;
        }

        //-- Loop around for all pieces
        while (b) {

            //-- Find the square it resides
            square = bitscan_reset(&b);

            //-- Generate moves
            moves = rook_magic_moves[square][((rook_magic[square].mask & _all_pieces) * rook_magic[square].magic) >> 52];
            eval->attacks[color][ROOK] |= moves;
            moves &= _not_occupied;

            //-- Mobility (along ranks)
            move_count = popcount(moves & square_rank_mask[square]);
            middlegame += horizontal_rook_mobility[MIDDLEGAME][move_count];
            endgame += horizontal_rook_mobility[ENDGAME][move_count];

            //-- Mobility (along files)
            move_count = popcount(moves & square_column_mask[square]);
            middlegame += vertical_rook_mobility[MIDDLEGAME][move_count];
            endgame += vertical_rook_mobility[ENDGAME][move_count];

            //-- King safety
            if (attack_squares = (moves & eval->king_zone[opponent])) {
                eval->king_attack_count[opponent]++;
                eval->king_attack_pressure[opponent] += popcount(attack_squares) * 40;
            }
			assert(eval->king_zone[opponent] != 0);

            // piece-square tables
            middlegame += piece_square_table[piece][MIDDLEGAME][square];
            endgame += piece_square_table[piece][ENDGAME][square];

        }

        //=========================================================
        //-- Queens
        //=========================================================
        piece = PIECEINDEX(color, QUEEN);
        eval->attacklist[piece] = 0;
        b = board->piecelist[piece];

        _all_pieces ^= board->pieces[color][BISHOP];
        _not_occupied = ~(board->occupied[color] & _all_pieces);

        while (b) {

            //-- Find the square it resides
            square = bitscan_reset(&b);

            //-- Rook-like Moves
            t_bitboard rook_moves = rook_magic_moves[square][((rook_magic[square].mask & _all_pieces) * rook_magic[square].magic) >> 52];
            eval->attacklist[piece] |= rook_moves;
			rook_moves &= _not_occupied;

            //-- Bishop-like moves
            t_bitboard bishop_moves = bishop_magic_moves[square][((bishop_magic[square].mask & _all_pieces) * bishop_magic[square].magic) >> 55];
            eval->attacklist[piece] |= bishop_moves;
			bishop_moves &= _not_occupied;

            //-- Mobility
            move_count = popcount((rook_moves & square_column_mask[square]) | bishop_moves);
			middlegame += move_count;

            //-- King safety
            if (attack_squares = ((rook_moves | bishop_moves) & eval->king_zone[opponent])) {
                eval->king_attack_count[opponent]++;
                eval->king_attack_pressure[opponent] += 80 * popcount(attack_squares);
            }

            //-- piece-square tables
            middlegame += piece_square_table[piece][MIDDLEGAME][square];
            endgame += piece_square_table[piece][ENDGAME][square];
        }

        //-- Interaction of double pawns & major pieces
        if (pawn_record->double_pawns[color]) {

            int double_pawn_count = popcount(pawn_record->double_pawns[color]);
            int major_piece_count = popcount(board->pieces[color][ROOK] | board->pieces[color][QUEEN]);

            switch (major_piece_count) {
            case 0:
                break;
            case 1:
                middlegame += (double_pawn_count * 12) - pawn_record->semi_open_double_pawns[color] * 30;
                endgame += (double_pawn_count * 12) - pawn_record->semi_open_double_pawns[color] * 25;
                break;
            case 2:
                middlegame += (double_pawn_count * 24) - pawn_record->semi_open_double_pawns[color] * 35;
                endgame += (double_pawn_count * 24) - pawn_record->semi_open_double_pawns[color] * 30;
                break;
            case 3:
                middlegame += (double_pawn_count * 30) - pawn_record->semi_open_double_pawns[color] * 40;
                endgame += (double_pawn_count * 30) - pawn_record->semi_open_double_pawns[color] * 35;
                break;
            case 4:
                middlegame += (double_pawn_count * 30) - pawn_record->semi_open_double_pawns[color] * 40;
                endgame += (double_pawn_count * 30) - pawn_record->semi_open_double_pawns[color] * 35;
                break;
            case 5:
                middlegame += (double_pawn_count * 30) - pawn_record->semi_open_double_pawns[color] * 40;
                endgame += (double_pawn_count * 30) - pawn_record->semi_open_double_pawns[color] * 35;
                break;
            }
        }

        //=========================================================
        //-- Bishops
        //=========================================================
        piece = PIECEINDEX(color, BISHOP);
        eval->attacklist[piece] = 0;
        b = board->piecelist[piece];

        //-- Bishop pair bonus
        if (b & (b - 1)) {
            middlegame += MG_BISHOP_PAIR;
            endgame += EG_BISHOP_PAIR;
        }

        //-- Remove Own Pieces (leave pawns)
		_all_pieces = board->all_pieces ^ board->pieces[color][KNIGHT] ^ board->pieces[color][QUEEN];
		_not_occupied = ~(board->occupied[color] & _all_pieces);

        while (b) {
            //-- Find the square it resides
            square = bitscan_reset(&b);

            //-- Generate moves
            moves = bishop_magic_moves[square][((bishop_magic[square].mask & _all_pieces) * bishop_magic[square].magic) >> 55];
            eval->attacklist[piece] |= moves;

			moves &= _not_occupied;

            //-- Mobility
            move_count = popcount(moves);
            middlegame += bishop_mobility[MIDDLEGAME][move_count];
            endgame += bishop_mobility[ENDGAME][move_count];

            //-- Trapped
            //middlegame -= trapped_bishop[MIDDLEGAME][move_count];
            //endgame -= trapped_bishop[ENDGAME][move_count];

            //-- King safety
            if (attack_squares = (moves & eval->king_zone[opponent])) {
                eval->king_attack_count[opponent]++;
                eval->king_attack_pressure[opponent] += 20 * popcount(attack_squares);
            }

            // piece-square tables
            middlegame += piece_square_table[piece][MIDDLEGAME][square];
            endgame += piece_square_table[piece][ENDGAME][square];
        }

        //=========================================================
        //-- Knights
        //=========================================================
        piece = PIECEINDEX(color, KNIGHT);
        eval->attacklist[piece] = 0;
        b = board->piecelist[piece];

        _not_occupied = ~board->occupied[color] & ~eval->attacks[opponent][PAWN];

        //-- Outposts
        t_bitboard knight_outpost = b & pawn_record->potential_outpost[color];
        while (knight_outpost) {

            square = bitscan_reset(&knight_outpost);
            t_chess_color square_color = SQUARECOLOR(square);

            //-- Can it be taken by a minor piece?
            if ((board->pieces[opponent][KNIGHT] == 0) && ((board->pieces[opponent][BISHOP] & color_square_mask[square_color]) == 0)) {
                middlegame += 25 - square_distance(square, board->king_square[opponent]);
                endgame += 10;
            }
            else {
                middlegame += 15 - square_distance(square, board->king_square[opponent]);
                endgame += 8;
            }
        }

        while (b) {
            //-- Find the square it resides
            square = bitscan_reset(&b);

            //-- Opponents King Tropism
            middlegame -= square_distance(square, board->king_square[opponent]) * 2;

            //-- Generate moves
            moves = knight_mask[square];
            eval->attacklist[piece] |= moves;

            //-- Connected to another knight
            if (moves & board->piecelist[piece]) {
                middlegame += MG_CONNECTED_KNIGHTS;
                endgame += EG_CONNECTED_KNIGHTS;
            }

            //-- King safety
            if (attack_squares = (moves & eval->king_zone[opponent])) {
                eval->king_attack_count[opponent]++;
                eval->king_attack_pressure[opponent] += 20 * popcount(attack_squares);
            }

            //-- Mobility (not including any squares attacked by enemy pawns)
            moves &= _not_occupied;
            move_count = popcount(moves);
            middlegame += knight_mobility[MIDDLEGAME][move_count];
            endgame += knight_mobility[ENDGAME][move_count];

            // piece-square tables
            middlegame += piece_square_table[piece][MIDDLEGAME][square];
            endgame += piece_square_table[piece][ENDGAME][square];
        }

        //=========================================================
        //-- King Attacks
        //=========================================================
        piece = PIECEINDEX(color, KING);
        eval->attacklist[piece] = king_mask[board->king_square[color]];

		//=========================================================
		//-- Blocked Central Pawns
		//=========================================================
		if (b = (central_kq_pawns[color] & board->pieces[color][PAWN])){
			b = ((b << 8) >> (color * 16)) & board->all_pieces;
			middlegame += -10 * popcount(b);
		}

        //-- Add to board scores
        eval->middlegame += middlegame * (1 - color * 2);
        eval->endgame += endgame * (1 - color * 2);

        //-- Create combined attacks
        eval->attacks[color][BLANK] = eval->attacks[color][PAWN] | eval->attacks[color][ROOK] | eval->attacks[color][BISHOP] | eval->attacks[color][KNIGHT] | eval->attacks[color][QUEEN] | eval->attacks[color][KING];

    }
}


inline void calc_passed_pawns(struct t_board *board, struct t_chess_eval *eval) {

    struct t_pawn_hash_record *pawn_record = eval->pawn_evaluation;

    for (t_chess_color color = WHITE; color <= BLACK; color++) {

        t_chess_color opponent = OPPONENT(color);

        t_chess_value middlegame = 0;
        t_chess_value endgame = 0;

        //-- Do we have any passed pawns
        t_bitboard b = pawn_record->passed[color];

        while (b) {

            //-- Yes!  Where are they?
            t_chess_square square = bitscan_reset(&b);
            int rank = RANK(square);
            if (color) rank = (7 - rank);

			//-- How close are the kings to the promotions square?
			t_chess_square promotion_square = PROMOTION_SQUARE(color, square);
			int distance = square_distance(promotion_square, board->king_square[opponent]) - square_distance(promotion_square, board->king_square[color]);
			BOOL king_support = (distance > (color != board->to_move));

            //-- Find normal bonus
            t_chess_value bonus = passed_pawn_bonus[color][square];

			//-- Not Blocked by opponent 			
			if ((forward_squares[color][square] & board->occupied[opponent]) == 0){

				//-- Does the opponent attack the path to promotion
				if (forward_squares[color][square] & eval->attacks[opponent][BLANK]){
					middlegame += bonus / 5;
					endgame += bonus / 4;
				}

				//-- No occupation or attacks of path to promotion
				else if (king_support){
					middlegame += bonus / 4;
					endgame += bonus / 2;
				}
				else{
					middlegame += bonus / 4;
					endgame += bonus / 3;
				}
			}

			//-- Opponent blocks path to promotion
			else{
				middlegame += bonus / 6;
				endgame += bonus / 5;
			}


            ////-- Is piece in front of passed pawn
            //if (forward_squares[color][square] & board->occupied[color]) {

            //    //-- 2015-03-09
            //    // tries += bonus / 4 and += bonus / 3 but didn't improve
            //    middlegame += bonus / 5;
            //    endgame += bonus / 5;
            //}
            //else {

            //    //-- Path to promoton not occupied!!
            //    if ((forward_squares[color][square] & board->occupied[opponent]) == 0) {

            //        //-- Is Path Attacked?
            //        if ((forward_squares[color][square] & eval->attacks[opponent][BLANK]) == 0) {

            //            //-- No! Free path!
            //            // 2013-10-10: m += b / 2; e += b / 1;
            //            // 2015-03-09: m += b / 4; e += b / 2; +14 ELO
            //            // 2015-03-09: m += b / 4; e += b / 3; +6 ELO
            //            middlegame += bonus / 4;
            //            endgame += bonus / 2;

            //        }
            //        //-- Yes it's attacked
            //        else {
            //            // 2013-10-10: m += b / 3; e += b / 2;
            //            // 2015-03-09: m += b / 6; e += b / 3; +13 ELO
            //            // 2015-03-09: m += b / 6; e += b / 4; +13 ELO
            //            // 2015-03-09: m += b / 6; e += b / 6; -8 ELO
            //            // 2015-03-09: m += b / 4; e += b / 3; -13 ELO
            //            // 2015-03-09: m += b / 4; e += 0;     +5 ELO
            //            middlegame += bonus / 6;
            //            endgame += bonus / 3;
            //        }
            //    }
            //}

            //-- Is a Rook behind the passed pawn
            if (forward_squares[opponent][square] & board->pieces[color][ROOK]) {
                middlegame += MG_ROOK_BEHIND_PASSED_PAWN * (1 - color * 2);
                endgame += EG_ROOK_BEHIND_PASSED_PAWN * (1 - color * 2);
            }
        }

        //-- Add the score to the Eval
        eval->middlegame += middlegame;
        eval->endgame += endgame;
    }
}

inline void calc_king_safety(struct t_board *board, struct t_chess_eval *eval)
{
    for (t_chess_color color = WHITE; color <= BLACK; color++)
        eval->king_attack[color] = (eval->king_attack_pressure[OPPONENT(color)] * king_safety[eval->king_attack_count[OPPONENT(color)] & 7]) / 128;

    eval->middlegame += eval->king_attack[WHITE] - eval->king_attack[BLACK];
}

void init_eval(struct t_chess_eval *eval)
{

    eval->middlegame = 0;
    eval->endgame = 0;

    for (t_chess_color color = WHITE; color <= BLACK; color++) {

        eval->attacks[color] = eval->attacklist + (8 * color);

        eval->king_attack_count[color] = 0;
        eval->king_attack_pressure[color] = 0;
        eval->king_zone[color] = 0;
    }
}

void init_eval_function() {
    t_chess_piece piece;
    t_chess_piece piece_type;
    t_chess_square square, s;
    t_chess_color color;

    piece_value[0][PAWN] = MG_PAWN_VALUE;
    piece_value[1][PAWN] = EG_PAWN_VALUE;

    piece_value[0][KNIGHT] = MG_KNIGHT_VALUE;
    piece_value[1][KNIGHT] = EG_KNIGHT_VALUE;

    piece_value[0][BISHOP] = MG_BISHOP_VALUE;
    piece_value[1][BISHOP] = EG_BISHOP_VALUE;

    piece_value[0][ROOK] = MG_ROOK_VALUE;
    piece_value[1][ROOK] = EG_ROOK_VALUE;

    piece_value[0][QUEEN] = MG_QUEEN_VALUE;
    piece_value[1][QUEEN] = EG_QUEEN_VALUE;

    piece_value[0][KING] = 0;
    piece_value[1][KING] = 0;

    //-- reset
    for (color = WHITE; color <= BLACK; color++) {
        for (s = A1; s <= H8; s++) {
            square = s;
            if (color == BLACK)
                square = FLIP64(square);
            for (piece_type = KNIGHT; piece_type <= KING; piece_type++) {
                piece = PIECEINDEX(color, piece_type);
                piece_square_table[piece][MIDDLEGAME][square] = 0;
                piece_square_table[piece][ENDGAME][square] = 0;
                switch (piece_type) {
                case KNIGHT:
                    piece_square_table[piece][MIDDLEGAME][square] = knight_pst[MIDDLEGAME][s] + MG_KNIGHT_VALUE;
                    piece_square_table[piece][ENDGAME][square] = knight_pst[ENDGAME][s] + EG_KNIGHT_VALUE;
                    break;
                case BISHOP:
                    piece_square_table[piece][MIDDLEGAME][square] = bishop_pst[MIDDLEGAME][s] + MG_BISHOP_VALUE;
                    piece_square_table[piece][ENDGAME][square] = bishop_pst[ENDGAME][s] + EG_BISHOP_VALUE;
                    break;
                case ROOK:
                    piece_square_table[piece][MIDDLEGAME][square] = rook_pst[MIDDLEGAME][s] + MG_ROOK_VALUE;
                    piece_square_table[piece][ENDGAME][square] = rook_pst[ENDGAME][s] + EG_ROOK_VALUE;
                    break;
                case QUEEN:
                    piece_square_table[piece][MIDDLEGAME][square] = queen_pst[MIDDLEGAME][s] + MG_QUEEN_VALUE;
                    piece_square_table[piece][ENDGAME][square] = queen_pst[ENDGAME][s] + EG_QUEEN_VALUE;
                    break;
                case PAWN:
                    piece_square_table[piece][MIDDLEGAME][square] = pawn_pst[MIDDLEGAME][s] + MG_PAWN_VALUE;
                    piece_square_table[piece][ENDGAME][square] = pawn_pst[ENDGAME][s] + EG_PAWN_VALUE;
                    break;
                case KING:
                    piece_square_table[piece][MIDDLEGAME][square] = king_pst[MIDDLEGAME][s];
                    piece_square_table[piece][ENDGAME][square] = king_pst[ENDGAME][s];
                    break;
                }
            }
        }
    }
}

t_chess_value calc_king_pawn_endgame(struct t_board *board, struct t_chess_eval *eval) {

	assert(board->pieces[WHITE][KNIGHT] == 0);
	assert(board->pieces[WHITE][BISHOP] == 0);
	assert(board->pieces[WHITE][ROOK] == 0);
	assert(board->pieces[WHITE][QUEEN] == 0);
	assert(board->pieces[BLACK][KNIGHT] == 0);
	assert(board->pieces[BLACK][BISHOP] == 0);
	assert(board->pieces[BLACK][ROOK] == 0);
	assert(board->pieces[BLACK][QUEEN] == 0);

	struct t_pawn_hash_record *pawn_record = lookup_pawn_hash(board, eval);

    return pawn_record->king_pawn_endgame_score;
}

void known_endgame_KPvk(struct t_board *board, struct t_chess_eval *eval)
{
    if (board->piecelist[WHITEPAWN] == 0)
        return;

    t_chess_square opponents_king = board->king_square[BLACK];
    t_chess_square own_king = board->king_square[WHITE];
    t_chess_square s = bitscan(board->piecelist[WHITEPAWN]);

    //-- Is the pawn obviously unstoppable?
    if (board->piecelist[WHITEPAWN] & cannot_catch_pawn_mask[BLACK][board->to_move][opponents_king])
        eval->static_score = 400 + 30 * RANK(s);

    //-- Is it a king's rook pawn?
    else if (COLUMN(s) == 7) {

        //-- Who is closer to h8
        if (square_distance(H8, own_king) > (square_distance(H8, opponents_king) - (board->to_move == BLACK)))
            eval->static_score = 0;
        else
            eval->static_score = 350 + 20 * RANK(s);
    }

    //-- Is it a queen's rook pawn?
    else if (COLUMN(s) == 0) {

        //-- Who is closer to h8
        if (square_distance(A8, own_king) > (square_distance(H8, opponents_king) - (board->to_move == BLACK)))
            eval->static_score = 0;
        else
            eval->static_score = 350 + 20 * RANK(s);
    }

    //-- Can the pawn be taken by the opponent
    else if (square_distance(s, own_king) < (square_distance(s, opponents_king) - (board->to_move == BLACK)))
        eval->static_score = 10;

    //-- Use normal heuristic of pawn advancing and proximity to king
    else
        eval->static_score = piece_square_table[WHITEKING][ENDGAME][own_king] - piece_square_table[BLACKKING][ENDGAME][opponents_king] + piece_square_table[WHITEPAWN][ENDGAME][s] + passed_pawn_bonus[WHITE][s] - 10 * square_distance(s, own_king) + 10 * square_distance(s, opponents_king);

    //-- Adjust for side to move
    eval->static_score *= (1 - board->to_move * 2);

}

void known_endgame_Kvkp(struct t_board *board, struct t_chess_eval *eval)
{
    if (board->piecelist[BLACKPAWN] == 0)
        return;

    t_chess_square opponents_king = board->king_square[WHITE];
    t_chess_square own_king = board->king_square[BLACK];
    t_chess_square s = bitscan(board->piecelist[BLACKPAWN]);

    //-- Is the pawn obviously unstoppable?
    if (board->piecelist[BLACKPAWN] & cannot_catch_pawn_mask[WHITE][board->to_move][opponents_king])
        eval->static_score = -400 - 30 * (7 - RANK(s));

    //-- Is it a king's rook pawn?
    else if (COLUMN(s) == 7) {

        //-- Who is closer to H1
        if (square_distance(H1, own_king) > (square_distance(H1, opponents_king) - (board->to_move == WHITE)))
            eval->static_score = 0;
        else
            eval->static_score = -350 - 20 * (7 - RANK(s));
    }

    //-- Is it a queen's rook pawn?
    else if (COLUMN(s) == 0) {

        //-- Who is closer to A1
        if (square_distance(A1, own_king) > (square_distance(H1, opponents_king) - (board->to_move == WHITE)))
            eval->static_score = 0;
        else
            eval->static_score = -350 - 20 * (7 - RANK(s));
    }

    //-- Can the pawn be taken by the opponent
    else if (square_distance(s, own_king) < (square_distance(s, opponents_king) - (board->to_move == WHITE)))
        eval->static_score = -10;

    //-- Use normal heuristic of pawn advancing and proximity to king
    else
        eval->static_score = -piece_square_table[BLACKKING][ENDGAME][own_king] + piece_square_table[WHITEKING][ENDGAME][opponents_king] - piece_square_table[BLACKPAWN][ENDGAME][s] + passed_pawn_bonus[BLACK][s] + 10 * square_distance(s, own_king) - 10 * square_distance(s, opponents_king);

    //-- Adjust for side to move
    eval->static_score *= (1 - board->to_move * 2);

}

void known_endgame_QKvk(struct t_board *board, struct t_chess_eval *eval)
{
    eval->static_score = lone_king[board->king_square[BLACK]] + 1200 - 10 * square_distance(board->king_square[WHITE], board->king_square[BLACK]);
    eval->static_score *= (1 - board->to_move * 2);
}

void known_endgame_Kvqk(struct t_board *board, struct t_chess_eval *eval)
{
    eval->static_score = -lone_king[board->king_square[WHITE]] - 1200 + 10 * square_distance(board->king_square[WHITE], board->king_square[BLACK]);
    eval->static_score *= (1 - board->to_move * 2);
}

void known_endgame_RKvk(struct t_board *board, struct t_chess_eval *eval)
{
    eval->static_score = lone_king[board->king_square[BLACK]] + 800 - 10 * square_distance(board->king_square[WHITE], board->king_square[BLACK]);
    eval->static_score *= (1 - board->to_move * 2);
}

void known_endgame_Kvrk(struct t_board *board, struct t_chess_eval *eval)
{
    eval->static_score = -lone_king[board->king_square[WHITE]] - 800 + 10 * square_distance(board->king_square[WHITE], board->king_square[BLACK]);
    eval->static_score *= (1 - board->to_move * 2);
}

void known_endgame_BBKvk(struct t_board *board, struct t_chess_eval *eval)
{
    eval->static_score = lone_king[board->king_square[BLACK]] + 1000 - 10 * square_distance(board->king_square[WHITE], board->king_square[BLACK]);
    eval->static_score *= (1 - board->to_move * 2);
}

void known_endgame_Kvbbk(struct t_board *board, struct t_chess_eval *eval)
{
    eval->static_score = -lone_king[board->king_square[WHITE]] - 1000 + 10 * square_distance(board->king_square[WHITE], board->king_square[BLACK]);
    eval->static_score *= (1 - board->to_move * 2);
}

void known_endgame_BNKvk(struct t_board *board, struct t_chess_eval *eval)
{
    if (board->piecelist[WHITEBISHOP] && board->piecelist[WHITEKNIGHT]) {
        t_chess_color bishop_square = bitscan(board->piecelist[WHITEBISHOP]);
        t_chess_color knight_square = bitscan(board->piecelist[WHITEKNIGHT]);
        eval->static_score = bishop_knight_corner[square_color[bishop_square]][board->king_square[BLACK]] + 950 - 10 * square_distance(board->king_square[WHITE], board->king_square[BLACK]) - 5 * square_distance(board->king_square[BLACK], knight_square);
        eval->static_score *= (1 - board->to_move * 2);
    }
    else
        eval->static_score = 0;
}

void known_endgame_Kvbnk(struct t_board *board, struct t_chess_eval *eval)
{
    if (board->piecelist[BLACKBISHOP] && board->piecelist[BLACKKNIGHT]) {
        t_chess_color bishop_square = bitscan(board->piecelist[BLACKBISHOP]);
        t_chess_color knight_square = bitscan(board->piecelist[BLACKKNIGHT]);
        eval->static_score = -bishop_knight_corner[square_color[bishop_square]][board->king_square[WHITE]] - 950 + 10 * square_distance(board->king_square[WHITE], board->king_square[BLACK]) + 5 * square_distance(board->king_square[WHITE], knight_square);
        eval->static_score *= (1 - board->to_move * 2);
    }
    else
        eval->static_score = 0;
}

void known_endgame_insufficient_material(struct t_board *board, struct t_chess_eval *eval)
{
    eval->static_score = 0;
    poke_draw(board->hash);
}

void known_endgame_KRNvk(struct t_board *board, struct t_chess_eval *eval)
{
    eval->static_score = lone_king[board->king_square[BLACK]] + 1100 - 10 * square_distance(board->king_square[WHITE], board->king_square[BLACK]);
    eval->static_score *= (1 - board->to_move * 2);
}

void known_endgame_Kvkrn(struct t_board *board, struct t_chess_eval *eval)
{
    eval->static_score = -lone_king[board->king_square[BLACK]] - 1100 + 10 * square_distance(board->king_square[WHITE], board->king_square[BLACK]);
    eval->static_score *= (1 - board->to_move * 2);
}

void known_endgame_KRBvk(struct t_board *board, struct t_chess_eval *eval)
{
    eval->static_score = lone_king[board->king_square[BLACK]] + 1100 - 10 * square_distance(board->king_square[WHITE], board->king_square[BLACK]);
    eval->static_score *= (1 - board->to_move * 2);
}

void known_endgame_Kvkrb(struct t_board *board, struct t_chess_eval *eval)
{
    eval->static_score = -lone_king[board->king_square[BLACK]] - 1100 + 10 * square_distance(board->king_square[WHITE], board->king_square[BLACK]);
    eval->static_score *= (1 - board->to_move * 2);
}

void known_endgame_KNvkp(struct t_board *board, struct t_chess_eval *eval)
{
    t_chess_value e = calc_evaluation(board, eval);
    if (e > -10)
        e = -10;
    eval->static_score = e * (1 - board->to_move * 2);
}

void known_endgame_KPvkn(struct t_board *board, struct t_chess_eval *eval)
{
    t_chess_value e = calc_evaluation(board, eval);
    if (e < 10)
        e = 10;
    eval->static_score = e * (1 - board->to_move * 2);
}

void known_endgame_KBvkp(struct t_board *board, struct t_chess_eval *eval)
{
    t_chess_value e = calc_evaluation(board, eval);
    if (e > -10)
        e = -10;
    eval->static_score = e * (1 - board->to_move * 2);
}

void known_endgame_KPvkb(struct t_board *board, struct t_chess_eval *eval)
{
    t_chess_value e = calc_evaluation(board, eval);
    if (e < 10)
        e = 10;
    eval->static_score = e * (1 - board->to_move * 2);
}

void known_endgame_KRvkn(struct t_board *board, struct t_chess_eval *eval)
{
    t_bitboard b = board->piecelist[WHITEROOK];

    if (b) {
        t_chess_square s = bitscan(b);
        eval->static_score = 4 * square_distance(board->king_square[BLACK], s);
        eval->static_score *= (1 - board->to_move * 2);
    }
    else
        eval->static_score = calc_evaluation(board, eval);
}

void known_endgame_KNvkr(struct t_board *board, struct t_chess_eval *eval)
{
    t_bitboard b = board->piecelist[BLACKROOK];

    if (b) {
        t_chess_square s = bitscan(b);
        eval->static_score = -4 * square_distance(board->king_square[WHITE], s);
        eval->static_score *= (1 - board->to_move * 2);
    }
    else
        eval->static_score = calc_evaluation(board, eval);

    ////-- Let's make this colorblind code!
    //t_chess_color color = BLACK;
    //t_chess_color opponent = OPPONENT(BLACK);

    ////-- Default score if hash collision
    //eval->static_score = 0;

    ////-- See if it's a possible black win if it's black to move
    //if (board->to_move == color){

    //	//-- Can black capture the white knight
    //	if (t_bitboard b = board->pieces[color][ROOK]){
    //		t_chess_square s = bitscan(b);
    //		b = board->pieces[opponent][KNIGHT];
    //		if ((rook_rays[s] & b) && ((king_mask[board->king_square[opponent]] & b) == 0)){

    //			//-- Rook can capture knight and win
    //			known_endgame_Kvrk(board, eval);
    //			return;
    //		}
    //	}
    //}

    ////-- See if the knight is pinned to the king and will be captured
    //else
    //{
    //	if ()

    //}

    ////-- See if the knight is pinned to the rook
    //dsvsavd
}

void known_endgame_KRvkb(struct t_board *board, struct t_chess_eval *eval)
{
    eval->static_score = 12;
    eval->static_score *= (1 - board->to_move * 2);
}

void known_endgame_KBvkr(struct t_board *board, struct t_chess_eval *eval)
{
    eval->static_score = -12;
    eval->static_score *= (1 - board->to_move * 2);
}

void known_endgame_KRBvkr(struct t_board *board, struct t_chess_eval *eval)
{
    eval->static_score = 12;
    eval->static_score *= (1 - board->to_move * 2);
}

void known_endgame_KRvkrb(struct t_board *board, struct t_chess_eval *eval)
{
    eval->static_score = -12;
    eval->static_score *= (1 - board->to_move * 2);
}

void known_endgame_KRNvkr(struct t_board *board, struct t_chess_eval *eval)
{
    eval->static_score = 12;
    eval->static_score *= (1 - board->to_move * 2);
}

void known_endgame_KRvkrn(struct t_board *board, struct t_chess_eval *eval)
{
    eval->static_score = -12;
    eval->static_score *= (1 - board->to_move * 2);
}

void known_endgame_KQBvkq(struct t_board *board, struct t_chess_eval *eval)
{
    eval->static_score = 12;
    eval->static_score *= (1 - board->to_move * 2);
}

void known_endgame_KQvkqb(struct t_board *board, struct t_chess_eval *eval)
{
    eval->static_score = -12;
    eval->static_score *= (1 - board->to_move * 2);
}

void known_endgame_KQNvkq(struct t_board *board, struct t_chess_eval *eval)
{
    eval->static_score = 12;
    eval->static_score *= (1 - board->to_move * 2);
}

void known_endgame_KQvkqn(struct t_board *board, struct t_chess_eval *eval)
{
    eval->static_score = -12;
    eval->static_score *= (1 - board->to_move * 2);
}

void known_endgame_KBNvkb(struct t_board *board, struct t_chess_eval *eval)
{
    eval->static_score = 12;
    eval->static_score *= (1 - board->to_move * 2);
}

void known_endgame_KBNvkn(struct t_board *board, struct t_chess_eval *eval)
{
    eval->static_score = 12;
    eval->static_score *= (1 - board->to_move * 2);
}

void known_endgame_KBvkbn(struct t_board *board, struct t_chess_eval *eval)
{
    eval->static_score = -12;
    eval->static_score *= (1 - board->to_move * 2);
}

void known_endgame_KNvkbn(struct t_board *board, struct t_chess_eval *eval)
{
    eval->static_score = -12;
    eval->static_score *= (1 - board->to_move * 2);
}

void known_endgame_KBPvk(struct t_board *board, struct t_chess_eval *eval)
{
	eval->pawn_evaluation = lookup_pawn_hash(board, eval);
	t_bitboard b;
	t_chess_square s;

	//-- Make sure it's not a clash
	if (board->piecelist[WHITEPAWN] == 0 || board->piecelist[WHITEBISHOP] == 0)
		eval->static_score = calc_evaluation(board, eval) * (1 - 2 * board->to_move);

	//-- Simple case where the pawn is unstoppable
	b = eval->pawn_evaluation->unstopable[WHITE];
	if (b){
		s = bitscan(b);
		eval->static_score = 1200 + (s >> 3) * 10;
		eval->static_score *= (1 - board->to_move * 2);
		return;
	}

	//
	//-- What's the promotion square?
	s = bitscan(board->piecelist[WHITEPAWN]);
	t_chess_square promotion_square = A8 + COLUMN(s);
	t_chess_square bishop_square = bitscan(board->piecelist[WHITEBISHOP]);

	//-- If bishop is the same color as promotion square then also a win
	if (square_color[promotion_square] == square_color[bishop_square]){
		eval->static_score = 500 + (s >> 3) * 20 - square_distance(board->king_square[WHITE], promotion_square) * 10 + square_distance(board->king_square[BLACK], promotion_square) * 10;
		eval->static_score *= (1 - board->to_move * 2);
		return;
	}

	//-- If the pawn is not on the "A" or "H" file it's a win for white but the king must support
	if (COLUMN(promotion_square) > 0 && COLUMN(promotion_square) < 7){
		eval->static_score = 500 + (s >> 3) * 20 - square_distance(board->king_square[WHITE], promotion_square) * 10 + square_distance(board->king_square[BLACK], promotion_square) * 10;
		eval->static_score *= (1 - board->to_move * 2);
		return;
	}

	//-- It's a draw if the Black King is closer to the promotion square!!
	if (board->to_move == WHITE){
		if (square_distance(board->king_square[WHITE], promotion_square) <= square_distance(board->king_square[BLACK], promotion_square)){
			eval->static_score = 500 + (s >> 3) * 20 - square_distance(board->king_square[WHITE], promotion_square) * 10 + square_distance(board->king_square[BLACK], promotion_square) * 10;
			eval->static_score *= (1 - board->to_move * 2);
			return;
		}
		else{
			eval->static_score = 10 - square_distance(board->king_square[WHITE], promotion_square) + square_distance(board->king_square[BLACK], promotion_square);
			eval->static_score *= (1 - board->to_move * 2);
			return;
		}

	}
	else{
		if (square_distance(board->king_square[WHITE], promotion_square) < square_distance(board->king_square[BLACK], promotion_square)){
			eval->static_score = 500 + (s >> 3) * 20 - square_distance(board->king_square[WHITE], promotion_square) * 10 + square_distance(board->king_square[BLACK], promotion_square) * 10;
			eval->static_score *= (1 - board->to_move * 2);
			return;
		}
		else{
			eval->static_score = 10 - square_distance(board->king_square[WHITE], promotion_square) + square_distance(board->king_square[BLACK], promotion_square);
			eval->static_score *= (1 - board->to_move * 2);
			return;
		}
	}
	assert(FALSE);
}

void known_endgame_Kvkbp(struct t_board *board, struct t_chess_eval *eval)
{
	eval->pawn_evaluation = lookup_pawn_hash(board, eval);
	t_bitboard b;
	t_chess_square s;

	//-- Make sure it's not a clash
	if (board->piecelist[BLACKPAWN] == 0 || board->piecelist[BLACKBISHOP] == 0)
		eval->static_score = calc_evaluation(board, eval) * (1 - 2 * board->to_move);

	//-- Simple case where the pawn is unstoppable
	b = eval->pawn_evaluation->unstopable[BLACK];
	if (b){
		s = bitscan(b);
		eval->static_score = -1200 - (7 - (s >> 3)) * 10;
		eval->static_score *= (1 - board->to_move * 2);
		return;
	}

	//
	//-- What's the promotion square?
	s = bitscan(board->piecelist[BLACKPAWN]);
	t_chess_square promotion_square = A1 + COLUMN(s);
	t_chess_square bishop_square = bitscan(board->piecelist[BLACKBISHOP]);

	//-- If bishop is the same color as promotion square then also a win
	if (square_color[promotion_square] == square_color[bishop_square]){
		eval->static_score = -500 - (7 - (s >> 3)) * 20 + square_distance(board->king_square[BLACK], promotion_square) * 10 - square_distance(board->king_square[WHITE], promotion_square) * 10;
		eval->static_score *= (1 - board->to_move * 2);
		return;
	}

	//-- If the pawn is not on the "A" or "H" file it's a win for white but the king must support
	if (COLUMN(promotion_square) > 0 && COLUMN(promotion_square) < 7){
		eval->static_score = -500 - (7 - (s >> 3)) * 20 + square_distance(board->king_square[BLACK], promotion_square) * 10 - square_distance(board->king_square[WHITE], promotion_square) * 10;
		eval->static_score *= (1 - board->to_move * 2);
		return;
	}

	//-- It's a draw if the Black King is closer to the promotion square!!
	if (board->to_move == BLACK){
		if (square_distance(board->king_square[BLACK], promotion_square) <= square_distance(board->king_square[WHITE], promotion_square)){
			eval->static_score = -500 - (7 - (s >> 3)) * 20 + square_distance(board->king_square[BLACK], promotion_square) * 10 - square_distance(board->king_square[WHITE], promotion_square) * 10;
			eval->static_score *= (1 - board->to_move * 2);
			return;
		}
		else{
			eval->static_score = -10 + square_distance(board->king_square[BLACK], promotion_square) - square_distance(board->king_square[WHITE], promotion_square);
			eval->static_score *= (1 - board->to_move * 2);
			return;
		}

	}
	else{
		if (square_distance(board->king_square[BLACK], promotion_square) < square_distance(board->king_square[WHITE], promotion_square)){
			eval->static_score = -500 - (7 - (s >> 3)) * 20 + square_distance(board->king_square[BLACK], promotion_square) * 10 - square_distance(board->king_square[WHITE], promotion_square) * 10;
			eval->static_score *= (1 - board->to_move * 2);
			return;
		}
		else{
			eval->static_score = -10 + square_distance(board->king_square[BLACK], promotion_square) - square_distance(board->king_square[WHITE], promotion_square);
			eval->static_score *= (1 - board->to_move * 2);
			return;
		}
	}
	assert(FALSE);
}

void known_endgame_KBPPvk(struct t_board *board, struct t_chess_eval *eval){

	//-- Are the pawns on the same file?
	t_chess_square p1 = 0;
	t_chess_square p2 = 1;

	t_bitboard b = board->piecelist[WHITEPAWN];
	if (b)
		p1 = bitscan_reset(&b);
	if (b)
		p2 = bitscan_reset(&b);
	
	int c = COLUMN(p1);

	if ((c == 0 || c == 7) && (c == COLUMN(p2))){
		b = board->piecelist[WHITEPAWN];
		board->piecelist[WHITEPAWN] = SQUARE64(p2);
		known_endgame_KBPvk(board, eval);
		board->piecelist[WHITEPAWN] = b;
	}
	else
		calc_evaluation(board, eval);
}

void known_endgame_Kvkbpp(struct t_board *board, struct t_chess_eval *eval){

	//-- Are the pawns on the same file?
	t_chess_square p1 = 0;
	t_chess_square p2 = 1;

	t_bitboard b = board->piecelist[BLACKPAWN];
	if (b)
		p1 = bitscan_reset(&b);
	if (b)
		p2 = bitscan_reset(&b);

	int c = COLUMN(p1);

	if ((c == 0 || c == 7) && (c == COLUMN(p2))){
		b = board->piecelist[BLACKPAWN];
		board->piecelist[BLACKPAWN] = SQUARE64(p1);
		known_endgame_Kvkbp(board, eval);
		board->piecelist[BLACKPAWN] = b;
	}
	else
		calc_evaluation(board, eval);
}

void known_endgame_KPPvk(struct t_board *board, struct t_chess_eval *eval){

	//-- Are the pawns on the same file?
	t_chess_square p1 = 0;
	t_chess_square p2 = 1;

	t_bitboard b = board->piecelist[WHITEPAWN];
	if (b)
		p1 = bitscan_reset(&b);
	if (b)
		p2 = bitscan_reset(&b);

	int c = COLUMN(p1);

	if ((c == 0 || c == 7) && (c == COLUMN(p2))){
		b = board->piecelist[WHITEPAWN];
		board->piecelist[WHITEPAWN] = SQUARE64(p2);
		known_endgame_KPvk(board, eval);
		board->piecelist[WHITEPAWN] = b;
	}
	else
		calc_evaluation(board, eval);
}

void known_endgame_Kvkpp(struct t_board *board, struct t_chess_eval *eval){

	//-- Are the pawns on the same file?
	t_chess_square p1 = 0;
	t_chess_square p2 = 1;

	t_bitboard b = board->piecelist[BLACKPAWN];
	if (b)
		p1 = bitscan_reset(&b);
	if (b)
		p2 = bitscan_reset(&b);

	int c = COLUMN(p1);

	if ((c == 0 || c == 7) && (c == COLUMN(p2))){
		b = board->piecelist[BLACKPAWN];
		board->piecelist[BLACKPAWN] = SQUARE64(p1);
		known_endgame_Kvkp(board, eval);
		board->piecelist[BLACKPAWN] = b;
	}
	else
		calc_evaluation(board, eval);
}
