/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#include "display-plan.h"
#include "display-priv.h"
#include "guacamole/display.h"
#include "guacamole/rect.h"

/**
 * Returns whether the given rectangle crosses the boundaries of any two
 * adjacent cells in a grid, where each cell in the grid is
 * 2^GUAC_DISPLAY_MAX_COMBINED_SIZE pixels on each side.
 *
 * This function exists because combination of adjacent image updates is
 * intentionally limited to a certain size in order to favor parallelism.
 * Greedily combining in the horizontal direction works, but in practice tends
 * to produce a vertical series of strips that are offset from each other to
 * the point that they cannot be further combined. Anchoring combined image
 * updates to a grid helps prevent ths.
 *
 * @param rect
 *     The rectangle to test.
 *
 * @return
 *     Non-zero if the rectangle crosses the boundary of any adjacent pair of
 *     cells in a grid, where each cell is 2^GUAC_DISPLAY_MAX_COMBINED_SIZE
 *     pixels on each side, zero otherwise.
 */
static int guac_display_plan_rect_crosses_boundary(const guac_rect* rect) {

    /* A particular rectangle crosses a grid boundary if and only if expanding
     * that rectangle to fit the grid would mean increasing the size of that
     * rectangle beyond a single grid cell */

    guac_rect rect_copy = *rect;
    guac_rect_align(&rect_copy, GUAC_DISPLAY_MAX_COMBINED_SIZE);

    const int max_size_pixels = 1 << GUAC_DISPLAY_MAX_COMBINED_SIZE;
    return guac_rect_width(&rect_copy) > max_size_pixels
        || guac_rect_height(&rect_copy) > max_size_pixels;

}

/**
 * Returns whether the two rectangles are adjacent and share exactly one common
 * edge.
 *
 * @param op_a
 *     One of the rectangles to compare.
 *
 * @param op_b
 *     The rectangle to compare op_a with.
 *
 * @return
 *     Non-zero if the rectangles are adjacent and share exactly one common
 *     edge, zero otherwise.
 */
static int guac_display_plan_has_common_edge(const guac_display_plan_operation* op_a,
        const guac_display_plan_operation* op_b) {

    /* Two operations share a common edge if they are perfectly aligned
     * vertically and have the same left/right or right/left edge */
    if (op_a->dest.top == op_b->dest.top
            && op_a->dest.bottom == op_b->dest.bottom) {

        return op_a->dest.right == op_b->dest.left
            || op_a->dest.left == op_b->dest.right;

    }

    /* Two operations share a common edge if they are perfectly aligned
     * horizontally and have the same top/bottom or bottom/top edge */
    else if (op_a->dest.left == op_b->dest.left
            && op_a->dest.right == op_b->dest.right) {

        return op_a->dest.top == op_b->dest.bottom
            || op_a->dest.bottom == op_b->dest.top;

    }

    /* There are no other cases where two operations share a common edge */
    return 0;

}

/**
 * Returns whether the given pair of operations should be combined into a
 * single operation.
 *
 * @param plan
 *     The plan containing op_a and op_b. Supplies the worker-balance
 *     target (plan->img_combine_target_area) used to cap IMG+IMG
 *     combines on frames where the existing 512x512 crosses-boundary
 *     cap would over-collapse the plan.
 *
 * @param op_a
 *     The first operation to check.
 *
 * @param op_b
 *     The second operation to check.
 *
 * @return
 *     Non-zero if the operations would be better represented as a single,
 *     combined operation, zero otherwise.
 */
static int guac_display_plan_should_combine(const guac_display_plan* plan,
        const guac_display_plan_operation* op_a,
        const guac_display_plan_operation* op_b) {

    /* Operations can only be combined within the same layer */
    if (op_a->layer != op_b->layer)
        return 0;

    /* Simulate combination */
    guac_rect combined = op_a->dest;
    guac_rect_extend(&combined, &op_b->dest);

    /* Operations of the same type can be trivially unified under specific
     * circumstances */
    if (op_a->type == op_b->type) {
        switch (op_a->type) {

            /* Copy operations can be combined if they are perfectly adjacent
             * (exactly share an edge) and copy from the same source layer in
             * the same direction */
            case GUAC_DISPLAY_PLAN_OPERATION_COPY:
                if (op_a->src.layer_rect.layer == op_b->src.layer_rect.layer
                        && guac_display_plan_has_common_edge(op_a, op_b)) {

                    int delta_xa = op_a->dest.left - op_a->src.layer_rect.rect.left;
                    int delta_ya = op_a->dest.top  - op_a->src.layer_rect.rect.top;
                    int delta_xb = op_b->dest.left - op_b->src.layer_rect.rect.left;
                    int delta_yb = op_b->dest.top  - op_b->src.layer_rect.rect.top;

                    return delta_xa == delta_xb
                        && delta_ya == delta_yb
                        && !guac_display_plan_rect_crosses_boundary(&combined);

                }
                break;

            /* Rectangle-drawing operations can be combined if they are
             * perfectly adjacent (exactly share an edge) and draw the same
             * color. Adjacent translucent rects with non-identical (but
             * compatible) colors are handled separately - see
             * try_merge_translucent_rects() and its caller in
             * guac_display_plan_combine_if_improved() below. */
            case GUAC_DISPLAY_PLAN_OPERATION_RECT:
                return op_a->src.rect.color == op_b->src.rect.color
                    && guac_display_plan_has_common_edge(op_a, op_b)
                    && !guac_display_plan_rect_crosses_boundary(&combined);

            /* Image-drawing operations can be combined if doing so neither
             * exceeds the 512x512 grid-cell cap (set by cross-boundary
             * detection) nor exceeds the worker-balance target area.
             * The cap enforces an absolute per-op encode ceiling; the
             * target enforces rough balance across the encode worker
             * pool so that small-area frames do not collapse to fewer
             * ops than the pool can process in parallel. */
            case GUAC_DISPLAY_PLAN_OPERATION_IMG: {
                if (guac_display_plan_rect_crosses_boundary(&combined))
                    return 0;
                size_t combined_area =
                        (size_t) guac_rect_width(&combined)
                      * (size_t) guac_rect_height(&combined);
                return combined_area <= plan->img_combine_target_area;
            }

            /* Other combinations require more complex logic... (see below) */
            default:
                break;

        }
    }

    /* Combine if result is still small */
    int combined_width = guac_rect_width(&combined);
    int combined_height = guac_rect_height(&combined);
    if (combined_width <= GUAC_DISPLAY_NEGLIGIBLE_WIDTH && combined_height <= GUAC_DISPLAY_NEGLIGIBLE_HEIGHT)
        return 1;

    /* Estimate costs of the existing update, new update, and both combined */
    int cost_ab = GUAC_DISPLAY_BASE_COST + combined_width * combined_height;
    int cost_a  = GUAC_DISPLAY_BASE_COST + op_a->dirty_size;
    int cost_b  = GUAC_DISPLAY_BASE_COST + op_b->dirty_size;

    /* Reduce cost if no image data */
    if (op_a->type != GUAC_DISPLAY_PLAN_OPERATION_IMG) cost_a /= GUAC_DISPLAY_DATA_FACTOR;
    if (op_b->type != GUAC_DISPLAY_PLAN_OPERATION_IMG) cost_b /= GUAC_DISPLAY_DATA_FACTOR;

    /* Combine if cost estimate shows benefit or the increase in cost is
     * negligible */
    if ((cost_ab <= cost_b + cost_a)
            || (cost_ab - cost_a <= cost_a / GUAC_DISPLAY_NEGLIGIBLE_INCREASE)
            || (cost_ab - cost_b <= cost_b / GUAC_DISPLAY_NEGLIGIBLE_INCREASE))
        return 1;

    /* Otherwise, do not combine */
    return 0;

}

/**
 * Minimum last-frame channel delta required across the combined sample set
 * before the merged-α derivation is considered precise enough to be useful.
 * Mirrors GUAC_DISPLAY_PLAN_TRANSLUCENT_MIN_DELTA from display-plan-rect.c
 * - both passes must agree on the threshold.
 */
#define GUAC_DISPLAY_PLAN_TRANSLUCENT_MERGE_MIN_DELTA 16

/**
 * Attempts to merge two adjacent translucent RECT ops by deriving a single
 * (α, src) approximation that satisfies the source-over equations for the
 * sample pixels of both ops. The same most-distant-pair system of
 * equations used for initial detection runs here over the four pooled
 * samples (two from each op), and the merged approximation is verified
 * against all four; if any sample falls outside the per-channel
 * tolerance, the merge is rejected and the ops remain separate.
 *
 * On success, op_a's rect data is updated in place: the color is set to
 * the merged (α, src), and the two stored sample pixels are replaced with
 * the most-distant pair found across the merged sample set. This keeps
 * future merges (chains of three or more rects) operating on increasingly
 * representative samples - whatever pair currently spans the widest
 * channel delta is preserved as the merge progresses.
 *
 * @param op_a
 *     The translucent RECT op to absorb op_b into. On success, its
 *     src.rect color and sample pixels are overwritten with the merged
 *     approximation.
 *
 * @param op_b
 *     The translucent RECT op being absorbed. Read-only here; the caller
 *     transitions it to NOP after the rect extension.
 *
 * @return
 *     Non-zero if a single (α, src) approximation satisfies all four
 *     pooled samples within tolerance, zero otherwise.
 */
static int try_merge_translucent_rects(guac_display_plan_operation* op_a,
        const guac_display_plan_operation* op_b) {

    /* Pool the four sample pixels from both ops. */
    uint32_t p[4] = {
        op_a->src.rect.pending_a, op_a->src.rect.pending_b,
        op_b->src.rect.pending_a, op_b->src.rect.pending_b
    };
    uint32_t l[4] = {
        op_a->src.rect.last_a, op_a->src.rect.last_b,
        op_b->src.rect.last_a, op_b->src.rect.last_b
    };

    /* Find the most-distant pair across all six (i, j) sample pairs and
     * all three RGB channels. Maximizing |last_j - last_i| in some
     * channel maximizes the precision of the α derivation below. */
    int best_delta = 0;
    int best_dl = 0, best_dp = 0;
    int best_i = 0, best_j = 1;

    for (int i = 0; i < 4; i++) {
        for (int j = i + 1; j < 4; j++) {

            uint32_t li = l[i], lj = l[j];
            if (li == lj)
                continue;

            uint32_t pi = p[i], pj = p[j];

            for (int shift = 0; shift <= 16; shift += 8) {

                int dl = (int)((lj >> shift) & 0xFF) - (int)((li >> shift) & 0xFF);
                int adl = dl < 0 ? -dl : dl;
                if (adl <= best_delta)
                    continue;

                int dp = (int)((pj >> shift) & 0xFF) - (int)((pi >> shift) & 0xFF);

                /* Same sign requirement as the initial detection: a valid
                 * source-over blend has 255 - α >= 0 so dp and dl share a
                 * sign in every channel. */
                if ((dl < 0) != (dp < 0))
                    continue;

                best_delta = adl;
                best_dl = dl;
                best_dp = dp;
                best_i = i;
                best_j = j;

            }
        }
    }

    if (best_delta < GUAC_DISPLAY_PLAN_TRANSLUCENT_MERGE_MIN_DELTA)
        return 0;

    /* Derive merged α via round-to-nearest division. */
    int abs_dl = best_dl < 0 ? -best_dl : best_dl;
    int abs_dp = best_dp < 0 ? -best_dp : best_dp;

    int neg_alpha = (255 * abs_dp + abs_dl / 2) / abs_dl;
    if (neg_alpha > 255)
        return 0;
    int alpha = 255 - neg_alpha;
    if (alpha < 8)
        return 0;

    /* Derive merged src per channel via the source-over identity, using
     * both pixels of the most-distant pair (the same averaging trick as
     * the per-rect derivation, halving rounding-noise variance). */
    uint32_t pi = p[best_i], li = l[best_i];
    uint32_t pj = p[best_j], lj = l[best_j];

    int p_sum_r = (int)((pi >> 16) & 0xFF) + (int)((pj >> 16) & 0xFF);
    int p_sum_g = (int)((pi >>  8) & 0xFF) + (int)((pj >>  8) & 0xFF);
    int p_sum_b = (int)( pi        & 0xFF) + (int)( pj        & 0xFF);
    int l_sum_r = (int)((li >> 16) & 0xFF) + (int)((lj >> 16) & 0xFF);
    int l_sum_g = (int)((li >>  8) & 0xFF) + (int)((lj >>  8) & 0xFF);
    int l_sum_b = (int)( li        & 0xFF) + (int)( lj        & 0xFF);

    int two_alpha = 2 * alpha;
    int src_r = (255 * p_sum_r - neg_alpha * l_sum_r + alpha) / two_alpha;
    int src_g = (255 * p_sum_g - neg_alpha * l_sum_g + alpha) / two_alpha;
    int src_b = (255 * p_sum_b - neg_alpha * l_sum_b + alpha) / two_alpha;

    if (src_r < 0) src_r = 0; else if (src_r > 255) src_r = 255;
    if (src_g < 0) src_g = 0; else if (src_g > 255) src_g = 255;
    if (src_b < 0) src_b = 0; else if (src_b > 255) src_b = 255;

    /* Verify all four pooled samples satisfy the merged (α, src) within
     * tolerance. The verification is in scaled space (α·src vs.
     * 255·p - (255-α)·l) to mirror the per-pixel check used in the
     * initial detection. */
    int c_r = alpha * src_r;
    int c_g = alpha * src_g;
    int c_b = alpha * src_b;
    int scaled_tolerance = 384 + alpha;

    for (int k = 0; k < 4; k++) {

        int sr = 255 * (int)((p[k] >> 16) & 0xFF) - neg_alpha * (int)((l[k] >> 16) & 0xFF);
        int sg = 255 * (int)((p[k] >>  8) & 0xFF) - neg_alpha * (int)((l[k] >>  8) & 0xFF);
        int sb = 255 * (int)( p[k]        & 0xFF) - neg_alpha * (int)( l[k]        & 0xFF);

        int dr = sr - c_r; if (dr < 0) dr = -dr;
        int dg = sg - c_g; if (dg < 0) dg = -dg;
        int db = sb - c_b; if (db < 0) db = -db;

        if (dr > scaled_tolerance || dg > scaled_tolerance || db > scaled_tolerance)
            return 0;

    }

    /* Merge accepted - install the merged color and replace op_a's
     * stored samples with the most-distant pair from the merged set, so
     * subsequent merge attempts continue to work from the widest-spread
     * pair available. */
    op_a->src.rect.color     = ((uint32_t) alpha << 24)
                             | ((uint32_t) src_r << 16)
                             | ((uint32_t) src_g <<  8)
                             |  (uint32_t) src_b;
    op_a->src.rect.pending_a = pi;
    op_a->src.rect.last_a    = li;
    op_a->src.rect.pending_b = pj;
    op_a->src.rect.last_b    = lj;

    return 1;

}

/**
 * Combines the given pair of operations into a single operation if doing so is
 * advantageous (results in an operation of lesser or negligibly-worse cost).
 *
 * @param plan
 *     The plan containing op_a and op_b. Passed through to
 *     guac_display_plan_should_combine() so the worker-balance target
 *     can gate IMG+IMG combines.
 *
 * @param op_a
 *     The first of the pair of operations to be combined. If they operations
 *     are combined, the combined operation will be stored here.
 *
 * @param op_b
 *     The second of the pair of operations to be combined, which may
 *     potentially be identical to the first. If the operations are combined,
 *     this operation will be updated to be a GUAC_DISPLAY_PLAN_OPERATION_NOP
 *     operation.
 *
 * @return
 *     Non-zero if the operations were combined, zero otherwise.
 */
static int guac_display_plan_combine_if_improved(guac_display_plan* plan,
        guac_display_plan_operation* op_a,
        guac_display_plan_operation* op_b) {

    if (op_a == op_b)
        return 0;

    /* Special path: two adjacent translucent RECT ops (alpha < 0xFF in
     * each) may share a single (α, src) approximation that satisfies
     * both. The standard same-color check below would miss them since
     * each derives slightly different (α, src) from its own pixel data;
     * instead, pool their four sample pixels, derive a merged (α, src)
     * via the same system of equations used in the initial detection,
     * and accept the merge only if all four samples fall within
     * tolerance. This avoids fading-image regions getting fragmented
     * into many small per-cell rects when a few large ones would render
     * the same result. */
    if (op_a->type == GUAC_DISPLAY_PLAN_OPERATION_RECT
            && op_b->type == GUAC_DISPLAY_PLAN_OPERATION_RECT
            && op_a->layer == op_b->layer
            && (op_a->src.rect.color & 0xFF000000u) != 0xFF000000u
            && (op_b->src.rect.color & 0xFF000000u) != 0xFF000000u) {

        guac_rect combined = op_a->dest;
        guac_rect_extend(&combined, &op_b->dest);

        if (guac_display_plan_has_common_edge(op_a, op_b)
                && !guac_display_plan_rect_crosses_boundary(&combined)
                && try_merge_translucent_rects(op_a, op_b)) {

            guac_rect_extend(&op_a->dest, &op_b->dest);
            op_a->dirty_size += op_b->dirty_size;
            if (op_b->last_frame > op_a->last_frame)
                op_a->last_frame = op_b->last_frame;
            op_b->type = GUAC_DISPLAY_PLAN_OPERATION_NOP;
            return 1;

        }

    }

    /* Combine any adjacent operations that match the combination criteria
     * (combining produces a net lower cost) */
    if (guac_display_plan_should_combine(plan, op_a, op_b)) {

        guac_rect_extend(&op_a->dest, &op_b->dest);

        /* Operations of different types can only be combined as images */
        if (op_a->type != op_b->type)
            op_a->type = GUAC_DISPLAY_PLAN_OPERATION_IMG;

        /* When combining two copy operations, additionally combine their
         * source rects (NOT just the destination rects) */
        else if (op_a->type == GUAC_DISPLAY_PLAN_OPERATION_COPY)
            guac_rect_extend(&op_a->src.layer_rect.rect, &op_b->src.layer_rect.rect);

        op_a->dirty_size += op_b->dirty_size;

        if (op_b->last_frame > op_a->last_frame)
            op_a->last_frame = op_b->last_frame;

        op_b->type = GUAC_DISPLAY_PLAN_OPERATION_NOP;

        return 1;

    }

    return 0;

}

/**
 * Minimum IMG combine target area (in pixels). Combines producing an IMG
 * op of this area or less are always permitted by the worker-balance
 * check, regardless of total_img_area / worker_count. Set to two
 * 64x64 cells so the common case of combining a pair of adjacent
 * cells horizontally or vertically is never blocked by the balance
 * target even on tiny frames.
 */
#define GUAC_DISPLAY_PLAN_IMG_COMBINE_FLOOR_AREA \
    (2 * GUAC_DISPLAY_CELL_SIZE * GUAC_DISPLAY_CELL_SIZE)

/**
 * Computes the per-op IMG target area for the combine phase and stores
 * it on the plan. Must be called before the horizontal and vertical
 * combine passes, as both consult plan->img_combine_target_area to
 * gate IMG+IMG merges.
 *
 * The target is total_img_area / encode_worker_count, floored at
 * GUAC_DISPLAY_PLAN_IMG_COMBINE_FLOOR_AREA so the balance heuristic
 * never blocks combines of pairs of adjacent 64x64 cells (those tiny
 * combines pay for their protocol overhead several times over even
 * when parallelism is the priority). The 512x512 crosses-boundary
 * cap in guac_display_plan_rect_crosses_boundary() continues to
 * enforce the absolute per-op ceiling above the target.
 */
static void compute_img_combine_target(guac_display_plan* plan) {

    size_t total_img_area = 0;
    for (int i = 0; i < plan->length; i++) {
        if (plan->ops[i].type == GUAC_DISPLAY_PLAN_OPERATION_IMG) {
            total_img_area +=
                    (size_t) guac_rect_width(&plan->ops[i].dest)
                  * (size_t) guac_rect_height(&plan->ops[i].dest);
        }
    }

    int workers = plan->display->encode_stage.thread_count;
    if (workers < 1)
        workers = 1;

    size_t target = total_img_area / (size_t) workers;
    if (target < GUAC_DISPLAY_PLAN_IMG_COMBINE_FLOOR_AREA)
        target = GUAC_DISPLAY_PLAN_IMG_COMBINE_FLOOR_AREA;

    plan->img_combine_target_area = target;

}

void PFW_guac_display_plan_combine_horizontally(guac_display_plan* plan) {

    /* Compute the worker-balance target once before the horizontal and
     * vertical combine passes. Both passes read plan->img_combine_
     * target_area to gate IMG+IMG merges. */
    compute_img_combine_target(plan);

    guac_display* display = plan->display;
    guac_display_layer* current = display->pending_frame.layers;
    while (current != NULL) {

        /* Process only layers that have been modified */
        if (!guac_rect_is_empty(&current->pending_frame.dirty)) {

            /* Loop through all cells in left-to-right, top-to-bottom order,
             * combining any operations that are combinable and horizontally
             * adjacent. */

            guac_display_layer_cell* cell = current->pending_frame_cells;
            for (int y = 0; y < current->pending_frame_cells_height; y++) {

                guac_display_layer_cell* previous = cell++;
                for (int x = 1; x < current->pending_frame_cells_width; x++) {

                    /* Combine adjacent updates if doing so is advantageous */
                    if (previous->related_op != NULL && cell->related_op != NULL
                            && guac_display_plan_combine_if_improved(plan, previous->related_op, cell->related_op)) {
                        cell->related_op = previous->related_op;
                    }

                    previous++;
                    cell++;

                }
            }

        }

        current = current->pending_frame.next;

    }

}

void PFW_guac_display_plan_combine_vertically(guac_display_plan* plan) {

    guac_display* display = plan->display;
    guac_display_layer* current = display->pending_frame.layers;
    while (current != NULL) {

        /* Process only layers that have been modified */
        if (!guac_rect_is_empty(&current->pending_frame.dirty)) {

            /* Loop through all cells in top-to-bottom, left-to-right order,
             * combining any operations that are combinable and horizontally
             * adjacent. */

            guac_display_layer_cell* cell_col = current->pending_frame_cells;
            for (int x = 0; x < current->pending_frame_cells_width; x++) {

                guac_display_layer_cell* previous = cell_col;
                guac_display_layer_cell* cell = cell_col + current->pending_frame_cells_width;

                for (int y = 1; y < current->pending_frame_cells_height; y++) {

                    /* Combine adjacent updates if doing so is advantageous */
                    if (previous->related_op != NULL && cell->related_op != NULL
                            && guac_display_plan_has_common_edge(previous->related_op, cell->related_op)
                            && guac_display_plan_combine_if_improved(plan, previous->related_op, cell->related_op)) {
                        cell->related_op = previous->related_op;
                    }

                    previous += current->pending_frame_cells_width;
                    cell += current->pending_frame_cells_width;

                }

                cell_col++;

            }

        }

        current = current->pending_frame.next;

    }

}
