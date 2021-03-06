
/* ----------------------------------------------------------------
 *
 * BabyC Toy compiler for educational purposes
 *
 * ---------------------------------------------------------------- */

#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "syntax.h"
#include "log_error.h"
#include "list.h"

/* helper functions to handle uint64_t overflows and intermediate values similar
 * to x86_initrin.h */
static inline char addcarry_uint64(char carry_in, uint64_t a, uint64_t b,
                                   uint64_t *r) {
    uint64_t t = a + b;
    char carry_out = (t < a);
    uint64_t s = t + carry_in;
    carry_out |= (s < t);
    *r = s;
    return carry_out;
}

static inline char subborrow_uint64(char borrow_in, uint64_t a, uint64_t b,
                                    uint64_t *r) {
    uint64_t t = b - a;
    char borrow_out = (t > b);
    uint64_t s = t - borrow_in;
    borrow_out |= (s > t);
    *r = s;
    return borrow_out;
}

static inline uint64_t mul_uint64(uint64_t a, uint64_t b, uint64_t *hi) {
    uint32_t a_lo = a;
    uint32_t a_hi = a >> 32;
    uint32_t b_lo = b;
    uint32_t b_hi = b >> 32;
    uint64_t lo = (uint64_t)a_lo * b_lo;
    uint64_t t2 = (uint64_t)a_lo * b_hi;
    uint64_t t3 = (uint64_t)a_hi * b_lo;
    uint64_t t4 = (uint64_t)a_hi * b_hi;

    t4 += addcarry_uint64(0, lo, t2 << 32, &lo);
    t4 += addcarry_uint64(0, lo, t3 << 32, &lo);
    t4 += t2 >> 32;
    t4 += t3 >> 32;
    *hi = t4;
    return lo;
}

static inline uint64_t div10_uint64(uint64_t hi, uint64_t lo, uint64_t *mod) {
    uint64_t t, d, r;

    t = hi;
    mul_uint64(t, 0xcccccccccccccccdull, &d);
    d >>= 3;
    r = t - d * 10;
    t = (r << 32) + (lo >> 32);
    mul_uint64(t, 0xcccccccccccccccdull, &d);
    d >>= 3;
    r = t - d * 10;
    t = (r << 32) + (lo & 0xffffffff);
    mul_uint64(t, 0xcccccccccccccccdull, &d);
    d >>= 3;
    *mod = t - d * 10;
    return d;
}

/* ----------------------------------------------------------------
 * Integer immediate in AST tree
 * ---------------------------------------------------------------- */

void ast_set_object_type(AstInteger *p) {
    if (p->val[1]) {
        p->objectType = O_UINT128;
    } else if (p->val[0] & 0xffffffff00000000ull) {
        p->objectType = O_UINT64;
    } else if (p->val[0] & 0xffff0000ull) {
        p->objectType = O_UINT32;
    } else if (p->val[0] & 0xff00ull) {
        p->objectType = O_UINT16;
        p->objectType = O_UINT32;
    } else {
        p->objectType = O_UINT8;
        p->objectType = O_UINT32;
    }
}

/* convert string to uint128_t, detect overflows */
void ast_integer_set_str(AstInteger *p, char *str, int radix) {
    char c, *s = str;
    char upper_limit = radix > 10 ? 'A' + radix - 10 : 'A';
    char lower_limit = radix > 10 ? 'a' + radix - 10 : 'a';
    char digit_limit = radix > 10 ? '0' + 10 : '0' + radix;
    uint64_t mask = (1ull << 56) - 1;
    uint64_t val0 = 0, val1 = 0, val2 = 0;

    while ((c = *s++) != 0) {
        /* stop at invalid characters (as does strtoul() */
        if (!((c >= 'a' && c < lower_limit) || (c >= 'A' && c < upper_limit) ||
              (c >= '0' && c < digit_limit)))
            break;

        val0 *= radix;
        val1 *= radix;
        val2 *= radix;

        if (c >= 'a')
            val0 += c - 'a' + 10;
        else if (c >= 'A')
            val0 += c - 'A' + 10;
        else if (c >= '0')
            val0 += c - '0';

        if (val0 > mask) {
            val1 += val0 >> 56;
            val0 &= mask;
        }
        if (val1 > mask) {
            val2 += val1 >> 56;
            val1 &= mask;
        }
        if (val2 > mask) {
            log_error("integer overflow : '%s' too large", str);
        }
    }
    p->val[0] = val0 | (val1 << 56);
    p->val[1] = (val1 >> 8) | (val2 << 48);
    if (val2 >> 16) {
        log_error("integer overflow : '%s' too large", str);
    }
    ast_set_object_type(p);
}

/* convert uint128_t to string */
char *ast_integer_get_str(AstInteger *p, char *str, int len) {
    uint64_t val0 = p->val[0];
    uint64_t val1 = p->val[1];
    char *s = str + len - 1;
    *--s = 0;

    do {
        uint64_t mod = 0;
        val1 = div10_uint64(mod, val1, &mod);
        val0 = div10_uint64(mod, val0, &mod);
        *--s = mod + '0';
    } while (val1 | val0);
    return s;
}

void ast_integer_set_int(AstInteger *p, int i) {
    if (i < 0) {
        i = -i;
        p->val[0] = 0 - i;
        p->val[1] = (uint64_t)-1;
    } else {
        p->val[0] = i;
        p->val[1] = 0;
    }
    ast_set_object_type(p);
}

void ast_integer_set_bool(AstInteger *p, bool b) {
    p->val[0] = b ? 1 : 0;
    p->val[1] = 0;
    p->objectType = O_UINT8; // TODO O_BOOL ?
}

bool ast_integer_is_zero(AstInteger *p) {
    return (p->val[0] == 0 && p->val[1] == 0);
}

bool ast_integer_is_one(AstInteger *p) {
    return (p->val[0] == 1 && p->val[1] == 0);
}

int ast_integer_get_int(AstInteger *p) {
    int ret = p->val[0] & 0xffffffff;
    return ret;
}

unsigned ast_integer_get_uint(AstInteger *p) {
    unsigned ret = p->val[0] & 0xffffffff;
    return ret;
}

uint64_t ast_integer_get_unsigned_long_long(AstInteger *p) {
    uint64_t ret = p->val[0];
    return ret;
}

/* arithmetic on uint128_t (res = left binary_op right) */
void ast_integer_binary_operation(AstInteger *res, AstInteger *left,
                                  AstInteger *right,
                                  BinaryExpressionType binary_type) {
    if (binary_type == MULTIPLICATION) {
        if (left->val[1] | right->val[1]) {
            // TODO
            assert(0);
        }
        res->val[0] = mul_uint64(left->val[0], right->val[0], &res->val[1]);
    } else if (binary_type == DIVISION) {
        if (left->val[1] | right->val[1]) {
            log_error("TODO intermediate numbers too large in division!\n");
        }
        if (right->val[0] == 0)
            log_error("Divide by zero !\n");
        res->val[0] = left->val[0] / right->val[0];
        res->val[1] = 0;
    } else if (binary_type == MODULUS) {
        if (left->val[1] | right->val[1]) {
            log_error("TODO intermediate numbers too large in modulus!\n");
        }
        if (right->val[0] == 0)
            log_error("Modulus is zero !\n");
        res->val[0] = left->val[0] % right->val[0];
        res->val[1] = 0;
    } else if (binary_type == OR) {
        res->val[0] = left->val[0] | right->val[0];
        res->val[1] = left->val[1] | right->val[1];
    } else if (binary_type == AND) {
        res->val[0] = left->val[0] & right->val[0];
        res->val[1] = left->val[1] & right->val[1];
    } else if (binary_type == XOR) {
        res->val[0] = left->val[0] ^ right->val[0];
        res->val[1] = left->val[1] ^ right->val[1];
    } else if (binary_type == RSHIFT) {
        int s = right->val[0];
        if (right->val[1] || s >= 128) {
            res->val[0] = 0;
            res->val[1] = 0;
        } else if (s >= 64) {
            s -= 64;
            res->val[0] = left->val[1] >> s;
            res->val[1] = 0;
        } else {
            res->val[0] = (left->val[0] >> s) | (res->val[1] << (64 - s));
            res->val[1] = (left->val[1] >> s);
        }
    } else if (binary_type == LSHIFT) {
        int s = right->val[0];
        if (right->val[1] || s >= 128) {
            res->val[0] = 0;
            res->val[1] = 0;
        } else if (right->val[0] >= 64) {
            s -= 64;
            res->val[1] = left->val[0] << s;
            res->val[0] = 0;
        } else {
            res->val[1] = (left->val[1] << s) | (left->val[0] >> (64 - s));
            res->val[0] = (left->val[0] << s);
        }
    } else if (binary_type == ADDITION) {
        char c = addcarry_uint64(0, left->val[0], right->val[0], &res->val[0]);

        addcarry_uint64(c, left->val[1], right->val[1], &res->val[1]);
    } else if (binary_type == SUBTRACTION) {
        char b = subborrow_uint64(0, right->val[0], left->val[0], &res->val[0]);

        subborrow_uint64(b, right->val[1], left->val[1], &res->val[1]);
    } else if (binary_type == LESS_THAN || binary_type == LARGER_THAN ||
               binary_type == LESS_THAN_OR_EQUAL ||
               binary_type == LARGER_THAN_OR_EQUAL || binary_type == EQUAL ||
               binary_type == NEQUAL) {
        uint64_t t0, t1;
        char b = subborrow_uint64(0, right->val[0], left->val[0], &t0);
        b = subborrow_uint64(b, right->val[1], left->val[1], &t1);

        bool c = (b != 0);
        bool z = ((t0 | t1) == 0);
        bool t;

        switch (binary_type) {
        case LESS_THAN:
            t = c;
            break;
        case LARGER_THAN:
            t = !c && !z;
            break;
        case LESS_THAN_OR_EQUAL:
            t = c || z;
            break;
        case LARGER_THAN_OR_EQUAL:
            t = !c;
            break;
        case EQUAL:
            t = !c && z;
            break;
        case NEQUAL:
            t = c || !z;
            break;
        default:
            t = false;
        }
        res->val[0] = t;
        res->val[1] = 0;
    } else {
        log_error("Invalid binary type %d\n", binary_type);
    }
    ast_set_object_type(res);
}

void ast_integer_unary_operation(AstInteger *res, AstInteger *b,
                                 UnaryExpressionType unary_type) {
    if (unary_type == CAST) {
    } else if (unary_type == BITWISE_NEGATION) {
        res->val[0] = ~b->val[0];
        res->val[1] = ~b->val[1];
    } else if (unary_type == ARITHMETIC_NEGATION) {
        char c = subborrow_uint64(0, b->val[0], 0, &res->val[0]);
        subborrow_uint64(c, b->val[1], 0, &res->val[1]);
    } else if (unary_type == LOGICAL_NEGATION) {
        res->val[0] = ast_integer_is_zero(b);
        res->val[1] = 0;
    } else {
        log_error("Invalid unary type %d\n", unary_type);
    }
    ast_set_object_type(res);
}
