/*
 *   Copyright (c) 2025-2026 Anton Kundenko <singaraiona@gmail.com>
 *   All rights reserved.
 *
 *   Permission is hereby granted, free of charge, to any person obtaining a copy
 *   of this software and associated documentation files (the "Software"), to deal
 *   in the Software without restriction, including without limitation the rights
 *   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *   copies of the Software, and to permit persons to whom the Software is
 *   furnished to do so, subject to the following conditions:
 *
 *   The above copyright notice and this permission notice shall be included in all
 *   copies or substantial portions of the Software.
 *
 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *   SOFTWARE.
 */

#include "ops.h"
#include <stdio.h>

/* Duplicate of find_ext() from opt.c — kept local for self-containment. */
static ray_op_ext_t* find_ext(ray_graph_t* g, uint32_t node_id) {
    for (uint32_t i = 0; i < g->ext_count; i++) {
        if (g->ext_nodes[i] && g->ext_nodes[i]->base.id == node_id)
            return g->ext_nodes[i];
    }
    return NULL;
}

const char* ray_opcode_name(uint16_t op) {
    switch (op) {
        case OP_SCAN:          return "SCAN";
        case OP_CONST:         return "CONST";
        case OP_NEG:           return "NEG";
        case OP_ABS:           return "ABS";
        case OP_NOT:           return "NOT";
        case OP_SQRT:          return "SQRT";
        case OP_LOG:           return "LOG";
        case OP_EXP:           return "EXP";
        case OP_CEIL:          return "CEIL";
        case OP_FLOOR:         return "FLOOR";
        case OP_ISNULL:        return "ISNULL";
        case OP_CAST:          return "CAST";
        case OP_ADD:           return "ADD";
        case OP_SUB:           return "SUB";
        case OP_MUL:           return "MUL";
        case OP_DIV:           return "DIV";
        case OP_MOD:           return "MOD";
        case OP_EQ:            return "EQ";
        case OP_NE:            return "NE";
        case OP_LT:            return "LT";
        case OP_LE:            return "LE";
        case OP_GT:            return "GT";
        case OP_GE:            return "GE";
        case OP_AND:           return "AND";
        case OP_OR:            return "OR";
        case OP_MIN2:          return "MIN2";
        case OP_MAX2:          return "MAX2";
        case OP_IF:            return "IF";
        case OP_LIKE:          return "LIKE";
        case OP_ILIKE:         return "ILIKE";
        case OP_UPPER:         return "UPPER";
        case OP_LOWER:         return "LOWER";
        case OP_STRLEN:        return "STRLEN";
        case OP_SUBSTR:        return "SUBSTR";
        case OP_REPLACE:       return "REPLACE";
        case OP_TRIM:          return "TRIM";
        case OP_CONCAT:        return "CONCAT";
        case OP_EXTRACT:       return "EXTRACT";
        case OP_DATE_TRUNC:    return "DATE_TRUNC";
        case OP_SUM:           return "SUM";
        case OP_PROD:          return "PROD";
        case OP_MIN:           return "MIN";
        case OP_MAX:           return "MAX";
        case OP_COUNT:         return "COUNT";
        case OP_AVG:           return "AVG";
        case OP_FIRST:         return "FIRST";
        case OP_LAST:          return "LAST";
        case OP_COUNT_DISTINCT:return "COUNT_DISTINCT";
        case OP_STDDEV:        return "STDDEV";
        case OP_STDDEV_POP:    return "STDDEV_POP";
        case OP_VAR:           return "VAR";
        case OP_VAR_POP:       return "VAR_POP";
        case OP_FILTER:        return "FILTER";
        case OP_SORT:          return "SORT";
        case OP_GROUP:         return "GROUP";
        case OP_PIVOT:         return "PIVOT";
        case OP_ANTIJOIN:      return "ANTIJOIN";
        case OP_JOIN:          return "JOIN";
        case OP_WINDOW_JOIN:   return "WINDOW_JOIN";
        case OP_SELECT:        return "SELECT";
        case OP_HEAD:          return "HEAD";
        case OP_TAIL:          return "TAIL";
        case OP_WINDOW:        return "WINDOW";
        case OP_ALIAS:         return "ALIAS";
        case OP_MATERIALIZE:   return "MATERIALIZE";
        case OP_EXPAND:        return "EXPAND";
        case OP_VAR_EXPAND:    return "VAR_EXPAND";
        case OP_SHORTEST_PATH: return "SHORTEST_PATH";
        case OP_WCO_JOIN:      return "WCO_JOIN";
        case OP_PAGERANK:      return "PAGERANK";
        case OP_CONNECTED_COMP: return "CONNECTED_COMP";
        case OP_DIJKSTRA:      return "DIJKSTRA";
        case OP_LOUVAIN:       return "LOUVAIN";
        case OP_DEGREE_CENT:   return "DEGREE_CENT";
        case OP_TOPSORT:       return "TOPSORT";
        case OP_DFS:           return "DFS";
        case OP_ASTAR:         return "ASTAR";
        case OP_K_SHORTEST:    return "K_SHORTEST";
        case OP_CLUSTER_COEFF: return "CLUSTER_COEFF";
        case OP_RANDOM_WALK:   return "RANDOM_WALK";
        case OP_COSINE_SIM:    return "COSINE_SIM";
        case OP_EUCLIDEAN_DIST:return "EUCLIDEAN_DIST";
        case OP_KNN:           return "KNN";
        case OP_HNSW_KNN:     return "HNSW_KNN";
        case OP_ANN_RERANK:    return "ANN_RERANK";
        case OP_KNN_RERANK:    return "KNN_RERANK";
        default:               return "UNKNOWN";
    }
}

static const char* type_name(int8_t t) {
    switch (t) {
        case RAY_LIST:      return "LIST";
        case RAY_BOOL:      return "BOOL";
        case RAY_U8:        return "U8";
        case RAY_I16:       return "I16";
        case RAY_I32:       return "I32";
        case RAY_I64:       return "I64";
        case RAY_F64:       return "F64";
        case RAY_DATE:      return "DATE";
        case RAY_TIME:      return "TIME";
        case RAY_TIMESTAMP: return "TIMESTAMP";
        case RAY_TABLE:     return "TABLE";
        case RAY_SEL:       return "SEL";
        case RAY_SYM:       return "SYM";
        default:           return "?";
    }
}

static void dump_node(FILE* f, ray_graph_t* g, ray_op_t* node, int depth) {
    if (!node) return;

    /* Indentation */
    for (int i = 0; i < depth; i++)
        fprintf(f, "  ");

    /* Opcode name */
    fprintf(f, "%s", ray_opcode_name(node->opcode));

    /* Find extended node for annotations */
    ray_op_ext_t* ext = find_ext(g, node->id);

    /* Annotations by opcode */
    switch (node->opcode) {
        case OP_SCAN:
            if (ext) {
                ray_t* s = ray_sym_str(ext->sym);
                if (s)
                    fprintf(f, "(%.*s)", (int)s->len, (char*)ray_data(s));
            }
            break;
        case OP_CONST:
            if (ext && ext->literal) {
                ray_t* lit = ext->literal;
                switch (lit->type) {
                    case RAY_I64:  fprintf(f, "(%lld)", (long long)lit->i64); break;
                    case RAY_F64:  fprintf(f, "(%.6g)", lit->f64); break;
                    case RAY_BOOL: fprintf(f, "(%s)", lit->i64 ? "true" : "false"); break;
                    case RAY_TABLE:fprintf(f, "(table)"); break;
                    default:      fprintf(f, "(?)"); break;
                }
            }
            break;
        case OP_JOIN:
            if (ext) {
                const char* jt = "INNER";
                if (ext->join.join_type == 1) jt = "LEFT";
                else if (ext->join.join_type == 2) jt = "FULL";
                fprintf(f, "(%s, keys=%u)", jt, ext->join.n_join_keys);
            }
            break;
        case OP_GROUP:
            if (ext)
                fprintf(f, "(keys=%u, aggs=%u)", ext->n_keys, ext->n_aggs);
            break;
        case OP_HEAD:
        case OP_TAIL:
            if (ext)
                fprintf(f, "(N=%lld)", (long long)ext->sym);
            break;
        default:
            break;
    }

    /* Output type */
    fprintf(f, " -> %s", type_name(node->out_type));

    /* Flags */
    if (node->flags & OP_FLAG_FUSED)
        fprintf(f, " [fused]");

    /* Estimated rows */
    if (node->est_rows > 0)
        fprintf(f, " ~%u rows", node->est_rows);

    /* Node ID */
    fprintf(f, " #%u", node->id);

    fprintf(f, "\n");

    /* Recurse into children */
    switch (node->opcode) {
        case OP_GROUP:
            if (ext) {
                /* keys */
                for (uint8_t i = 0; i < ext->n_keys; i++)
                    dump_node(f, g, ext->keys[i], depth + 1);
                /* agg inputs */
                for (uint8_t i = 0; i < ext->n_aggs; i++)
                    dump_node(f, g, ext->agg_ins[i], depth + 1);
            }
            /* Also recurse into standard inputs */
            for (uint8_t i = 0; i < node->arity && i < 2; i++)
                dump_node(f, g, node->inputs[i], depth + 1);
            break;
        case OP_SORT:
        case OP_SELECT:
            if (ext) {
                for (uint8_t i = 0; i < ext->sort.n_cols; i++)
                    dump_node(f, g, ext->sort.columns[i], depth + 1);
            }
            for (uint8_t i = 0; i < node->arity && i < 2; i++)
                dump_node(f, g, node->inputs[i], depth + 1);
            break;
        default:
            for (uint8_t i = 0; i < node->arity && i < 2; i++)
                dump_node(f, g, node->inputs[i], depth + 1);
            break;
    }
}

void ray_graph_dump(ray_graph_t* g, ray_op_t* root, void* out) {
    FILE* f = out ? (FILE*)out : stderr;
    fprintf(f, "=== Query Plan ===\n");
    dump_node(f, g, root, 0);
    fprintf(f, "==================\n");
}
