#include <math.h>
#include <stdio.h>

float quadratic(float a, float b, float c) {

    float denom = (2 * a);

    float num = -b + sqrt(pow(b, 2) - (4 * a * c) );

    float x = num / denom;

    return x;
}

int main() {
    int x = quadratic(3,4,5);
    return 0;
}
