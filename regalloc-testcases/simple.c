
#include <assert.h>

int magic(int a0, int a1, int a2, int a3, int a4) {
    return a0*a4 + a1*a3 + a2*a2 + a3*a1 + a4*a0;
}

int main() {
    assert(magic(-27,-23,-26,-13,4) == 1058);
    return 0;
}

