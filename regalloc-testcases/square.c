#include <math.h>
#include <stdio.h>

float square(float a) {
    float square = a * a;
    return square;
}

int main() {
    int x = square(3);
    return 0;
}
