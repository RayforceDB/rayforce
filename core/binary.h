/*
 *   Copyright (c) 2023 Anton Kundenko <singaraiona@gmail.com>
 *   All rights reserved.

 *   Permission is hereby granted, free of charge, to any person obtaining a copy
 *   of this software and associated documentation files (the "Software"), to deal
 *   in the Software without restriction, including without limitation the rights
 *   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *   copies of the Software, and to permit persons to whom the Software is
 *   furnished to do so, subject to the following conditions:

 *   The above copyright notice and this permission notice shall be included in all
 *   copies or substantial portions of the Software.

 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *   SOFTWARE.
 */

#ifndef BINARY_H
#define BINARY_H

#include "rayforce.h"

rf_object_t rf_set_variable(rf_object_t *key, rf_object_t *val);
rf_object_t rf_dict(rf_object_t *x, rf_object_t *y);
rf_object_t rf_table(rf_object_t *x, rf_object_t *y);
rf_object_t rf_add_i64_i64(rf_object_t *x, rf_object_t *y);
rf_object_t rf_add_f64_f64(rf_object_t *x, rf_object_t *y);
rf_object_t rf_add_I64_i64(rf_object_t *x, rf_object_t *y);
rf_object_t rf_add_I64_I64(rf_object_t *x, rf_object_t *y);
rf_object_t rf_add_F64_f64(rf_object_t *x, rf_object_t *y);
rf_object_t rf_add_F64_F64(rf_object_t *x, rf_object_t *y);
rf_object_t rf_sub_i64_i64(rf_object_t *x, rf_object_t *y);
rf_object_t rf_sub_f64_f64(rf_object_t *x, rf_object_t *y);
rf_object_t rf_sub_I64_i64(rf_object_t *x, rf_object_t *y);
rf_object_t rf_sub_I64_I64(rf_object_t *x, rf_object_t *y);
rf_object_t rf_sub_F64_f64(rf_object_t *x, rf_object_t *y);
rf_object_t rf_sub_F64_F64(rf_object_t *x, rf_object_t *y);
rf_object_t rf_mul_i64_i64(rf_object_t *x, rf_object_t *y);
rf_object_t rf_mul_f64_f64(rf_object_t *x, rf_object_t *y);
rf_object_t rf_mul_I64_i64(rf_object_t *x, rf_object_t *y);
rf_object_t rf_mul_I64_I64(rf_object_t *x, rf_object_t *y);
rf_object_t rf_mul_F64_f64(rf_object_t *x, rf_object_t *y);
rf_object_t rf_mul_F64_F64(rf_object_t *x, rf_object_t *y);
rf_object_t rf_div_i64_i64(rf_object_t *x, rf_object_t *y);
rf_object_t rf_div_I64_i64(rf_object_t *x, rf_object_t *y);
rf_object_t rf_div_I64_I64(rf_object_t *x, rf_object_t *y);
rf_object_t rf_div_f64_f64(rf_object_t *x, rf_object_t *y);
rf_object_t rf_div_F64_f64(rf_object_t *x, rf_object_t *y);
rf_object_t rf_div_F64_F64(rf_object_t *x, rf_object_t *y);
rf_object_t rf_mod_i64_i64(rf_object_t *x, rf_object_t *y);
rf_object_t rf_mod_I64_i64(rf_object_t *x, rf_object_t *y);
rf_object_t rf_mod_I64_I64(rf_object_t *x, rf_object_t *y);
rf_object_t rf_mod_f64_f64(rf_object_t *x, rf_object_t *y);
rf_object_t rf_mod_F64_f64(rf_object_t *x, rf_object_t *y);
rf_object_t rf_mod_F64_F64(rf_object_t *x, rf_object_t *y);
rf_object_t rf_fdiv_i64_i64(rf_object_t *x, rf_object_t *y);
rf_object_t rf_fdiv_f64_f64(rf_object_t *x, rf_object_t *y);
rf_object_t rf_fdiv_I64_i64(rf_object_t *x, rf_object_t *y);
rf_object_t rf_fdiv_I64_I64(rf_object_t *x, rf_object_t *y);
rf_object_t rf_fdiv_F64_f64(rf_object_t *x, rf_object_t *y);
rf_object_t rf_fdiv_F64_F64(rf_object_t *x, rf_object_t *y);
rf_object_t rf_like_Char_Char(rf_object_t *x, rf_object_t *y);
rf_object_t rf_eq_i64_i64(rf_object_t *x, rf_object_t *y);
rf_object_t rf_eq_I64_i64(rf_object_t *x, rf_object_t *y);
rf_object_t rf_eq_I64_I64(rf_object_t *x, rf_object_t *y);
rf_object_t rf_eq_f64_f64(rf_object_t *x, rf_object_t *y);
rf_object_t rf_eq_F64_f64(rf_object_t *x, rf_object_t *y);
rf_object_t rf_eq_F64_F64(rf_object_t *x, rf_object_t *y);
rf_object_t rf_eq_bool_bool(rf_object_t *x, rf_object_t *y);
rf_object_t rf_eq_symbol_symbol(rf_object_t *x, rf_object_t *y);
rf_object_t rf_eq_Symbol_symbol(rf_object_t *x, rf_object_t *y);
rf_object_t rf_eq_Symbol_Symbol(rf_object_t *x, rf_object_t *y);
rf_object_t rf_ne_i64_i64(rf_object_t *x, rf_object_t *y);
rf_object_t rf_ne_f64_f64(rf_object_t *x, rf_object_t *y);
rf_object_t rf_ne_I64_i64(rf_object_t *x, rf_object_t *y);
rf_object_t rf_ne_I64_I64(rf_object_t *x, rf_object_t *y);
rf_object_t rf_ne_F64_f64(rf_object_t *x, rf_object_t *y);
rf_object_t rf_ne_F64_F64(rf_object_t *x, rf_object_t *y);
rf_object_t rf_ne_Bool_Bool(rf_object_t *x, rf_object_t *y);
rf_object_t rf_lt_i64_i64(rf_object_t *x, rf_object_t *y);
rf_object_t rf_lt_f64_f64(rf_object_t *x, rf_object_t *y);
rf_object_t rf_lt_I64_i64(rf_object_t *x, rf_object_t *y);
rf_object_t rf_lt_I64_I64(rf_object_t *x, rf_object_t *y);
rf_object_t rf_lt_F64_f64(rf_object_t *x, rf_object_t *y);
rf_object_t rf_lt_F64_F64(rf_object_t *x, rf_object_t *y);
rf_object_t rf_gt_i64_i64(rf_object_t *x, rf_object_t *y);
rf_object_t rf_gt_f64_f64(rf_object_t *x, rf_object_t *y);
rf_object_t rf_gt_I64_i64(rf_object_t *x, rf_object_t *y);
rf_object_t rf_gt_I64_I64(rf_object_t *x, rf_object_t *y);
rf_object_t rf_gt_F64_f64(rf_object_t *x, rf_object_t *y);
rf_object_t rf_gt_F64_F64(rf_object_t *x, rf_object_t *y);
rf_object_t rf_le_i64_i64(rf_object_t *x, rf_object_t *y);
rf_object_t rf_le_f64_f64(rf_object_t *x, rf_object_t *y);
rf_object_t rf_le_I64_i64(rf_object_t *x, rf_object_t *y);
rf_object_t rf_le_I64_I64(rf_object_t *x, rf_object_t *y);
rf_object_t rf_le_F64_f64(rf_object_t *x, rf_object_t *y);
rf_object_t rf_le_F64_F64(rf_object_t *x, rf_object_t *y);
rf_object_t rf_ge_i64_i64(rf_object_t *x, rf_object_t *y);
rf_object_t rf_ge_f64_f64(rf_object_t *x, rf_object_t *y);
rf_object_t rf_ge_I64_i64(rf_object_t *x, rf_object_t *y);
rf_object_t rf_ge_I64_I64(rf_object_t *x, rf_object_t *y);
rf_object_t rf_ge_F64_f64(rf_object_t *x, rf_object_t *y);
rf_object_t rf_ge_F64_F64(rf_object_t *x, rf_object_t *y);
rf_object_t rf_and_bool_bool(rf_object_t *x, rf_object_t *y);
rf_object_t rf_or_bool_bool(rf_object_t *x, rf_object_t *y);
rf_object_t rf_and_Bool_Bool(rf_object_t *x, rf_object_t *y);
rf_object_t rf_or_Bool_Bool(rf_object_t *x, rf_object_t *y);
rf_object_t rf_get_Bool_i64(rf_object_t *x, rf_object_t *y);
rf_object_t rf_get_Bool_I64(rf_object_t *x, rf_object_t *y);
rf_object_t rf_get_I64_i64(rf_object_t *x, rf_object_t *y);
rf_object_t rf_get_I64_I64(rf_object_t *x, rf_object_t *y);
rf_object_t rf_get_Timestamp_i64(rf_object_t *x, rf_object_t *y);
rf_object_t rf_get_Timestamp_I64(rf_object_t *x, rf_object_t *y);
rf_object_t rf_get_Char_i64(rf_object_t *x, rf_object_t *y);
rf_object_t rf_get_Char_I64(rf_object_t *x, rf_object_t *y);
rf_object_t rf_get_F64_i64(rf_object_t *x, rf_object_t *y);
rf_object_t rf_get_F64_I64(rf_object_t *x, rf_object_t *y);
rf_object_t rf_get_List_i64(rf_object_t *x, rf_object_t *y);
rf_object_t rf_get_List_I64(rf_object_t *x, rf_object_t *y);
rf_object_t rf_get_Guid_i64(rf_object_t *x, rf_object_t *y);
rf_object_t rf_get_Guid_I64(rf_object_t *x, rf_object_t *y);
rf_object_t rf_get_Table_symbol(rf_object_t *x, rf_object_t *y);
rf_object_t rf_find_I64_i64(rf_object_t *x, rf_object_t *y);
rf_object_t rf_find_I64_I64(rf_object_t *x, rf_object_t *y);
rf_object_t rf_find_Char_char(rf_object_t *x, rf_object_t *y);
rf_object_t rf_find_Char_Char(rf_object_t *x, rf_object_t *y);
rf_object_t rf_find_F64_f64(rf_object_t *x, rf_object_t *y);
rf_object_t rf_find_F64_F64(rf_object_t *x, rf_object_t *y);
rf_object_t rf_concat_bool_bool(rf_object_t *x, rf_object_t *y);
rf_object_t rf_concat_Bool_bool(rf_object_t *x, rf_object_t *y);
rf_object_t rf_concat_Bool_Bool(rf_object_t *x, rf_object_t *y);
rf_object_t rf_concat_bool_Bool(rf_object_t *x, rf_object_t *y);
rf_object_t rf_concat_i64_i64(rf_object_t *x, rf_object_t *y);
rf_object_t rf_concat_I64_i64(rf_object_t *x, rf_object_t *y);
rf_object_t rf_concat_I64_I64(rf_object_t *x, rf_object_t *y);
rf_object_t rf_concat_i64_I64(rf_object_t *x, rf_object_t *y);
rf_object_t rf_concat_f64_f64(rf_object_t *x, rf_object_t *y);
rf_object_t rf_concat_F64_f64(rf_object_t *x, rf_object_t *y);
rf_object_t rf_concat_F64_F64(rf_object_t *x, rf_object_t *y);
rf_object_t rf_concat_f64_F64(rf_object_t *x, rf_object_t *y);
rf_object_t rf_concat_char_char(rf_object_t *x, rf_object_t *y);
rf_object_t rf_concat_Char_char(rf_object_t *x, rf_object_t *y);
rf_object_t rf_concat_Char_Char(rf_object_t *x, rf_object_t *y);
rf_object_t rf_concat_char_Char(rf_object_t *x, rf_object_t *y);
rf_object_t rf_concat_List_List(rf_object_t *x, rf_object_t *y);
rf_object_t rf_filter_I64_Bool(rf_object_t *x, rf_object_t *y);
rf_object_t rf_filter_Timestamp_Bool(rf_object_t *x, rf_object_t *y);
rf_object_t rf_filter_Guid_Bool(rf_object_t *x, rf_object_t *y);
rf_object_t rf_filter_F64_Bool(rf_object_t *x, rf_object_t *y);
rf_object_t rf_filter_Char_Bool(rf_object_t *x, rf_object_t *y);
rf_object_t rf_filter_Table_Bool(rf_object_t *x, rf_object_t *y);
rf_object_t rf_take_i64_i64(rf_object_t *x, rf_object_t *y);

#endif
