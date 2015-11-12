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

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "defs.h"
#include "data.h"
#include "procs.h"
#include "bittwiddle.h"


inline BOOL can_do_null_move(struct t_board *board, struct t_pv_data *pv, int ply, t_chess_value alpha, t_chess_value beta) {

    t_chess_color color = board->to_move;
    int piece_count = popcount(board->occupied[color] ^ board->pieces[color][PAWN]);

    return !board->in_check 
		&& pv->eval->static_score >= beta 
		&& piece_count > 3;
}

t_chess_value alphabeta(struct t_board *board, int ply, int depth, t_chess_value alpha, t_chess_value beta, BOOL early_cutoff, struct t_move_record *exclude_move) {

    //-- Should we call qsearch?
    if (depth <= 0 && !board->in_check)
        return qsearch_plus(board, ply, depth, alpha, beta);

    //-- Increment the nodes
    nodes++;

    //-- see if we need to update stats */
    if ((nodes & message_update_mask) == 0)
        uci_check_status(board, ply);

    //-- Local Principle Variation variable
    struct t_pv_data *pv = &(board->pv_data[ply]);

    //-- Has the maximum depth been reached
    if (ply >= MAXPLY || uci.stop) {
        pv->best_line_length = ply;
        assert(pv->eval->static_score >= -CHECKMATE && pv->eval->static_score <= CHECKMATE);
        return pv->eval->static_score;
    }

    /* check to see if this is a repeated position or draw by 50 moves */
    if (repetition_draw(board)) {
        pv->best_line_length = ply;
        return 0;
    }

    //-- Mate Distance Pruning
    if (CHECKMATE - ply <= alpha) {
        return alpha;
    }
    else if (-CHECKMATE + ply >= beta) {
        return beta;
    }
    else if ((!board->in_check) && (-CHECKMATE + ply + 2 >= beta)) {
        return beta;
    }

    //-- Declare local variables
    struct t_pv_data *next_pv = pv->next_pv;
    struct t_pv_data *previous_pv = pv->previous_pv;

    t_chess_value					best_score = -CHECKMATE;
    t_chess_value					a = alpha;
    t_chess_value					b = beta;

    //-- Determine what type of node it is
    if (beta > alpha + 1)
        pv->node_type = node_pv;
    else if (previous_pv->node_type == node_pv)
        pv->node_type = node_lite_all;
    else if (previous_pv->node_type == node_cut)
        pv->node_type = node_all;
    else
        pv->node_type = node_cut;

    //-- Probe Hash
    struct t_move_record *hash_move = NULL;
    struct t_hash_record *hash_record = probe(board->hash);

    //-- Has there been a match?
    if (hash_record != NULL) {

		//-- Get the score from the hash table
		t_chess_value hash_score = get_hash_score(hash_record, ply);
		
		//-- Could it make a cut-off?
		if (early_cutoff && hash_record->depth >= depth) {

            //-- Score in hash table is at least as good as beta
            if (hash_record->bound != HASH_UPPER && hash_score >= beta) {
                hash_record->age = hash_age;
                assert(hash_score >= -CHECKMATE && hash_score <= CHECKMATE);
                return hash_score;
            }

            //-- Score is worse than alpha
            if (hash_record->bound != HASH_LOWER && hash_score <= alpha) {
                hash_record->age = hash_age;
                assert(hash_score >= -CHECKMATE && hash_score <= CHECKMATE);
                return hash_score;
            }

            //-- Score is more accurate
            if (hash_record->bound == HASH_EXACT) {
                hash_record->age = hash_age;
                pv->best_line_length = ply;
                update_best_line_from_hash(board, ply);
                assert(hash_score >= -CHECKMATE && hash_score <= CHECKMATE);
                return hash_score;
            }
        }

        //-- Store the hash move for further use!
        hash_move = hash_record->move;

        //-- Use the hash score to refine the node type
        if (hash_record->bound != HASH_UPPER && hash_score >= beta)
            pv->node_type = node_super_cut;

        else if (hash_record->bound != HASH_LOWER && hash_score <= alpha)
            pv->node_type = node_super_all;

        else if (hash_record->bound == HASH_EXACT && pv->node_type == node_all)
            pv->node_type = node_lite_all;
    }

    //-- Beta pruning
    if (early_cutoff && depth <= 4 && pv->node_type != node_pv && beta < MAX_CHECKMATE && beta > -MAX_CHECKMATE && !board->in_check) {

        int pessimistic_score = pv->eval->static_score - depth * 50 - 100;

        if (pessimistic_score >= beta)
            return pessimistic_score;
    }

	//-- Razoring.
	t_chess_value e;	
	if (early_cutoff && (depth <= 4) && pv->node_type != node_pv  && !board->in_check){

		t_chess_value razor_margin = depth * 50 + 50;
		if (pv->eval->static_score + razor_margin <= alpha){

			t_chess_value razor_alpha = alpha - razor_margin;
			e = qsearch_plus(board, ply, depth, razor_alpha, razor_alpha + 1);
			
			if (e <= razor_alpha)
				return e;
		}
	}
		
    //-- Null Move
    t_undo undo[1];

	pv->mate_threat = 0;
	pv->null_refutation = NULL;
	pv->extension = FALSE;
	if (early_cutoff && can_do_null_move(board, pv, ply, alpha, beta)) {

		//-- Calculate Reduction
		//int r = (800 + 70 * depth) / 256 + min(3, (pv->eval->static_score - beta) / 128);
		int r = min(4, 2 + (25 * depth) / 128 + (pv->eval->static_score - beta) / 128);
		//int r = 3;

		//-- Make the changes on the board
		make_null_move(board, undo);

		//-- Store the move in the PV data
		pv->current_move = NULL;

		//-- Clear the Killer +2
		if (ply + 2 <= MAXPLY){
			board->pv_data[ply + 2].killer1 = NULL;
			board->pv_data[ply + 2].killer2 = NULL;
		}

		//-- Evaluate the new board position
		evaluate(board, next_pv->eval);

		//-- Find the new score
		e = -alphabeta(board, ply + 1, depth - r - 1 , -beta, -beta + 1, TRUE, NULL);

		//-- undo the null move
		unmake_null_move(board, undo);

		//-- is it good enough for a cut-off?
		if (e >= beta) {
			if (e > MAX_CHECKMATE)
				e = beta;
			poke(board->hash, e, ply, depth, HASH_LOWER, NULL);
			return e;
		}

		//-- Is there a Mate Threat after a super-reduced move - if so then exit?
		if (e < -MAX_CHECKMATE){
			if (pv->previous_pv->reduction > 1)
				return alpha;
			pv->mate_threat = e;
		}

		//-- Record the move which refuted the NULL move
		if (ply < MAXPLY)
			pv->null_refutation = board->pv_data[ply + 1].current_move;
    }

    //-- Internal Iterative Deepening!
	if (hash_move == NULL && !uci.stop){

		//-- PV Nodes - we *really* need a good move
		if (pv->node_type == node_pv && depth > 2) {

			//-- Search with reduced depth
			e = alphabeta(board, ply, depth - 2, alpha, beta, FALSE, NULL);

			//-- If still no move then search with -INFINITY bound
			if (e <= alpha)
				e = alphabeta(board, ply, depth - 2, -CHESS_INFINITY, beta, FALSE, NULL);

			//-- Probe the hash
			hash_record = probe(board->hash);

			//-- Set the hash move
			if (hash_record != NULL)
				hash_move = hash_record->move;
		}

		//-- Fail high nodes
		//else if ((pv->node_type == node_cut || pv->node_type == node_super_cut) && (depth >= 7) && alpha > -MAX_CHECKMATE && beta < MAX_CHECKMATE){

		//	//-- Search with reduced depth
		//	e = alphabeta(board, ply, depth / 2, alpha, beta);

		//	//-- If still no move then search with -INFINITY bound
		//	if (e <= alpha)
		//		e = alphabeta(board, ply, depth / 2, -CHESS_INFINITY, beta);

		//	//-- Probe the hash
		//	hash_record = probe(board->hash);

		//	//-- Set the hash move
		//	if (hash_record != NULL)
		//		hash_move = hash_record->move;

		//}
    }

    //-- Generate All Moves
    struct t_move_list moves[1];
    moves->hash_move = hash_move;

    if (board->in_check) {
        generate_evade_check(board, moves);

        // Are we in checkmate?
        if (moves->count == 0) {
            pv->best_line_length = ply;
			e = -CHECKMATE + ply;
            return e;
        }
        order_evade_check(board, moves, ply);
    }
    else {
        generate_moves(board, moves);
        order_moves(board, moves, ply);
    }

    //-- Enhanced Transposition Cutoff?
	t_chess_color to_move = board->to_move;
	if (early_cutoff && (depth > 4) && pv->node_type != node_pv && beta < MAX_CHECKMATE && alpha > -MAX_CHECKMATE && !uci.stop) {
        BOOL fail_low;
        while (simple_make_next_move(board, moves, undo)) {

            //-- Calculate Reduction Conservatively i.e. assume minimum reduction
			if (board->in_check)
				pv->reduction = 0;
			else if (PIECETYPE(moves->current_move->piece) == PAWN && COLOR_RANK(to_move, moves->current_move->to_square) >= 6)
				pv->reduction = 0;
			else
				pv->reduction = 1;

            //-- Simple version of alpha_beta for tips of search
            e = -alphabeta_tip(board, ply + 1, depth - pv->reduction, -beta, &fail_low);

            //-- Take the move back
            unmake_move(board, undo);

            //-- Is it good enough for a cutoff?
            if (e >= beta) {
                poke(board->hash, e, ply, depth, HASH_LOWER, moves->current_move);
                assert(e >= -CHECKMATE && e <= CHECKMATE);
                return e;
            }

            //-- Is it going to enhance the move ordering?
            if (fail_low) {
                moves->value[moves->imove] += MOVE_ORDER_ETC;
                assert(moves->move[moves->imove] == moves->current_move);
            }

        }
        //-- Reset the real move count for the "proper" search
        moves->imove = moves->count;
    }

	////-- No Hash Move for Cut Nodes with score below beta and no good capture and no ETC
	//if (hash_move == NULL && (any_fail_low == FALSE) && ((pv->node_type == node_cut) || (pv->node_type == node_super_all)) && (depth >= 3) && (beta > pv->eval->static_score) && no_good_captures(board, moves)){

	//	//-- Search with reduced depth
	//	e = alphabeta(board, ply, depth - 3, alpha, beta);

	//	//-- If still no move then search with -INFINITY bound
	//	if (e <= alpha)
	//		e = alphabeta(board, ply, depth - 3, -CHESS_INFINITY, beta);

	//	//-- Probe the hash
	//	hash_record = probe(board->hash);

	//	//-- Set the hash move
	//	if (hash_record != NULL)
	//		hash_move = hash_record->move;
	//}

    //-- Create the list of "bad" captures
    struct t_move_list bad_moves[1];
    bad_moves->count = 0;
    bad_moves->imove = 0;

    //-- Reset the move count (must be after IID)
    pv->legal_moves_played = 0;

    //-- Variables used to calculate the reduction
	t_chess_color opponent = OPPONENT(to_move);
    BOOL in_check = board->in_check;
    struct t_move_record *last_move = NULL;
	if (ply > 1)
        last_move = board->pv_data[ply - 2].current_move;


    //-- Play moves
    while (!uci.stop && make_next_move(board, moves, bad_moves, undo)) {

        //-- Increment the "legal_moves_played" counter
        pv->legal_moves_played++;
        pv->current_move = moves->current_move;

		//========================================//
		// Futility Pruning
		//========================================//
		if (uci.options.futility_pruning && is_futile(pv, next_pv, depth, a, b)){
			unmake_move(board, undo);
			continue;
		}		
		
		//-- Clear the Killer +2
		if (ply + 2 <= MAXPLY){
			board->pv_data[ply + 2].killer1 = NULL;
			board->pv_data[ply + 2].killer2 = NULL;
		}

		//-- Evaluate the new board position
        evaluate(board, next_pv->eval);

		////========================================//
		//// See if Extension is Necessary
		////========================================//
		//pv->extension = FALSE;
		//if (pv->mate_threat){

		//	e = -alphabeta(board, ply + 1, depth - 1, -CHECKMATE, pv->mate_threat + 2);

		//	pv->extension = (e > pv->mate_threat);

		//	if (e <= pv->mate_threat && e <= a)
		//	{
		//		unmake_move(board, undo);
		//		continue;
		//	}
		//}
			
		//========================================//
		// Calculate reduction
		//========================================//

		struct t_move_record *current_move = pv->current_move;

		//-- In Check?
		if (board->in_check){
			if (see_safe(board, current_move->to_square, 0))
				pv->reduction = 0;
			else if (ply > 3 && board->pv_data[ply - 1].in_check && board->pv_data[ply - 3].in_check)
				pv->reduction = 0;
			else
				pv->reduction = 1;
		}

		//-- Pawn push to 7th
		else if (PIECETYPE(current_move->piece) == PAWN && COLOR_RANK(to_move, current_move->to_square) >= 6){
			
			//-- Pawn Promotion
			if (current_move->promote_to){
				
				if (pv->legal_moves_played == 1 || PIECETYPE(current_move->promote_to) == QUEEN){

					//--Extend if it's a safe pawn promotion or first move
					if ((pv->current_move->captured && moves->current_move_see_positive) || see_safe(board, current_move->to_square, 0))
						pv->reduction = 0;
					else
						pv->reduction = 1;
				}

				// Reduce Heavily if not a queen promotion
				else
					pv->reduction = 5;
			}

			//-- Push to the 7th
			else if (pv->legal_moves_played == 1 || (pv->current_move->captured && moves->current_move_see_positive) || see_safe(board, current_move->to_square, 0))
				pv->reduction = 0;
			else
				pv->reduction = 1;

		}

		//-- First Move?
		else if (pv->legal_moves_played == 1)
			pv->reduction = 1;

		////-- Under Threat of Mate
		//else if (pv->mate_threat)
		//	pv->reduction = 1;

		//-- Good Capture?
		else if (pv->current_move->captured && moves->current_move_see_positive){
			pv->reduction = 1;
		}

		//-- Is this getting out of check?
		else if (in_check){
			if (pv->current_move == pv->check_killer1)
				pv->reduction = 1;
			else if (PIECETYPE(current_move->piece) == KING){
				if (CAN_CASTLE(to_move, board->castling))
					pv->reduction = 4;
				else
					pv->reduction = 1;
			}
			else if (current_move->captured) /* must be a bad capture */
				pv->reduction = 2;
			else if (see_safe(board, current_move->to_square, 0))
				pv->reduction = 1;
			else
				pv->reduction = 2;
		}

		//-- Don't reduce Killers!
		else if (pv->current_move == pv->killer1){
			pv->reduction = 1;
		}

		//-- Does it move a threatened piece?
		else if (pv->null_refutation != NULL && pv->null_refutation->to_square == pv->current_move->from_square){

			if (see_safe(board, current_move->to_square, 0))
				pv->reduction = 1;
			else
				pv->reduction = 3;
		}

		//-- Candidate for serious reductions
		else{

			switch (pv->node_type)
			{
			case node_cut:
				pv->reduction = 3;
				if (pv->current_move->captured)
					pv->reduction += 1; 
				break;

			case node_super_cut:
				pv->reduction = 4;
				if (pv->current_move->captured)
					pv->reduction += 1; 
				break;

			case node_pv:
				if (pv->legal_moves_played > 2)
					pv->reduction = 2;
				else
					pv->reduction = 1;
				break;

			case node_lite_all:
				if (pv->legal_moves_played > 2)
					pv->reduction = 2;
				else
					pv->reduction = 1;
				
				if (pv->current_move->captured)
					pv->reduction += 1;				
				break;

			case node_super_all:
				if (current_move->captured){
					if (pv->legal_moves_played < 4)
						pv->reduction = 3;
					else
						pv->reduction = 4;
				}
				else if (!see_safe(board, current_move->to_square, 0)){
					if (pv->legal_moves_played < 4)
						pv->reduction = 4;
					else if (pv->legal_moves_played < 12)
						pv->reduction = 5;
					else 
						pv->reduction = 6;
				}
				else if (pv->legal_moves_played < 4)
					pv->reduction = 2;
				else if (pv->legal_moves_played < 12)
					pv->reduction = 3;
				else
					pv->reduction = 4;
				break;

			case node_all:
				if (current_move->captured){
					if (pv->legal_moves_played < 4)
						pv->reduction = 3;
					else 
						pv->reduction = 4;
				}
				else if (!see_safe(board, current_move->to_square, 0)){
					if (pv->legal_moves_played < 4)
						pv->reduction = 4;
					else 
						pv->reduction = 5;
				}
				else if (pv->legal_moves_played < 4)
					pv->reduction = 2;
				else if (pv->legal_moves_played < 18)
					pv->reduction = 3;
				else
					pv->reduction = 4;
				break;

			}

		}

        //-- Search the next ply at reduced depth
        e = -alphabeta(board, ply + 1, depth - pv->reduction, -b, -a, TRUE, NULL);

        //-- Fail high on a super-reduced move?
        if (e > a && pv->reduction > 1) {
            pv->reduction = 1;

            //-- Search again using the full width
            e = -alphabeta(board, ply + 1, depth - 1, -beta, -a, TRUE, NULL);
        }

        //-- Is a research required?
        else if (alpha + 1 != beta && e > a && a + 1 == b)
            e = -alphabeta(board, ply + 1, depth - pv->reduction, -beta, -a, TRUE, NULL);

        unmake_move(board, undo);

        //-- Is it good enough to cut-off?
        if (e >= beta) {
            if (board->in_check)
                update_check_killers(pv, depth);
            else
                update_killers(pv, depth);

            //-- Record the cutoff
            cutoffs++;
            if (pv->legal_moves_played == 1)
                first_move_cutoffs++;

            //-- Store in the hash table
            poke(board->hash, e, ply, depth, HASH_LOWER, pv->current_move);
            return e;
        }

        //-- Is it the best so far?
        if (e > best_score) {
            best_score = e;

            //-- Does it improve upon alpha (i.e. is it part of the PV)?
            if (e > a) {
                a = e;

                //-- Update the Principle Variation
                update_best_line(board, ply);
            }
        }

        // Reset the zero width window
        b = a + 1;

		//-- Was this a fail low at a node which should have failed high?
		//if (pv->node_type == )
    }

    //-- Is it a draw
    if (pv->legal_moves_played == 0) {
        pv->best_line_length = ply;
        return 0;
    }

    //-- Update Hash
    if (best_score > alpha)
        poke(board->hash, best_score, ply, depth, HASH_EXACT, pv->best_line[ply]);
	else
        poke(board->hash, best_score, ply, depth, HASH_UPPER, NULL);

    // Return Best Score found
    assert(best_score >= -CHECKMATE && best_score <= CHECKMATE);
    return best_score;

}

t_chess_value qsearch_plus(struct t_board *board, int ply, int depth, t_chess_value alpha, t_chess_value beta) {

    //-- Principle Variation
    struct t_pv_data *pv = &(board->pv_data[ply]);

    //-- Has the maximum depth been reached
    if (ply >= MAXPLY || uci.stop)
        return pv->eval->static_score;

    //-- Increment the node count
    qnodes++;

    /* check to see if this is a repeated position or draw by 50 moves */
    if (repetition_draw(board)) {
        pv->best_line_length = ply;
        return 0;
    }

    //-- Mate Distance Pruning
    if (CHECKMATE - ply <= alpha) {
        return alpha;
    }
    else if (-CHECKMATE + ply >= beta) {
        return beta;
    }
    else if ((!board->in_check) && (-CHECKMATE + ply + 2 >= beta)) {
        return beta;
    }

    //-- Next Principle Variation
    struct t_pv_data *next_pv = &(board->pv_data[ply + 1]);

    //-- Define the local variables
    pv->legal_moves_played = 0;
    t_chess_value best_score;
    t_chess_value a = alpha;
    t_chess_value b = beta;
	t_chess_value e;

    // Declare local variables
    t_undo undo[1];

    //-- Generate All Moves
    struct t_move_list moves[1];
    moves->hash_move = NULL;

    //-----------------------------------------------
    //-- Handling In-Check (e.g. Cannot Stand-Pat)
    //-----------------------------------------------
    if (board->in_check) {

        best_score = -CHECKMATE;

        //-- Generate moves which get out of check
        generate_evade_check(board, moves);

        // Are we in checkmate?
        if (moves->count == 0) {
            pv->best_line_length = ply;
            return -CHECKMATE + ply;
        }

        t_chess_value(*q_search)(struct t_board *board, int ply, int depth, t_chess_value alpha, t_chess_value beta);

        if (moves->count == 1)
            q_search = &qsearch_plus;
        else {
            q_search = &qsearch;
            //-- Order the moves
            order_evade_check(board, moves, ply);
        }

        //-- Play moves
        while (make_next_best_move(board, moves, undo)) {

            //-- Increment the "legal_moves_played" counter
            pv->legal_moves_played++;
            pv->current_move = moves->current_move;

            //-- Evaluate the new board position
            evaluate(board, next_pv->eval);

            //-- More than one move out of check so just use "vanilla" qsearch
            e = -q_search(board, ply + 1, depth - 1, -b, -a);

            //-- Is a research required?
            if (alpha + 1 != beta && e > a && a + 1 == b)
                e = -q_search(board, ply + 1, depth - 1, -beta, -a);

            unmake_move(board, undo);

            //-- Is it good enough to cut-off?
            if (e >= beta) {
                update_check_killers(pv, 0);
                poke(board->hash, e, ply, depth, HASH_LOWER, pv->current_move);
                return e;
            }

            //-- Is it the best so far?
            if (e > best_score) {
                best_score = e;

                //-- Does it improve upon alpha (i.e. is it part of the PV)?
                if (e > a) {
                    a = e;

                    //-- Update the Principle Variation
                    update_best_line(board, ply);
                }
            }

            // Reset the zero width window
            b = a + 1;

        }
    }
    else {
        //--------------------------------------------------------
        //-- Normal moves handled differently (e.g. can stand-pat)
        //--------------------------------------------------------

        //-- Does stand-pat cause a cut-off?
        e = pv->eval->static_score;
        if (e >= beta)
            return e;

        //-- Does the current value raise alpha?
        best_score = e;
        if (e > alpha) {
            a = e;
            pv->best_line_length = ply;
        }

        //-- Generate all captures
        generate_captures(board, moves);

        //-- Order the moves
        order_captures(board, moves);

        //-- Play *ALL* captures
        while (make_next_best_move(board, moves, undo)) {

            //-- Increment the "legal_moves_played" counter
            pv->legal_moves_played++;
            pv->current_move = moves->current_move;

			//-- Evaluate the new board position
			evaluate(board, next_pv->eval);

			//-- Search the next ply at reduced depth
			e = -qsearch(board, ply + 1, depth - 1, -b, -a);

			//-- Is a research required?
			if (alpha + 1 != beta && e > a && a + 1 == b)
				e = -qsearch(board, ply + 1, depth - 1, -beta, -a);

            unmake_move(board, undo);

            //-- Is it good enough to cut-off?
            if (e >= beta) {
                return e;
            }

            //-- Is it the best so far?
            if (e > best_score) {
                best_score = e;

                //-- Does it improve upon alpha (i.e. is it part of the PV)?
                if (e > a) {
                    a = e;
                    update_best_line(board, ply);
                }
            }

            // Reset the zero width window
            b = a + 1;

        }

		//-- Reset the move count
		moves->count = 0;

        //-- Now Try the checks!
		generate_quiet_checks(board, moves);

		////-- Maybe if there are many, one will be a checkmate?
		//if (moves->count > 4){

		//	while (simple_make_next_move(board, moves, undo)) {

		//		assert(board->in_check);

		//		//-- Generate moves
		//		struct t_move_list evade_check_moves[1];
		//		generate_evade_check(board, evade_check_moves);

		//		//-- Take the move back
		//		unmake_move(board, undo);

		//		//-- Is it Checkmate?
		//		if (evade_check_moves->count == 0){
		//			pv->best_line_length = ply + 1;
		//			e = +CHECKMATE - ply - 1;
		//			return e;
		//		}
		//	}
		//	//-- Reset the real move count 
		//	moves->imove = moves->count;
		//}
		
        //-- Order the moves
        order_moves(board, moves, ply);

        //-- Play moves
        while (make_next_best_move(board, moves, undo)) {

            //-- Increment the "legal_moves_played" counter
            pv->legal_moves_played++;
            pv->current_move = moves->current_move;

            //-- Evaluate the new board position
            evaluate(board, next_pv->eval);

            //-- Search the next ply at reduced depth
            e = -qsearch_plus(board, ply + 1, depth - 1, -b, -a);

            //-- Is a research required?
            if (alpha + 1 != beta && e > a && a + 1 == b)
                e = -qsearch_plus(board, ply + 1, depth - 1, -beta, -a);

            unmake_move(board, undo);

            //-- Is it good enough to cut-off?
            if (e >= beta) {
                poke(board->hash, e, ply, depth, HASH_LOWER, pv->current_move);
                update_killers(pv, 0);
                return e;
            }

            //-- Is it the best so far?
            if (e > best_score) {
                best_score = e;

                //-- Does it improve upon alpha (i.e. is it part of the PV)?
                if (e > a) {
                    a = e;
                    update_best_line(board, ply);
                }
            }

            // Reset the zero width window
            b = a + 1;

        }
    }

    //-- Update Hash
    if (best_score > alpha)
        poke(board->hash, best_score, ply, depth, HASH_EXACT, pv->best_line[ply]);

    // Return Best Score found
    return best_score;
}

t_chess_value qsearch(struct t_board *board, int ply, int depth, t_chess_value alpha, t_chess_value beta) {

    //-- Principle Variation
    struct t_pv_data *pv = &(board->pv_data[ply]);

    //-- Has the maximum depth been reached
    if (ply >= MAXPLY || uci.stop)
        return pv->eval->static_score;

    //-- Increment the node count
    qnodes++;

    //-- Is this the deepest?
    if (ply > deepest) {
        deepest = ply;
        do_uci_depth();
    }

    //-- Mate Distance Pruning
    if (CHECKMATE - ply <= alpha) {
        return alpha;
    }
    else if (-CHECKMATE + ply >= beta) {
        return beta;
    }

    //-- PV of Next Ply
    struct t_pv_data *next_pv = &(board->pv_data[ply + 1]);

    //-- Define the local variables
    pv->legal_moves_played = 0;
    t_chess_value best_score;
    t_chess_value a = alpha;
    t_chess_value b = beta;
    t_chess_value e;


    // Declare local variables
    t_undo undo[1];

    //-- Generate All Moves
    struct t_move_list moves[1];
    moves->hash_move = NULL;

    //-----------------------------------------------
    //-- Handling In-Check (e.g. Cannot Stand-Pat)
    //-----------------------------------------------
    if (board->in_check) {

        best_score = -CHECKMATE;

        //-- Generate moves which get out of check
        generate_evade_check(board, moves);

        // Are we in checkmate?
        if (moves->count == 0) {
            pv->best_line_length = ply;
            return -CHECKMATE + ply;
        }

        //-- Order the moves
        order_evade_check(board, moves, ply);

        //-- Play moves
        while (make_next_best_move(board, moves, undo)) {

            //-- Increment the "legal_moves_played" counter
            pv->legal_moves_played++;
            pv->current_move = moves->current_move;

            //-- Evaluate the new board position
            evaluate(board, next_pv->eval);

            //-- Search the next ply at reduced depth
            e = -qsearch(board, ply + 1, depth - 1, -b - 1, -a);

            //-- Is a research required?
            if (alpha + 1 != beta && e > a && a + 1 == b)
                e = -qsearch(board, ply + 1, depth - 1, -beta, -a);

            unmake_move(board, undo);

            //-- Is it good enough to cut-off?
            if (e >= beta) {
                update_check_killers(pv, 0);
                return e;
            }

            //-- Is it the best so far?
            if (e > best_score) {
                best_score = e;

                //-- Does it improve upon alpha (i.e. is it part of the PV)?
                if (e > a) {
                    a = e;

                    //-- Update the Principle Variation
                    update_best_line(board, ply);
                }
            }

            // Reset the zero width window
            b = a + 1;

        }

    }
    else {
        //--------------------------------------------------------
        //-- Normal moves handled differently (e.g. can stand-pat)
        //--------------------------------------------------------

        //-- Does stand-pat cause a cutoff?
        e = pv->eval->static_score;
        if (e >= beta)
            return e;

        //-- Does the current value raise alpha?
        best_score = e;
        if (e > alpha) {
            a = e;
            pv->best_line_length = ply;
        }

        //-- Generate all captures
        generate_captures(board, moves);

        //-- Order the moves
        order_captures(board, moves);

		//-- Only search break even captures during the PV
		//int threshold = 10;
		//if (beta != alpha + 1)
		int threshold = 0;

        //-- Play moves
        while (make_next_see_positive_move(board, moves, threshold, undo)) {

            //-- Increment the "legal_moves_played" counter
            pv->legal_moves_played++;
            pv->current_move = moves->current_move;

            //-- Evaluate the new board position
            evaluate(board, next_pv->eval);

            //-- Search the next ply at reduced depth
            e = -qsearch(board, ply + 1, depth - 1, -b, -a);

            //-- Is a research required?
            if (alpha + 1 != beta && e > a && a + 1 == b)
                e = -qsearch(board, ply + 1, depth - 1, -beta, -a);

            unmake_move(board, undo);

            //-- Is it good enough to cut-off?
            if (e >= beta)
                return e;

            //-- Is it the best so far?
            if (e > best_score) {
                best_score = e;

                //-- Does it improve upon alpha (i.e. is it part of the PV)?
                if (e > a) {
                    a = e;
                    update_best_line(board, ply);
                }
            }

            // Reset the zero width window
            b = a + 1;

        }
    }

    // Return Best Score found
    return best_score;

}

t_chess_value is_it_checkmate(struct t_board *board, int ply){

	return 0;

}

t_chess_value alphabeta_tip(struct t_board *board, int ply, int depth, t_chess_value alpha, BOOL *fail_low) {

    //-- Set the default of Fail Low
    *fail_low = FALSE;

    //-- Has the maximum depth been reached
    if (ply >= MAXPLY) {
        return 0;
    }

    /* check to see if this is a repeated position or draw by 50 moves */
    if (repetition_draw(board)) {
        return 0;
    }

    //-- Probe Hash
    struct t_move_record *hash_move = NULL;
    t_hash_record *hash_record = probe(board->hash);

    //-- Has there been a match?
    if (hash_record != NULL) {

        //-- Get the score from the hash table
        t_chess_value hash_score = get_hash_score(hash_record, ply);

        //-- Score is worse than alpha
        if (hash_record->bound != HASH_LOWER && hash_score <= alpha) {
            *fail_low = TRUE;
            if (hash_record->depth >= depth)
                return hash_score;
        }
    }

    // Return Infinity
    return +CHESS_INFINITY;

}
