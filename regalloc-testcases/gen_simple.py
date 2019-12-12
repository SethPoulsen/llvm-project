#!/usr/bin/env python3.8

import random

n = 5
arg_names = ['a' + str(i) for i in range(n)]
sum_expr = ' + '.join(
    f'{arg1}*{arg2}'
    for arg1, arg2 in zip(arg_names, reversed(arg_names))
)
arg_vals = [random.randint(-30, 30) for _ in arg_names]
sum_val = sum(
    arg1*arg2
    for arg1, arg2 in zip(arg_vals, reversed(arg_vals))
)

print(f'''
#include <assert.h>

int magic(int {', int '.join(arg_names)}) {{
    return {sum_expr};
}}

int main() {{
    for (unsigned long i = 0; i < 100000000L; ++i) {{
        assert(magic({','.join(map(str, arg_vals))}) == {sum_val});
    }}
    return 0;
}}
''')
# Run with:
# python3.8 gen_simple.py > simple.c && ./test_simple.sh rass simple.c && mv simple.s simple_rass.s && ./test_simple.sh greedy simple.c && mv simple.s simple_greedy.s && icdiff simple_rass.s simple_greedy.s
